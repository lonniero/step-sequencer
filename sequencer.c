#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <portmidi.h>
#include <porttime.h>
#include "sequencer.h"

#define VIRTUAL_PORT_NAME "raygui-midi"
#define CLOCK_RESOLUTION_MS 1

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static SeqConfig config;
static SeqStatus status;

static PortMidiStream *stream = NULL;
static int current_device = -1;
static int virtual_device = -1;
static char last_error[128] = "";

// Clock state, only touched with the lock held.
static PtTimestamp last_tick_ms;
static double position_ms;       // where we are inside the loop
static int sounding_note[TRACK_COUNT];
static int sounding_channel[TRACK_COUNT];
static double note_off_at[TRACK_COUNT]; // in loop-independent clock ms
static double clock_ms;                 // monotonic, advances only while playing

bool
pattern_get (uint64_t pattern, int step) {
  return (pattern >> step) & 1ULL;
}

uint64_t
pattern_toggle (uint64_t pattern, int step) {
  return pattern ^ (1ULL << step);
}

uint64_t
pattern_random (int steps) {
  uint64_t r;
  arc4random_buf(&r, sizeof r);
  return steps >= 64 ? r : r & ((1ULL << steps) - 1);
}

static void
set_error (const char *what, PmError err) {
  snprintf(last_error, sizeof last_error, "%s: %s", what, Pm_GetErrorText(err));
  fprintf(stderr, "%s\n", last_error);
}

static void
send (int status_byte, int data1, int data2) {
  if (!stream) return;
  PmError err = Pm_WriteShort(stream, 0, Pm_Message(status_byte, data1, data2));
  if (err != pmNoError) set_error("Pm_WriteShort", err);
}

static void
note_off (int track) {
  if (sounding_note[track] < 0) return;
  send(0x80 | (sounding_channel[track] - 1), sounding_note[track], 0);
  sounding_note[track] = -1;
  status.sounding[track] = false;
}

static void
note_on (int track, double step_ms) {
  TrackConfig *t = &config.tracks[track];
  note_off(track); // retrigger
  send(0x90 | (t->channel - 1), t->note, t->velocity);
  sounding_note[track] = t->note;
  sounding_channel[track] = t->channel;
  note_off_at[track] = clock_ms + step_ms * config.gate;
  status.sounding[track] = true;
}

static void
all_notes_off (void) {
  for (int i = 0; i < TRACK_COUNT; i++) note_off(i);
}

static void
reset_steps (void) {
  // -1 so step 0 fires on the next tick
  for (int i = 0; i < TRACK_COUNT; i++) status.current_step[i] = -1;
}

// Runs on PortTime's thread every millisecond.
static void
tick (PtTimestamp now, void *user_data) {
  (void)user_data;
  pthread_mutex_lock(&lock);

  double dt = now - last_tick_ms;
  last_tick_ms = now;

  if (config.playing) {
    clock_ms += dt;
    position_ms += dt;
    double loop_ms = seq_loop_ms(&config);
    if (position_ms >= loop_ms) {
      // Every track restarts together at the loop boundary, which keeps
      // the polyrhythm locked no matter how the step lengths divide.
      position_ms -= loop_ms * (int)(position_ms / loop_ms);
      reset_steps();
    }
    status.phase = position_ms / loop_ms;

    for (int i = 0; i < TRACK_COUNT; i++) {
      if (sounding_note[i] >= 0 && clock_ms >= note_off_at[i]) note_off(i);

      TrackConfig *t = &config.tracks[i];
      double step_ms = loop_ms / t->steps;
      int step = (int)(position_ms / step_ms);
      if (step >= t->steps) step = t->steps - 1;
      if (step == status.current_step[i]) continue;

      status.current_step[i] = step;
      if (!t->muted && pattern_get(t->pattern, step)) note_on(i, step_ms);
    }
  }

  pthread_mutex_unlock(&lock);
}

bool
sequencer_init (const SeqConfig *initial) {
  config = *initial;
  for (int i = 0; i < TRACK_COUNT; i++) sounding_note[i] = -1;
  reset_steps();

  PmError err = Pm_Initialize();
  if (err != pmNoError) {
    set_error("Pm_Initialize", err);
    return false;
  }

  // PortTime has to run before any stream is opened (see portmidi.h).
  if (Pt_Start(CLOCK_RESOLUTION_MS, tick, NULL) != ptNoError) {
    snprintf(last_error, sizeof last_error, "Pt_Start failed");
    return false;
  }
  pthread_mutex_lock(&lock);
  last_tick_ms = Pt_Time();
  pthread_mutex_unlock(&lock);

  // A virtual port lets a DAW/synth pick us up without any IAC setup.
  // Not available on every platform, so failure is fine.
  int id = Pm_CreateVirtualOutput(VIRTUAL_PORT_NAME, NULL, NULL);
  if (id >= 0) virtual_device = id;

  int initial_device = virtual_device >= 0 ? virtual_device : Pm_GetDefaultOutputDeviceID();
  if (initial_device >= 0) sequencer_open_device(initial_device);
  return true;
}

void
sequencer_shutdown (void) {
  pthread_mutex_lock(&lock);
  config.playing = false;
  all_notes_off();
  pthread_mutex_unlock(&lock);

  Pt_Stop();

  if (stream) Pm_Close(stream);
  stream = NULL;
  if (virtual_device >= 0) Pm_DeleteVirtualDevice(virtual_device);
  Pm_Terminate();
}

void
sequencer_get (SeqConfig *out_config, SeqStatus *out_status) {
  pthread_mutex_lock(&lock);
  if (out_config) *out_config = config;
  if (out_status) *out_status = status;
  pthread_mutex_unlock(&lock);
}

void
sequencer_set (const SeqConfig *new_config) {
  pthread_mutex_lock(&lock);

  double old_loop_ms = seq_loop_ms(&config);
  double new_loop_ms = seq_loop_ms(new_config);
  if (new_loop_ms != old_loop_ms) {
    // Keep the playhead at the same relative spot in the loop.
    position_ms = position_ms / old_loop_ms * new_loop_ms;
  }
  if (config.playing && !new_config->playing) all_notes_off();
  if (!config.playing && new_config->playing) last_tick_ms = Pt_Time();

  for (int i = 0; i < TRACK_COUNT; i++) {
    const TrackConfig *old = &config.tracks[i];
    const TrackConfig *new = &new_config->tracks[i];
    // Anything that changes what is sounding cuts the note so nothing hangs.
    if (new->muted || new->note != old->note || new->channel != old->channel) {
      note_off(i);
    }
    if (new->steps != old->steps) {
      // Recompute the step without retriggering mid-step.
      double step_ms = new_loop_ms / new->steps;
      status.current_step[i] = (int)(position_ms / step_ms);
    }
  }

  config = *new_config;
  pthread_mutex_unlock(&lock);
}

void
sequencer_rewind (void) {
  pthread_mutex_lock(&lock);
  all_notes_off();
  position_ms = 0;
  status.phase = 0;
  reset_steps();
  pthread_mutex_unlock(&lock);
}

void
sequencer_panic (void) {
  pthread_mutex_lock(&lock);
  all_notes_off();
  for (int ch = 0; ch < 16; ch++) {
    send(0xB0 | ch, 123, 0); // All Notes Off
    send(0xB0 | ch, 120, 0); // All Sound Off
  }
  pthread_mutex_unlock(&lock);
}

int
sequencer_list_devices (MidiDevice devices[], int max) {
  int count = 0;
  for (int id = 0; id < Pm_CountDevices() && count < max; id++) {
    const PmDeviceInfo *info = Pm_GetDeviceInfo(id);
    if (!info || !info->output) continue;
    devices[count].id = id;
    snprintf(devices[count].name, DEVICE_NAME_LEN, "%s%s",
             info->is_virtual ? "(virtual) " : "", info->name);
    count++;
  }
  return count;
}

bool
sequencer_open_device (int device_id) {
  pthread_mutex_lock(&lock);

  all_notes_off();
  if (stream) Pm_Close(stream);
  stream = NULL;
  current_device = -1;

  // latency 0: timestamps are ignored and messages go out immediately,
  // the 1ms clock thread is already doing the scheduling.
  PmError err = Pm_OpenOutput(&stream, device_id, NULL, 0, NULL, NULL, 0);
  if (err != pmNoError) {
    set_error("Pm_OpenOutput", err);
    stream = NULL;
  } else {
    current_device = device_id;
    last_error[0] = '\0';
  }

  pthread_mutex_unlock(&lock);
  return stream != NULL;
}

int
sequencer_current_device (void) {
  return current_device;
}

void
sequencer_last_error (char *out, int len) {
  pthread_mutex_lock(&lock);
  snprintf(out, len, "%s", last_error);
  pthread_mutex_unlock(&lock);
}
