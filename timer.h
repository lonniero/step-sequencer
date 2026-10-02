#include "raylib.h"

typedef struct Interval {
  int interval_id;
  double last_tick;
  double interval_time;
  void (*fn)(int);
  bool initalized;
} Interval;

void
initialize_interval (Interval *interval, double interval_ms, void (*fn)(int), int interval_id);

void
run_intervals (Interval* intervals[], int length);
