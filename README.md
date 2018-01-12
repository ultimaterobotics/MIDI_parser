# MIDI parser
A simple .mid files parser that outputs in a plain text format MIDI events and their absolute timestamps in milliseconds (instead of relative timing between MIDI events)
output format:
time_in_milliseconds,track_number,channel,event_type,key,value
Saved event types are:
Note off - 0,
Note on - 1,
Aftertouch - 2
Controller change - 3
Program change - 4 (key is set to 255, program number stored in value field)
Channel Key Pressure - 5 (key is set to 255, pressure stored in value field)
Pitch Bend - 6 (key is set to 255, pitch value stored in value field as signed 16 bit integer)
