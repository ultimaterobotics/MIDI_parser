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
#define SEND_TRACK_END		0b10000000

enum e_event_types
{
	evt_note_off = 0, 
	evt_note_on,
	evt_aftertouch,
	evt_ctrl_change,
	evt_prog_change,
	evt_chan_keypress,
	evt_pitch_bend,
	evt_track_end
};

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
	while(buf[pp] & 0x80)
	{
		val += buf[pp] & 0x7F;
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
	uint8_t active; //whant to turn off some events during post processing
	uint32_t T;
	uint8_t type;
	uint8_t track;
	uint8_t channel;
	uint8_t key;
	int value; //stores also pitch bend values
	void set_to(sMIDI_event e)
	{
		active = e.active;
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

void process_overlaps(int overlap_master)
{
	uint8_t keys_on[255];
	int keys_last_time[255];
	for(int x = 0; x < 255; x++)
	{
		keys_on[x] = 0;
		keys_last_time[x] = -1;
	}
	for(int n = 0; n < events_count; n++)
	{
		sMIDI_event *evt = events+n;
		if(evt->type < 2)
		{
			if(evt->T - keys_last_time[evt->key] < 1) evt->T++; //potentially can lead to wrong events order in the output, but practically unlikely
			keys_last_time[evt->key] = evt->T;
		}
		if(evt->type == 1)
		{
			if(keys_on[evt->key])
			{
				if(evt->track == overlap_master)
					keys_on[evt->key]++;
				evt->active = 0;
			}
		}
		if(evt->type == 0)
		{
			if(keys_on[evt->key] > 1)
			{ 
				keys_on[evt->key]--;
				evt->active = 0;
			}
			else keys_on[evt->key] = 0;
		}
	}
}

void save_events(char *fname, uint64_t track_mask)
{
	int handle = open(fname, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH);
	
	if(handle < 1)
	{
		fprintf(stderr, "can't open/create output file %s\n", fname);
		return;
	}
	
	char tbuf[1024];
	for(int x = 0; x < events_count; x++)
	{
		if(!((1<<events[x].track) & track_mask)) continue;
		
		int len = sprintf(tbuf, "%d,%d,%d,%d,%d,%d\n", events[x].T, events[x].track, events[x].channel, events[x].type, events[x].key, events[x].value);
/*		int conv[16];
		conv[0] = 2;
		conv[1] = 1;
		conv[7] = 4;
		int len = sprintf(tbuf, "%d,%d,%d,%d\n", events[x].T, conv[events[x].type], events[x].key, events[x].value*(events[x].type != 0));
*/
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
	for(int x = tempo_points-1; x >= 0; x--)
	{
		if(ms >= tempo_ms[x]) return tempo_value[x];
	}
	return 500000; //MIDI default
}

double get_dt_ms(uint32_t start_ms, uint32_t ticks)
{
	if(tempo_fixed) return ticks * ticks_to_ms;

	double cur_tempo = get_tempo(start_ms);
	ticks_to_ms = (cur_tempo / 1000.0) / (double)(ticks_per_qn);	
	return ticks * ticks_to_ms;

	double ms = start_ms;
	for(uint32_t t = 0; t < ticks; t++)
	{
		double cur_tempo = get_tempo(ms);
		ticks_to_ms = (cur_tempo / 1000.0) / (double)(ticks_per_qn);
		ms += ticks_to_ms;
	}
	return ms - start_ms;
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
	double rT = 0;
	uint32_t T = 0;
	int unhandled_sum = 0;
	int out_verbose = 1;//out_process;
	int send_out = out_process;
	
	int prev_msg_type = -1;
	int prev_msg_chan = -1;
	int prev_send = 0;

	while(pos < length)
	{
		uint32_t dt;
		int dpos = parse_vbl(buf+pos, &dt);
		pos += dpos;
		uint8_t type = buf[pos]>>4;
		uint8_t channel = buf[pos]&0x0F;
		uint8_t b1 = buf[pos+1];
		uint8_t b2 = buf[pos+2];
		int handled = 0;
		uint32_t cur_tempo = 0;
		double dt_ms = get_dt_ms(T, dt);
		rT += dt_ms ;//dt * ticks_to_ms;
		T = rT;
		sMIDI_event evt;
		evt.active = 1;
		evt.T = T;
		evt.channel = channel;
		evt.track = track_num;
		
//		printf("(%d, %d) ", T, dt_ms);
//		for(int nn = 0; nn < 16; nn++)
//			printf("%02X ", buf[pos-dpos+nn]);
//		printf("\n");
		if(0)if(buf[pos] < 128 && buf[pos] > 0)
		{
			handled = 1;
			pos += 1;
		}
		if(1)if(prev_msg_type != -1 && buf[pos] < 127)
		{
//			printf("(%d) controller?\n", T);
//			pos++;
			evt.type = prev_msg_type;
			evt.key = buf[pos];
			evt.channel = prev_msg_chan;
			evt.value = buf[pos+1];
			if(prev_send)
				add_event(evt);
			handled = 1;
			pos++;
			if(prev_msg_type <= evt_ctrl_change)
				pos++;//= 3;
//			else
//				pos += 1;
		}
		if(type >= 0x8 && type < 0xF)
		{
			out_verbose = 0;
			if(type == 8 || type == 0)
			{
				prev_send = 0;
				if(send_out & SEND_NOTE_OFF)
				{
					prev_send = 1;
					evt.type = evt_note_off;
					evt.key = b1;
					evt.value = b2;
					add_event(evt);
				}
				pos += 3;
				handled = 1;
				prev_msg_type = evt_note_off;
				prev_msg_chan = channel;
			}
			if(type == 9)
			{
				prev_send = 0;
				if(send_out & SEND_NOTE_ON)
				{
					prev_send = 1;
					evt.type = evt_note_on;
					evt.key = b1;
					evt.value = b2;
					add_event(evt);
				}
				pos += 3;
				handled = 1;
				prev_msg_type = evt_note_on;
				prev_msg_chan = channel;
			}
			if(type == 0xA)
			{
				prev_send = 0;
				if(send_out & SEND_AFTERTOUCH)
				{
					prev_send = 1;
					evt.type = evt_aftertouch;
					evt.key = b1;
					evt.value = b2;
					add_event(evt);
				}
				if(out_verbose) printf("(%d) ch %d aft %d, v %d\n", T, channel, b1, b2);
				pos += 3;
				handled = 1;
				prev_msg_type = evt_aftertouch;
				prev_msg_chan = channel;
			}
			if(type == 0xB)
			{
				prev_send = 0;
				if(send_out & SEND_CTRL_CHANGE)
				{
					prev_send = 1;
					evt.type = evt_ctrl_change;
					evt.key = b1;
					evt.value = b2;
					add_event(evt);
				}
				if(out_verbose) printf("(%d) ch %d cc %d, cv %d\n", T, channel, b1, b2);
				pos += 3;
				handled = 1;
				prev_msg_type = evt_ctrl_change;
				prev_msg_chan = channel;
			}
			if(type == 0xC)
			{
				prev_send = 0;
				if(send_out & SEND_PROG_CHANGE)
				{
					prev_send = 1;
					evt.type = evt_prog_change;
					evt.key = 255;
					evt.value = b1;
					add_event(evt);
				}
				if(out_verbose) printf("(%d) ch %d prog %d\n", T, channel, b1);
				pos += 2;
				handled = 1;
				prev_msg_type = evt_prog_change;
				prev_msg_chan = channel;
			}
			if(type == 0xD)
			{
				prev_send = 0;
				if(send_out & SEND_CHAN_KEYPRES)
				{
					prev_send = 1;
					evt.type = evt_chan_keypress;
					evt.key = 255;
					evt.value = b1;
					add_event(evt);
				}
				
				if(out_verbose) printf("(%d) ch %d AFT %d\n", T, channel, b1);
				pos += 2;
				handled = 1;
				prev_msg_type = evt_chan_keypress;
				prev_msg_chan = channel;
			}
			if(type == 0xE)
			{
				prev_send = 0;
				if(send_out & SEND_PITCH_BEND)
				{
					prev_send = 1;
					evt.type = evt_pitch_bend;
					evt.key = 255;
					evt.value = (b2<<8) + b1;
					add_event(evt);
				}
				if(out_verbose) printf("(%d) ch %d pitch %d\n", T, channel, (b2<<8) + b1);
				pos += 3;
				handled = 1;
				prev_msg_type = evt_pitch_bend;
				prev_msg_chan = channel;
			}
		}
		if(type == 0xF)
		{
			prev_msg_type = -1;
			out_verbose = 1;
			if(channel == 0)
			{
				if(out_verbose) printf("(%d) sysex F0\n", T);
				handled = 1;
				uint32_t len = 0;
				int dpos = parse_vbl(buf+pos+1, &len);
				pos += dpos + len + 1;
			}
			if(channel == 1)
			{
				if(out_verbose) printf("(%d) MIDI Time Code Qtr. Frame\n", T);
				handled = 1;
				pos += 3;
			}
			if(channel == 2)
			{
				if(out_verbose) printf("(%d) Song Position Pointer\n", T);
				handled = 1;
				pos += 3;
			}
			if(channel == 3)
			{
				if(out_verbose) printf("(%d) Song Select\n", T);
				handled = 1;
				pos += 2;
			}
			if(channel == 6)
			{
				if(out_verbose) printf("(%d) Tune Request\n", T);
				handled = 1;
				pos += 1;
			}
			if(channel == 7)
			{ 
				if(out_verbose) printf("(%d) sysex F7\n", T);
				handled = 1;
				uint32_t len = 0;
				int dpos = parse_vbl(buf+pos+1, &len);
				pos += dpos + len + 1;
			}
			if(channel == 8)
			{
				if(out_verbose) printf("(%d) Timing clock\n", T);
				handled = 1;
				pos += 1;
			}
			if(channel == 0xA)
			{
				if(out_verbose) printf("(%d) Start\n", T);
				handled = 1;
				pos += 1;
			}
			if(channel == 0xB)
			{
				if(out_verbose) printf("(%d) Stop\n", T);
				handled = 1;
				pos += 1;
			}
			if(channel == 0xF) //meta event
			{
				uint32_t len = 0;
				if(b1 >= 1 && b1 <= 9 || b1 == 0x7F)
				{
					int dpos = parse_vbl(buf+pos+2, &len);
//					printf("vbl: %d %d\n", dpos, len);
					pos += dpos+2;
//					b1 = buf[pos+1];
//					b2 = buf[pos+2];
//					pos++; //b1
				}
				
				if(b1 == 0)
				{
					if(out_verbose) printf("(%d) meta 0\n", T); 
					handled = 1; 
					pos += 4;
				}
				if(b1 == 1)
				{
					if(out_verbose)
					{
						printf("(%d) meta text: ", T); 
						for(int x = 0; x < len; x++) printf("%c", buf[pos+x]);
						printf("\n");
					}
					handled = 1; 
					pos += len;
				}
				if(b1 == 2)
				{
					if(out_verbose)
					{
						printf("(%d) meta copyright: ", T); 
						for(int x = 0; x < len; x++) printf("%c", buf[pos+x]);
						printf("\n");
					}
					handled = 1; 
					pos += len;
				}
				if(b1 == 3)
				{ 
					if(out_verbose)
					{
						printf("(%d) meta Track Name (%d): ", T, pos); 
						for(int x = 0; x < len; x++) printf("%c", buf[pos+x]);
						printf("\n");
					}
					handled = 1; 
					pos += len;
				}
				if(b1 == 4) 
				{
					if(out_verbose)
					{
						printf("(%d) meta Instrument Name: ", T); 
						for(int x = 0; x < len; x++) printf("%c", buf[pos+x]);
						printf("\n");
					}
					
					handled = 1; 
					pos += len;
				}
				if(b1 == 5)
				{
					if(out_verbose)
					{
						printf("(%d) meta Lyrics: ", T); 
						for(int x = 0; x < len; x++) printf("%c", buf[pos+x]);
						printf("\n");
					}
					handled = 1; 
					pos += len;
				}
				if(b1 == 6) 
				{
					if(out_verbose)
					{
						printf("(%d) meta Marker: ", T); 
						for(int x = 0; x < len; x++) printf("%c", buf[pos+x]);
						printf("\n");
					}
					handled = 1; 
					pos += len;
				}
				if(b1 == 7) 
				{
					if(out_verbose)
					{
						printf("(%d) meta Cue Point: ", T); 
						for(int x = 0; x < len; x++) printf("%c", buf[pos+x]);
						printf("\n");
					}
					handled = 1; 
					pos += len;
				}
				if(b1 == 8) 
				{
					if(out_verbose)
					{
						printf("(%d) meta Program Name: ", T); 
						for(int x = 0; x < len; x++) printf("%c", buf[pos+x]);
						printf("\n");
					}
					handled = 1; 
					pos += len;
				}
				if(b1 == 9) 
				{
					if(out_verbose)
					{
						printf("(%d) meta Device Name: ", T); 
						for(int x = 0; x < len; x++) printf("%c", buf[pos+x]);
						printf("\n");
					}
					handled = 1; 
					pos += len;
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
					if(send_out & SEND_TRACK_END)
					{
						evt.type = evt_track_end;
						evt.key = 255;
						evt.value = 255;
						add_event(evt);
					}
					handled = 1; 
					pos += 3;
				}
				if(b1 == 0x51)
				{
					uint32_t mpqn = (buf[pos+3]<<16)|(buf[pos+4]<<8)|buf[pos+5];
					if(out_verbose) printf("(%d) meta tempo %d\n", T, mpqn);
					add_tempo_point(T, mpqn);
					ticks_to_ms = (float)(mpqn / 1000.0) / (float)(ticks_per_qn);
					
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
					pos += len;
				}
				if(!handled) 
				{
					if(out_verbose) fprintf(stderr, "unhandled meta: %02X %02X %02X %02X %02X %02X %02X %02X\n", b1, b2, buf[pos+3], buf[pos+4], buf[pos+5], buf[pos+6], buf[pos+7], buf[pos+8]); 
					pos += len;
				}
			}
			
		}
		if(!handled)
		{ 
			//if(out_verbose) 
			//fprintf(stderr, "(%d) unhandled %02X %02X %02X %02X %02X %02X %02X %02X\n", T, buf[pos-2], buf[pos-1], buf[pos], buf[pos+1], buf[pos+2], buf[pos+3], buf[pos+4], buf[pos+5]); 
			printf("(%d, %d) unhandled ", T, pos);
			for(int nn = 0; nn < 16; nn++)
				printf("%02X ", buf[pos-3+nn]);
			printf("\n");
			
			unhandled_sum++;
			
			rT -= dt_ms;
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
			int tpqn = (buf[pos+8+4]<<8) | buf[pos+8+5];
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
		printf("\nMIDI file parser v1.0\nusage: midi_parser -flags <input filename> <output filename>\n");
		printf("flags define which MIDI events from which tracks will and will not be stored\n");
		printf("track flags starts with -t to enable track\n");
		printf("-t1 -t2 will enable first and second tracks\n");
		printf("-tall will enable all tracks (default option)\n");
		printf("event flag starts with -e to disable, -E to enable event\n");
		printf("events:\n");
		printf("\tNOFF - Note Off (code %d)\n", evt_note_off);
		printf("\tNON - Note On (code %d)\n", evt_note_on);
		printf("\tAFT - Aftertouch (code %d)\n", evt_aftertouch);
		printf("\tCC - Controller Change (code %d)\n", evt_ctrl_change);
		printf("\tPC - Program Change (code %d)\n", evt_prog_change);
		printf("\tCKP - Channel Key Pressure (code %d)\n", evt_chan_keypress);
		printf("\tPB - Pitch Bend (code %d)\n", evt_pitch_bend);
		printf("\tTE - Track End (code %d)\n", evt_track_end);

		printf("\n\nAdditional options:\n");
		printf("\tCUTOVP - cut overlapping notes\n");

		printf("\nBy default, events Note On, Note off and Track End are stored, all others ignored\n");
		printf("example:\n");

		printf("midi_parser -t2 -eNON -eNOFF -ECC -EPC -EPB input.mid output.txt\n");
		printf("this command will store only Controller Change, Program Change and Pitch Bend events on track 2\n");

		printf("\nOutput format: time in milliseconds, track, channel, type, key, value\n");
		printf("For events that are not related to a specific key, key field is set to 255\n");
		printf("For events that don't have valid value, it is set to 255\n");
		printf("\n");
		
		return 1;
	}
	
	int send_events = SEND_NOTE_ON | SEND_NOTE_OFF | SEND_TRACK_END;
	
	uint64_t track_mask = 0;
	int prevent_overlap = 0;
	int overlap_master = 1;

	for(int a = 1; a < argc-2; a++)
	{
		if(str_eq(argv[a], "-eNON")) send_events &= ~SEND_NOTE_ON;
		if(str_eq(argv[a], "-ENON")) send_events |= SEND_NOTE_ON;
		if(str_eq(argv[a], "-eNOFF")) send_events &= ~SEND_NOTE_OFF;
		if(str_eq(argv[a], "-ENOFF")) send_events |= SEND_NOTE_OFF;
		if(str_eq(argv[a], "-eAFT")) send_events &= ~SEND_AFTERTOUCH;
		if(str_eq(argv[a], "-EAFT")) send_events |= SEND_AFTERTOUCH;
		if(str_eq(argv[a], "-eCC")) send_events &= ~SEND_CTRL_CHANGE;
		if(str_eq(argv[a], "-ECC")) send_events |= SEND_CTRL_CHANGE;
		if(str_eq(argv[a], "-ePC")) send_events &= ~SEND_PROG_CHANGE;
		if(str_eq(argv[a], "-EPC")) send_events |= SEND_PROG_CHANGE;
		if(str_eq(argv[a], "-eCKP")) send_events &= ~SEND_CHAN_KEYPRES;
		if(str_eq(argv[a], "-ECKP")) send_events |= SEND_CHAN_KEYPRES;
		if(str_eq(argv[a], "-ePB")) send_events &= ~SEND_PITCH_BEND;
		if(str_eq(argv[a], "-EPB")) send_events |= SEND_PITCH_BEND;
		if(str_eq(argv[a], "-eTE")) send_events &= ~SEND_TRACK_END;
		if(str_eq(argv[a], "-ETE")) send_events |= SEND_TRACK_END;
		
		if(argv[a][0] == '-' && argv[a][1] == 't')
		{
			int tnum = 0;
			if(argv[a][2] >= '0' && argv[a][2] <= '9') tnum += argv[a][2]-'0';
			if(argv[a][3] >= '0' && argv[a][3] <= '9') tnum = tnum*10 + argv[a][3]-'0';
			if(tnum > 0 && tnum < 65) track_mask |= (1<<(tnum-1));
		}
		
		if(str_eq(argv[a], "-CUTOVP")) prevent_overlap = 1;
	}
	if(track_mask == 0) track_mask = 0xFFFFFFFFFFFFFFFF;

	read_file(argv[argc-2]);
	if(file_length < 1) return 1;
	
	parse_midi(file_buf, file_length, send_events);
	sort_events();
	if(prevent_overlap)
		process_overlaps(overlap_master);
	save_events(argv[argc-1], track_mask);
	
	delete[] file_buf;
	return 0;
}
