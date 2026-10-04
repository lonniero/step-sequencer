#ifndef SEQUENCER_H
#define SEQUENCER_H

#include <stdbool.h>
#include <stdint.h>

#define TRACK_COUNT 8
#define MAX_STEPS 64           // one bit of the pattern per step
#define MAX_DEVICES 32
#define DEVICE_NAME_LEN 64

// Everything the GUI is allowed to edit.
typedef struct TrackConfig {
  int steps;         // how many steps the loop is divided into (1..64)
  uint64_t pattern;  // bit n set -> step n plays
  int note;          // 0..127
  int channel;       // 1..16
  int velocity;      // 1..127
  bool muted;
} TrackConfig;

typedef struct SeqConfig {
  int bpm;           // tempo, quarter notes per minute
  int beats;         // length of one full loop in beats, shared by all tracks
  float gate;        // note length as a fraction of the step (0.05..1)
  bool playing;
  TrackConfig tracks[TRACK_COUNT];
} SeqConfig;

static inline double
seq_loop_ms (const SeqConfig *c) {
  return 60000.0 * c->beats / c->bpm;
}

// Read-only view of the playback state, for drawing.
typedef struct SeqStatus {
  double phase;                 // 0..1 position inside the loop
  int current_step[TRACK_COUNT];
  bool sounding[TRACK_COUNT];
} SeqStatus;

typedef struct MidiDevice {
  int id;
  char name[DEVICE_NAME_LEN];
} MidiDevice;

bool sequencer_init (const SeqConfig *initial);
void sequencer_shutdown (void);

// Copy config in/out under the sequencer lock. The clock thread only ever
// sees whole configs, so the GUI can freely edit its own copy.
void sequencer_get (SeqConfig *config, SeqStatus *status);
void sequencer_set (const SeqConfig *config);

// Restart the loop from the beginning.
void sequencer_rewind (void);
// Note off on everything (all channels).
void sequencer_panic (void);

// Output devices. Index 0 is always the app's own virtual port when it
// could be created.
int sequencer_list_devices (MidiDevice devices[], int max);
bool sequencer_open_device (int device_id);
int sequencer_current_device (void);
void sequencer_last_error (char *out, int len);

bool pattern_get (uint64_t pattern, int step);
uint64_t pattern_toggle (uint64_t pattern, int step);
uint64_t pattern_random (int steps);

#endif
