#define RAYGUI_IMPLEMENTATION 

#include <stdint.h>
#include <stdlib.h>
#include <raygui.h>
#include <pthread.h>
#include <porttime.h>
#include "raylib.h"

#define CLOCK_RESOLUTION_MS 1
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
int loop_ms = 10000;
const int tracks_length = 8;

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

Loop loop;
Track tracks[tracks_length];

uint64_t rand_u64 (void) {
  uint64_t r;
  arc4random_buf(&r, sizeof r);
  return r;
}

bool bit_by_position (uint64_t num, int bit_position) {
  uint64_t mask = (uint64_t)1 << bit_position;
  return (num & mask) >> bit_position;
}

void randomize_tracks_seqs (void) {
  for (size_t i = 0; i < tracks_length; i++) {
    tracks[i].sequence = rand_u64();
  }
}

void init_tracks (void) {
  for (size_t i = 0; i < tracks_length; i++) {
    tracks[i].steps = 8;
  }
}

void run_step (Track *track) {
  if (track->playing) {
    // send midi off
    track->playing = false;
  }
  if (bit_by_position(track->sequence, track->current_step)) {
    // send midi on
    track->playing = true;
  }
  track->current_step++;
}

void sync_tracks (void) {
  for (size_t i = 0; i < tracks_length; i++) {
    tracks[i].current_step = 0;
    run_step(&tracks[i]);
  }
}

static void
tick (PtTimestamp now, void *user_data) {
  pthread_mutex_lock(&lock);
  double elapsed = now - loop.loop_start;
  if (elapsed >= loop_ms) {
    loop.loop_start += loop_ms;
    sync_tracks();
  } else {
    for (size_t i = 0; i < tracks_length; i++) {
      double step_length = loop_ms / (double) tracks[i].steps;
      int loop_current_step = elapsed / step_length;
      if (tracks[i].current_step <= loop_current_step) {
        tracks[i].current_step = loop_current_step;
        run_step(&tracks[i]);
      }
    }
  }
  pthread_mutex_unlock(&lock);
}

int main(void) {
  InitWindow(400, 200, "raygui - controls test suite");
  SetTargetFPS(60);

  init_tracks();
  randomize_tracks_seqs();

  if (Pt_Start(CLOCK_RESOLUTION_MS, tick, NULL) != ptNoError) {
    CloseWindow();
    return 1;
  }

  while (!WindowShouldClose()) {
    BeginDrawing();
    ClearBackground(GetColor(GuiGetStyle(DEFAULT, BACKGROUND_COLOR)));
    EndDrawing();
  }

  Pt_Stop();
  CloseWindow();
  return 0;
}
