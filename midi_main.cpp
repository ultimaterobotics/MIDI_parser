#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

#define SEND_NOTE_OFF		0b00000001
#define SEND_NOTE_ON		0b00000010
#define SEND_AFTERTOUCH		0b00000100
#define SEND_CTRL_CHANGE	0b00001000
#define SEND_PROG_CHANGE	0b00010000
#define SEND_CHAN_KEYPRES	0b00100000
#define SEND_PITCH_BEND		0b01000000

enum e_event_types
{
	evt_note_off = 0,
	evt_note_on,
	evt_aftertouch,
	evt_ctrl_change,
	evt_prog_change,
	evt_chan_keypress,
	evt_pitch_bend
};

//#include <string.h>
int str_eq(const char *t1, const char *t2)
{
	int x = 0;
	if(t1 == NULL && t2 == NULL) return 1;
	if(t1 == NULL || t2 == NULL) return 0;
	while(t1[x] != 0 && t2[x] != 0)
	{
		if(t1[x] != t2[x]) return 0;
		++x;
	}
	if(t1[x] != t2[x]) return 0;
	return 1;
}

int parse_vbl(uint8_t *buf, uint32_t *res)
{
	uint32_t val = 0;
	int pp = 0;
	while(buf[pp] >= 128)
	{
		val |= buf[pp] & 0x7F;
		val <<= 7;
		pp++;
	}
	val += buf[pp];
	*res = val;
	return pp+1;
}

float ticks_to_ms = 1.0;
uint32_t ticks_per_qn = 1000;
uint32_t micros_per_qn = 800000; 


#define MAX_TEMPO_POINTS 10000
int tempo_points = 0;
uint32_t tempo_ms[MAX_TEMPO_POINTS];
uint32_t tempo_value[MAX_TEMPO_POINTS];

typedef struct sMIDI_event
{
	uint32_t T;
	uint8_t type;
	uint8_t track;
	uint8_t channel;
	uint8_t key;
	int value; //stores also pitch bend values
	void set_to(sMIDI_event e)
	{
		T = e.T;
		type = e.type;
		track = e.track;
		channel = e.channel;
		key = e.key;
		value = e.value;
	};
}sMIDI_event;

sMIDI_event *events;
int events_count = 0;
int events_size = 0;
int event_count_memstep = 100;

void add_event(sMIDI_event evt)
{
	if(events_count >= events_size)
	{
		events_size += event_count_memstep;
		sMIDI_event *ee = new sMIDI_event[events_size];
		for(int x = 0; x < events_count; x++)
			ee[x].set_to(events[x]);
		delete events;
		events = ee;
	}
	events[events_count].set_to(evt);
	events_count++;
}

void sort_events()
{
	for(int n = 0; n < events_count; n++)
	{
		for(int n2 = n+1; n2 < events_count; n2++)
		{
			if(events[n2].T < events[n].T)
			{
				sMIDI_event ee;
				ee.set_to(events[n]);
				events[n].set_to(events[n2]);
				events[n2].set_to(ee);
			}
		}
	}
}

void save_events(char *fname, uint32_t channel_mask)
{
	int handle = open(fname, O_WRONLY | O_CREAT, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH);
	
	if(handle < 1)
	{
		fprintf(stderr, "can't open/create output file %s\n", fname);
		return;
	}
	
	char tbuf[1024];
	for(int x = 0; x < events_count; x++)
	{
		if(!((1<<events[x].channel) & channel_mask)) continue;
		
		int len = sprintf(tbuf, "%d,%d,%d,%d,%d,%d\n", events[x].T, events[x].track, events[x].channel, events[x].type, events[x].key, events[x].value);
		if(write(handle, tbuf, len) < len)
			fprintf(stderr, "write %d bytes failed\n", len);

	}
	close(handle);
}

int tempo_fixed = 0;

void add_tempo_point(uint32_t ms, uint32_t tempo)
{
	tempo_ms[tempo_points] = ms;
	tempo_value[tempo_points] = tempo;
	if(tempo_points <= MAX_TEMPO_POINTS)
		tempo_points++;
}

uint32_t get_tempo(uint32_t ms)
{
	for(int x = 0; x < tempo_points; x++)
	{
		if(ms >= tempo_ms[x]) return tempo_value[x];
	}
	return 500000; //reasonable default
}

int key_map(int key)
{
	return key;
}
int value_map_on(int value)
{
	return value;
//	return TARGET_MIN + value * (TARGET_MAX-TARGET_MIN) / 127.0;
}
int value_map_off(int value)
{
	return 0;
}

void parse_track(uint8_t *buf, int length, int out_process, int track_num)
{
	int pos = 0;
	uint32_t T = 0;
	int unhandled_sum = 0;
	int out_verbose = 0;//out_process;
	int send_out = out_process;
	if(send_out)
	{
//		printf("ser.write('<0,3,0,0>')\n"); //track start event
		printf("0,3,0,0\n"); //track start event
	}
	while(pos < length)
	{
		uint32_t dt;
		pos += parse_vbl(buf+pos, &dt);
		uint8_t type = buf[pos]>>4;
		uint8_t channel = buf[pos]&0x0F;
		uint8_t b1 = buf[pos+1];
		uint8_t b2 = buf[pos+2];
		int handled = 0;
		if(!tempo_fixed)
		{
			uint32_t cur_tempo = get_tempo(T);
			ticks_to_ms = (float)(cur_tempo / 1000) / (float)(ticks_per_qn);
		}
		T += dt * ticks_to_ms;
		if(type >= 0x8 && type < 0xF)
		{
			sMIDI_event evt;
			evt.T = T;
			evt.channel = channel;
			evt.track = track_num;
			if(type == 8)
			{
				if(send_out & SEND_NOTE_OFF)
				{
					evt.type = evt_note_off;
					evt.key = b1;
					evt.value = b2;
					add_event(evt);
				}
				pos += 3;
				handled = 1;
			}
			if(type == 9)
			{
				if(send_out & SEND_NOTE_ON)
				{
					evt.type = evt_note_on;
					evt.key = b1;
					evt.value = b2;
					add_event(evt);
				}
				pos += 3;
				handled = 1;
			}
			if(type == 0xA)
			{
				if(send_out & SEND_AFTERTOUCH)
				{
					evt.type = evt_aftertouch;
					evt.key = b1;
					evt.value = b2;
					add_event(evt);
				}
				if(out_verbose) printf("(%d) ch %d aft %d, v %d\n", T, channel, b1, b2);
				pos += 3;
				handled = 1;
			}
			if(type == 0xB)
			{
				if(send_out & SEND_CTRL_CHANGE)
				{
					evt.type = evt_ctrl_change;
					evt.key = b1;
					evt.value = b2;
					add_event(evt);
				}
				if(out_verbose) printf("(%d) ch %d cc %d, cv %d\n", T, channel, b1, b2);
				pos += 3;
				handled = 1;
			}
			if(type == 0xC)
			{
				if(send_out & SEND_PROG_CHANGE)
				{
					evt.type = evt_prog_change;
					evt.key = 255;
					evt.value = b1;
					add_event(evt);
				}
				if(out_verbose) printf("(%d) ch %d prog %d\n", T, channel, b1);
				pos += 2;
				handled = 1;
			}
			if(type == 0xD)
			{
				if(send_out & SEND_CHAN_KEYPRES)
				{
					evt.type = evt_chan_keypress;
					evt.key = 255;
					evt.value = b1;
					add_event(evt);
				}
				
				if(out_verbose) printf("(%d) ch %d AFT %d\n", T, channel, b1);
				pos += 2;
				handled = 1;
			}
			if(type == 0xE)
			{
				if(send_out & SEND_PITCH_BEND)
				{
					evt.type = evt_pitch_bend;
					evt.key = 255;
					evt.value = (b2<<8) + b1;
					add_event(evt);
				}
				if(out_verbose) printf("(%d) ch %d pitch %d\n", T, channel, (b2<<8) + b1);
				pos += 3;
				handled = 1;
			}
		}
		if(type == 0xF)
		{
			if(channel == 0)
			{
				if(out_verbose) printf("(%d) sysex F0\n", T);
				handled = 1;
				pos += b1;
			}
			if(channel == 7)
			{ 
				if(out_verbose) printf("(%d) sysex F7\n", T);
				handled = 1;
				pos += b1;
			}
			if(channel == 0xF) //meta event
			{
				if(b1 == 0)
				{
					if(out_verbose) printf("(%d) meta 0\n", T); 
					handled = 1; 
					pos += 5;
				}
				if(b1 == 1)
				{
					if(out_verbose) printf("(%d) meta text\n", T); 
					handled = 1; 
					pos += 3+b2;
				}
				if(b1 == 2)
				{
					if(out_verbose) printf("(%d) meta copyright\n", T);
					handled = 1; 
					pos += 3+b2;
				}
				if(b1 == 3)
				{ 
					if(out_verbose) printf("(%d) meta Track Name\n", T); 
					handled = 1; 
					pos += 3+b2;
				}
				if(b1 == 4) 
				{
					if(out_verbose) printf("(%d) meta Instrument Name\n", T); 
					handled = 1; 
					pos += 3+b2;
				}
				if(b1 == 5)
				{
					if(out_verbose) printf("(%d) meta Lyrics\n", T); 
					handled = 1; 
					pos += 3+b2;
				}
				if(b1 == 6) 
				{
					if(out_verbose) printf("(%d) meta Marker\n", T); 
					handled = 1; 
					pos += 3+b2;
				}
				if(b1 == 7) 
				{
					if(out_verbose) printf("(%d) meta Cue Point\n", T); 
					handled = 1; 
					pos += 3+b2;
				}
				if(b1 == 0x20) 
				{
					if(out_verbose) printf("(%d) meta channel prefix\n", T); 
					handled = 1; 
					pos += 4;
				}
				if(b1 == 0x2F) 
				{
					if(out_verbose) printf("(%d) meta track end\n", T); 
					if(send_out)
					{
//						printf("ser.write('<%d,4,0,0>')\n", T); //track end event
						printf("%d,4,0,0\n", T); //track end event
					}
					handled = 1; 
					pos += 3;
				}
				if(b1 == 0x51)
				{
					uint32_t mpqn = (buf[pos+3]<<16)|(buf[pos+4]<<8)|buf[pos+5];
					if(out_verbose) printf("(%d) meta tempo %d\n", T, mpqn);
					add_tempo_point(T, mpqn);
					handled = 1; 
					pos += 6;
				}
				if(b1 == 0x54) 
				{
					if(out_verbose) printf("(%d) meta SMTPE offset\n", T); 
					handled = 1; 
					pos += 8;
				}
				if(b1 == 0x58) 
				{
					if(out_verbose) printf("(%d) meta Time Signature\n", T); 
					handled = 1; 
					pos += 7;
				}
				if(b1 == 0x59) 
				{
					if(out_verbose) printf("(%d) meta Key Signature\n", T); 
					handled = 1; 
					pos += 5;
				}
				if(b1 == 0x7F) 
				{
					if(out_verbose) printf("(%d) meta Sequencer-Specific\n", T); 
					handled = 1; 
					pos += 2+b2;
				}
				if(!handled) 
				{
					if(out_verbose) fprintf(stderr, "unhandled meta: %02X %02X %02X %02X %02X %02X %02X %02X\n", b1, b2, buf[pos+3], buf[pos+4], buf[pos+5], buf[pos+6], buf[pos+7], buf[pos+8]); 
					pos += 2+b2;
				}
			}
			
		}
		if(!handled)
		{ 
			if(out_verbose) fprintf(stderr, "(%d) unhandled %02X %02X %02X %02X %02X %02X %02X %02X\n", T, buf[pos-2], buf[pos-1], buf[pos], buf[pos+1], buf[pos+2], buf[pos+3], buf[pos+4], buf[pos+5]); 
			unhandled_sum++;
		}
	}
	fprintf(stderr, "unhandled messages: %d\n", unhandled_sum);
}

void parse_midi(uint8_t *buf, int length, int send_out)
{
	int pos = 0;
	uint8_t type[5];
	type[4] = 0;
	int cur_track = 0;
	while(pos < length)
	{
		uint32_t len;
		for(int x = 0; x < 4; x++)
			type[x] = buf[pos + x];
		len = buf[pos+4]; len <<= 8;
		len += buf[pos+5]; len <<= 8;
		len += buf[pos+6]; len <<= 8;
		len += buf[pos+7];
		fprintf(stderr, "%s: %d\n", type, len);
		if(str_eq((char*)type, "MThd"))
		{
			int format = (buf[pos+8]<<8) | buf[pos+8+1];
			int tracks = (buf[pos+8+2]<<8) | buf[pos+8+3];
			int tpqn_type = !(buf[pos+8+4] > 0x7F);
			int tpqn = ((buf[pos+8+4]&0x7F)<<8) | buf[pos+8+5];
			int fps = buf[pos+8+4]&0x7F;
			int tpf = buf[pos+8+5];

			if(tpqn_type)
			{
				fprintf(stderr, "MIDI format %d, tracks %d, tpqn %d\n", format, tracks, tpqn);
				ticks_per_qn = tpqn;
				tempo_fixed = 0;
			}
			else
			{
				fprintf(stderr, "MIDI format %d, tracks %d, fps %d, tpf %d\n", format, tracks, fps, tpf);
				ticks_to_ms = (float)(tpf * fps) / 1000.0;
				tempo_fixed = 1;
			}
		}
		if(str_eq((char*)type, "MTrk"))
		{ 
			parse_track(buf + pos + 8, len, send_out, cur_track);
			cur_track++;
		}
		pos += 8 + len;
		
	}
}

uint8_t *file_buf;
int file_length = 0;

void read_file(const char *fname)
{
	int handle = open(fname, O_RDONLY);
	if(handle < 1)
	{
		fprintf(stderr, "can't open file!\n");
		return;
	}
	lseek(handle, 0, 0);
	file_length = lseek(handle, 0, 2);
	lseek(handle, 0, 0);

	file_buf = new uint8_t[file_length];
	if(read(handle, file_buf, file_length) != file_length)
	{
		fprintf(stderr, "file reading error\n");
	}
	close(handle);
}

int main(int argc, char **argv)
{
	if(argc < 3)
	{
		printf("\nMIDI file parser v1.0\nusage: midi_parser <input filename> <output filename>\n");
		return 1;
	}
//	int export_track = atoi(argv[2]);
	read_file(argv[argc-2]);
	if(file_length < 1) return 1;
	
	parse_midi(file_buf, file_length, 0xFF);
	sort_events();
	save_events(argv[argc-1], 0xFF);
	
	delete[] file_buf;
	return 0;
}
