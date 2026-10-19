#pragma once

#include <portmidi.h>
#include <stdint.h>

#define NOTE_ON  144
#define NOTE_OFF 128

// System real-time and common messages used to follow an external MIDI clock.
#define MIDI_CLOCK         0xF8 // 24 per quarter note
#define MIDI_START         0xFA
#define MIDI_CONTINUE      0xFB
#define MIDI_STOP          0xFC
#define MIDI_SONG_POSITION 0xF2 // position in 16th notes since the song start

typedef struct MidiOutputDevice {
  int id;
  char *name;
} MidiOutputDevice;

extern PmStream *midi_output_stream;
extern MidiOutputDevice *midi_output_devices;

extern int midi_output_count;
extern char *midi_output_labels;

extern MidiOutputDevice *midi_input_devices;
extern int midi_input_count;
extern char *midi_input_labels; // "Internal Clock" then one "Clock: <name>" per input

void
send_midi_note_on (PmStream *stream, uint8_t channel, uint8_t note, unsigned int velocity);

void
send_midi_note_off (PmStream *stream, uint8_t channel, uint8_t note);

int32_t
list_midi_devices (void);
