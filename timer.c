#include <stdio.h>
#include "timer.h"

void
initialize_interval (Interval *interval, double interval_ms, void (*fn)(int), int interval_id) {
  interval->last_tick = GetTime();
  interval->interval_time = interval_ms / 1000.0;
  interval->interval_id = interval_id;
  interval->fn = fn;
  interval->initalized = true;
}

void
run_intervals (Interval* intervals[], int length) {
  double time = GetTime();
  for (int i = 0; i < length; i++) {
    if (intervals[i]->initalized) {
      if (time - intervals[i]->last_tick >= intervals[i]->interval_time) {
        intervals[i]->fn(intervals[i]->interval_id);
        intervals[i]->last_tick = time;
      }
    }
  }
}
