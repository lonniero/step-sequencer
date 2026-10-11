#pragma once

#include <portmidi.h>
#include <stdint.h>

#define NOTE_ON  144
#define NOTE_OFF 128

typedef struct MidiOutputDevice {
  int id;
  char *name;
} MidiOutputDevice;

extern PmStream *midi_output_stream;
extern MidiOutputDevice *midi_output_devices;

extern int midi_output_count;
extern char *midi_output_labels;

void
send_midi_note_on (PmStream *stream, uint8_t note, unsigned int velocity);

void
send_midi_note_off (PmStream *stream, uint8_t note);

int32_t
list_midi_devices (void);
