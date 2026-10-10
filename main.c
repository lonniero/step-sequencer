#define RAYGUI_IMPLEMENTATION

#include <porttime.h>
#include <pthread.h>
#include <raygui.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "midi.h"
#include "raylib.h"

#define CLOCK_RESOLUTION_MS 1
#define TRACKS_LENGTH       8

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct Loop {
  double loop_start;
} Loop;

typedef struct Track {
  unsigned int steps;
  unsigned int midi_note;
  unsigned int current_step;
  uint64_t sequence;
  bool playing;
} Track;

double bpm = 120.0; // guarded by lock
const int beats_per_loop = 4;
bool running = true; // guarded by lock; once false, tick never touches MIDI again

Loop loop;
Track tracks[TRACKS_LENGTH];

int dropdownBoxActive = 0;
bool dropDownEditMode = false;

uint64_t
rand_u64 (void) {
  uint64_t r;
  arc4random_buf(&r, sizeof r);
  return r;
}

bool
bit_by_position (uint64_t num, int bit_position) {
  uint64_t mask = (uint64_t) 1 << bit_position;
  return (num & mask) >> bit_position;
}

void
randomize_tracks_seqs (void) {
  for (size_t i = 0; i < TRACKS_LENGTH; i++) {
    tracks[i].sequence = rand_u64();
  }
}

void
init_tracks (void) {
  for (size_t i = 0; i < TRACKS_LENGTH; i++) {
    tracks[i].steps = 8;
    tracks[i].midi_note = 40 + i;
  }
}

void
run_step (Track *track) {
  if (track->playing) {
    send_midi_note_off(midi_output_stream, track->midi_note);
    track->playing = false;
  }
  if (bit_by_position(track->sequence, track->current_step)) {
    send_midi_note_on(midi_output_stream, track->midi_note);
    track->playing = true;
  }
  track->current_step++;
}

void
sync_tracks (void) {
  for (size_t i = 0; i < TRACKS_LENGTH; i++) {
    tracks[i].current_step = 0;
    run_step(&tracks[i]);
  }
}

double
loop_length_ms (void) {
  return beats_per_loop * 60000.0 / bpm;
}

static void
tick (PtTimestamp now, void *user_data) {
  pthread_mutex_lock(&lock);
  if (running) {
    double loop_ms = loop_length_ms();
    double elapsed = now - loop.loop_start;
    if (elapsed >= loop_ms) {
      loop.loop_start += loop_ms;
      sync_tracks();
    } else {
      for (size_t i = 0; i < TRACKS_LENGTH; i++) {
        double step_length = loop_ms / (double) tracks[i].steps;
        int loop_current_step = elapsed / step_length;
        if (tracks[i].current_step <= loop_current_step) {
          tracks[i].current_step = loop_current_step;
          run_step(&tracks[i]);
        }
      }
    }
  }
  pthread_mutex_unlock(&lock);
}

void
stop_sequencer (void) {
  pthread_mutex_lock(&lock);
  running = false;
  for (size_t i = 0; i < TRACKS_LENGTH; i++) {
    if (tracks[i].playing) {
      send_midi_note_off(midi_output_stream, tracks[i].midi_note);
      tracks[i].playing = false;
    }
  }
  pthread_mutex_unlock(&lock);
}

int32_t
init_midi_output_stream (PmDeviceID output_device_id) {
  PmError result = Pm_OpenOutput(&midi_output_stream, output_device_id, NULL, 512, NULL, NULL, 0);
  if (result != pmNoError) {
    printf("Failed to open MIDI output device: %s\n", Pm_GetErrorText(result));
    return 1;
  }
  return 0;
}

int32_t
close_midi_output_stream (void) {
  if (!midi_output_stream) return 0;
  for (size_t i = 0; i < TRACKS_LENGTH; i++) {
    if (tracks[i].playing) {
      send_midi_note_off(midi_output_stream, tracks[i].midi_note);
      tracks[i].playing = false;
    }
  }
  PmError result = Pm_Close(midi_output_stream);
  midi_output_stream = NULL;
  if (result != pmNoError) {
    printf("Failed to close MIDI output device: %s\n", Pm_GetErrorText(result));
    return 1;
  }
  return 0;
}

void
render_dropdown (void) {
  if (GuiDropdownBox((Rectangle){20, 20, 125, 30}, midi_output_labels, &dropdownBoxActive, dropDownEditMode)) {
    dropDownEditMode = !dropDownEditMode;
    close_midi_output_stream();
    if (dropdownBoxActive > 0) {
      init_midi_output_stream(midi_output_devices[dropdownBoxActive - 1].id);
    }
  }
}

void
render_grid (void) {
  for (int i = 0; i < TRACKS_LENGTH; i++) {
    for (int j = 0; j < tracks[i].steps; j++) {
      bool on = bit_by_position(tracks[i].sequence, j);
      int x0 = 50 + (j * 160) / tracks[i].steps;
      int x1 = 50 + ((j + 1) * 160) / tracks[i].steps;
      Rectangle r = {x0, 50 + i * 20, x1 - x0, 20};
      if (GuiToggle(r, NULL, &on)) {
        tracks[i].sequence ^= (uint64_t) 1 << j; // flip bit 0
      }
    }
  }
}

int
main (void) {
  InitWindow(800, 600, "raygui - controls test suite");
  SetTargetFPS(60);
  Pm_Initialize();

  init_tracks();
  list_midi_devices();
  randomize_tracks_seqs();

  if (Pt_Start(CLOCK_RESOLUTION_MS, tick, NULL) != ptNoError) {
    CloseWindow();
    return 1;
  }

  while (!WindowShouldClose()) {
    BeginDrawing();
    ClearBackground(GetColor(GuiGetStyle(DEFAULT, BACKGROUND_COLOR)));

    pthread_mutex_lock(&lock);

    render_grid();
    render_dropdown();

    pthread_mutex_unlock(&lock);

    EndDrawing();
  }

  stop_sequencer();
  Pt_Stop();
  close_midi_output_stream();
  Pm_Terminate();
  CloseWindow();
  return 0;
}
