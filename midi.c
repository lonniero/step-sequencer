#include "midi.h"

#include <stdlib.h>
#include <string.h>

PmStream *midi_output_stream;
MidiOutputDevice *midi_output_devices;

int midi_output_count = 0;
char *midi_output_labels;

MidiOutputDevice *midi_input_devices;
int midi_input_count = 0;
char *midi_input_labels;

void
send_midi_note_on (PmStream *stream, uint8_t channel, uint8_t note, unsigned int velocity) {
  PmEvent note_on_event;
  note_on_event.message = Pm_Message(NOTE_ON | (channel & 0x0F), note, velocity);
  note_on_event.timestamp = 0;
  Pm_WriteShort(stream, 0, note_on_event.message);
}

void
send_midi_note_off (PmStream *stream, uint8_t channel, uint8_t note) {
  PmEvent note_on_event;
  note_on_event.message = Pm_Message(NOTE_OFF | (channel & 0x0F), note, 0);
  note_on_event.timestamp = 0;
  Pm_WriteShort(stream, 0, note_on_event.message);
}

static void
list_midi_inputs (void) {
  int32_t num_devices = Pm_CountDevices();
  free(midi_input_devices);
  free(midi_input_labels);

  size_t labels_size = strlen("Internal Clock") + 1;
  for (int32_t i = 0; i < num_devices; i++) {
    const PmDeviceInfo *info = Pm_GetDeviceInfo(i);
    if (info->input) labels_size += strlen("Clock: ") + strlen(info->name) + 1;
  }

  midi_input_devices = malloc(sizeof(MidiOutputDevice) * num_devices);
  midi_input_labels = malloc(labels_size);
  midi_input_count = 0;

  strlcpy(midi_input_labels, "Internal Clock", labels_size);

  for (int32_t i = 0; i < num_devices; i++) {
    const PmDeviceInfo *info = Pm_GetDeviceInfo(i);
    if (info->input) {
      midi_input_devices[midi_input_count].id = i;
      midi_input_devices[midi_input_count].name = (char *) info->name;
      strlcat(midi_input_labels, ";Clock: ", labels_size);
      strlcat(midi_input_labels, info->name, labels_size);
      midi_input_count++;
    }
  }
}

int32_t
list_midi_devices (void) {
  int32_t num_devices = Pm_CountDevices();
  free(midi_output_devices);
  free(midi_output_labels);

  size_t labels_size = strlen("No Output") + 1;
  for (int32_t i = 0; i < num_devices; i++) {
    const PmDeviceInfo *info = Pm_GetDeviceInfo(i);
    if (info->output) labels_size += strlen(info->name) + 1;
  }

  midi_output_devices = malloc(sizeof(MidiOutputDevice) * num_devices);
  midi_output_labels = malloc(labels_size);
  midi_output_count = 0;

  strlcpy(midi_output_labels, "No Output", labels_size);

  for (int32_t i = 0; i < num_devices; i++) {
    const PmDeviceInfo *info = Pm_GetDeviceInfo(i);
    if (info->output) {
      midi_output_devices[midi_output_count].id = i;
      midi_output_devices[midi_output_count].name = (char *) info->name;
      strlcat(midi_output_labels, ";", labels_size);
      strlcat(midi_output_labels, info->name, labels_size);
      midi_output_count++;
    }
  }
  list_midi_inputs();
  return midi_output_count;
}
