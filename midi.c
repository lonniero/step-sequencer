#include "midi.h"

#include <stdlib.h>
#include <string.h>

PmStream *midi_output_stream;
MidiOutputDevice *midi_output_devices;

int midi_output_count = 0;
char midi_output_labels[1024];

void
send_midi_note_on (PmStream *stream, uint8_t note) {
  PmEvent note_on_event;
  note_on_event.message = Pm_Message(NOTE_ON, note, 127);
  note_on_event.timestamp = 0;
  Pm_WriteShort(stream, 0, note_on_event.message);
}

void
send_midi_note_off (PmStream *stream, uint8_t note) {
  PmEvent note_on_event;
  note_on_event.message = Pm_Message(NOTE_OFF, note, 0);
  note_on_event.timestamp = 0;
  Pm_WriteShort(stream, 0, note_on_event.message);
}

int32_t
list_midi_devices (void) {
  int32_t num_devices = Pm_CountDevices();
  midi_output_devices = malloc(sizeof(MidiOutputDevice) * num_devices);
  midi_output_count = 0;
  strlcat(midi_output_labels, "No Output;", sizeof midi_output_labels);
  for (int32_t i = 0; i < num_devices; ++i) {
    const PmDeviceInfo *info = Pm_GetDeviceInfo(i);
    if (info->output) {
      midi_output_devices[midi_output_count].id = i;
      midi_output_devices[midi_output_count].name = (char *) info->name;
      if (midi_output_count > 0) strlcat(midi_output_labels, ";", sizeof midi_output_labels);
      strlcat(midi_output_labels, info->name, sizeof midi_output_labels);
      midi_output_count++;
    }
  }
  return midi_output_count;
}
