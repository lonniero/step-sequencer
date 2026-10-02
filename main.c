#define RAYGUI_IMPLEMENTATION

#include <stdint.h>
#include <stdlib.h>
#include "raylib.h"
#include <raygui.h>
#include "timer.h"

int loop_ms = 10000;
const int intervals_length = 9;
Interval *intervals[intervals_length];

int64_t rand_i64(void) {
  int64_t r;
  arc4random_buf(&r, sizeof r);
  return r;
}

int divs[] = {32, 6, 8, 8, 8, 3, 5, 2};
#define DIVS_LENGTH (sizeof divs / sizeof divs[0])

uint64_t seqs[DIVS_LENGTH];

void
randomize_seqs (void) {
  for (size_t i = 0; i < DIVS_LENGTH; i++) {
    seqs[i] = (uint64_t)rand_i64();
  }
}

bool bit_by_position (uint64_t num, int bit_position) {
    unsigned int mask = 1 << bit_position;
    return (num & mask) >> bit_position;
}


void sync_intervals (int interval_id) {
  for (int i = 1; i < intervals_length; i++) { // interval[0] is the control interval
    intervals[i]->last_tick = GetTime();
    intervals[i]->interval_time = (loop_ms / (double)divs[i - 1]) / 1000.0;
    intervals[i]->fn(intervals[i]->interval_id); // trigger all
  }
}

void
print_internval_id (int interval_id) {
  printf("%s%d\n", "Interval: ", interval_id);
}

int 
main(void) {
  InitWindow(400, 200, "raygui - controls test suite");
  SetTargetFPS(60);

  randomize_seqs();

  for (int i = 1; i < intervals_length; i++) { // interval[0] is the control interval
    intervals[i] = malloc(sizeof(Interval));
    initialize_interval(intervals[i], loop_ms / (double)divs[i - 1], print_internval_id, i);
  }

  intervals[0] = malloc(sizeof(Interval));
  initialize_interval(intervals[0], loop_ms, sync_intervals, 0);

  sync_intervals(0); // first tick

  while (!WindowShouldClose()) {
    run_intervals(intervals, intervals_length);
    BeginDrawing();
    ClearBackground(GetColor(GuiGetStyle(DEFAULT, BACKGROUND_COLOR)));

    if (GuiButton((Rectangle){ 24, 24, 120, 30 }, "#191#Print message")) {
      printf("%s\n", "Hello world");
    }

    EndDrawing();
  }

  CloseWindow();
  return 0;
}
