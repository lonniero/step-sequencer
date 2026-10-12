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
#define MAX_STEPS           32
#define MIN_BPM             40
#define MAX_BPM             240

#define MIDI_CHANNELS       16

#define WINDOW_WIDTH  910
#define WINDOW_HEIGHT 520
#define MARGIN        20
#define HEADER_HEIGHT 36
#define TOOLBAR_Y     52
#define LABELS_Y      98
#define CHANNEL_X     310
#define GRID_X        440
#define GRID_Y        120
#define GRID_WIDTH    450
#define GRID_GAP      2
#define ROW_HEIGHT    38
#define CELL_HEIGHT   28
#define STEP_LABELS_Y 428
#define STEP_Y        450
#define FOOTER_Y      490

#define PAPER      (Color){232, 230, 223, 255}
#define PAPER_DARK (Color){214, 211, 202, 255}
#define INK        (Color){21, 21, 21, 255}
#define INK_LIGHT  (Color){58, 58, 58, 255}
#define MUTED      (Color){140, 138, 132, 255}
#define RULE       (Color){201, 198, 188, 255}
#define ACCENT     (Color){206, 112, 78, 255}

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct Loop {
  double loop_start;
} Loop;

typedef struct Track {
  unsigned int id;
  unsigned int steps;
  unsigned int midi_note;
  unsigned int midi_channel; // 1-16
  unsigned int current_step;
  uint64_t sequence;
  bool playing;
  unsigned int playing_note;    // note and channel of the sounding note, so note off matches it
  unsigned int playing_channel; // even if detune, note or channel changed since note on
} Track;

typedef struct Step {
  signed int detune;
  unsigned int velocity;
} Step;

double bpm = 120.0; // guarded by lock
const int beats_per_loop = 4;
bool running = true; // guarded by lock; once false, tick never touches MIDI again
bool transport_playing = false; // guarded by lock; play/stop state

Loop loop;
Track tracks[TRACKS_LENGTH];
Step steps[TRACKS_LENGTH * MAX_STEPS];

int dropdown_active = 0;
bool dropdown_edit_mode = false;

int selected_step = 0;

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

uint64_t
steps_mask (unsigned int steps) {
  return ((uint64_t) 1 << steps) - 1;
}

void
randomize_tracks_seqs (void) {
  for (size_t i = 0; i < TRACKS_LENGTH; i++) {
    tracks[i].sequence = rand_u64() & steps_mask(tracks[i].steps);
  }
}

void
clear_tracks_seqs (void) {
  for (size_t i = 0; i < TRACKS_LENGTH; i++) {
    tracks[i].sequence = 0;
  }
}

void
init_tracks (void) {
  for (size_t i = 0; i < TRACKS_LENGTH; i++) {
    tracks[i].id = i;
    tracks[i].steps = 16;
    tracks[i].midi_note = 40 + i;
    tracks[i].midi_channel = 1;
  }
}

void
init_steps (void) {
  for (size_t i = 0; i < TRACKS_LENGTH * MAX_STEPS; i++) {
    steps[i].detune = 0;
    steps[i].velocity = 100;
  }
}

void
release_track (Track *track) {
  if (track->playing) {
    send_midi_note_off(midi_output_stream, track->playing_channel - 1, track->playing_note);
    track->playing = false;
  }
}

void
run_step (Track *track) {
  release_track(track);
  if (bit_by_position(track->sequence, track->current_step)) {
    signed int detune = steps[(track->id * MAX_STEPS) + track->current_step].detune;
    unsigned int velocity = steps[(track->id * MAX_STEPS) + track->current_step].velocity;
    track->playing_note = track->midi_note + detune;
    track->playing_channel = track->midi_channel;
    send_midi_note_on(midi_output_stream, track->playing_channel - 1, track->playing_note, velocity);
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

void
set_bpm (double new_bpm) {
  double now = Pt_Time();
  double phase = (now - loop.loop_start) / loop_length_ms();
  bpm = new_bpm;
  loop.loop_start = now - phase * loop_length_ms();
}

void
set_track_steps (Track *track, unsigned int steps) {
  double step_length = loop_length_ms() / steps;
  unsigned int step = (Pt_Time() - loop.loop_start) / step_length;
  track->steps = steps;
  track->current_step = step + 1;
}

void
set_track_note (Track *track, unsigned int note) {
  release_track(track);
  track->midi_note = note;
}

void
set_track_channel (Track *track, unsigned int channel) {
  release_track(track);
  track->midi_channel = channel;
}

void
release_all_tracks (void) {
  for (size_t i = 0; i < TRACKS_LENGTH; i++) {
    release_track(&tracks[i]);
  }
}

void
play (void) {
  loop.loop_start = Pt_Time();
  transport_playing = true;
  sync_tracks();
}

void
stop (void) {
  transport_playing = false;
  release_all_tracks();
  for (size_t i = 0; i < TRACKS_LENGTH; i++) {
    tracks[i].current_step = 0;
  }
}

void
toggle_transport (void) {
  if (transport_playing) {
    stop();
  } else {
    play();
  }
}

static void
tick (PtTimestamp now, void *user_data) {
  pthread_mutex_lock(&lock);
  if (running && transport_playing) {
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
  release_all_tracks();
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
  release_all_tracks();
  PmError result = Pm_Close(midi_output_stream);
  midi_output_stream = NULL;
  if (result != pmNoError) {
    printf("Failed to close MIDI output device: %s\n", Pm_GetErrorText(result));
    return 1;
  }
  return 0;
}

void
select_midi_output (int index) {
  close_midi_output_stream();
  if (index > 0) init_midi_output_stream(midi_output_devices[index - 1].id);
}

const char *
note_name (unsigned int note) {
  static const char *names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  return TextFormat("%s%d", names[note % 12], (int) note / 12 - 1);
}

void
setup_style (void) {
  GuiSetStyle(DEFAULT, BACKGROUND_COLOR, ColorToInt(PAPER));
  GuiSetStyle(DEFAULT, LINE_COLOR, ColorToInt(INK));
  GuiSetStyle(DEFAULT, TEXT_SIZE, 10);
  GuiSetStyle(DEFAULT, TEXT_SPACING, 1);
  GuiSetStyle(DEFAULT, BORDER_WIDTH, 1);

  GuiSetStyle(DEFAULT, BORDER_COLOR_NORMAL, ColorToInt(INK));
  GuiSetStyle(DEFAULT, BASE_COLOR_NORMAL, ColorToInt(PAPER));
  GuiSetStyle(DEFAULT, TEXT_COLOR_NORMAL, ColorToInt(INK));
  GuiSetStyle(DEFAULT, BORDER_COLOR_FOCUSED, ColorToInt(INK));
  GuiSetStyle(DEFAULT, BASE_COLOR_FOCUSED, ColorToInt(PAPER_DARK));
  GuiSetStyle(DEFAULT, TEXT_COLOR_FOCUSED, ColorToInt(INK));
  GuiSetStyle(DEFAULT, BORDER_COLOR_PRESSED, ColorToInt(INK));
  GuiSetStyle(DEFAULT, BASE_COLOR_PRESSED, ColorToInt(INK));
  GuiSetStyle(DEFAULT, TEXT_COLOR_PRESSED, ColorToInt(PAPER));
  GuiSetStyle(DEFAULT, BORDER_COLOR_DISABLED, ColorToInt(MUTED));
  GuiSetStyle(DEFAULT, BASE_COLOR_DISABLED, ColorToInt(PAPER));
  GuiSetStyle(DEFAULT, TEXT_COLOR_DISABLED, ColorToInt(MUTED));

  GuiSetStyle(LABEL, TEXT_COLOR_FOCUSED, ColorToInt(INK));
  GuiSetStyle(LABEL, TEXT_COLOR_PRESSED, ColorToInt(INK));

  GuiSetStyle(BUTTON, BASE_COLOR_FOCUSED, ColorToInt(INK));
  GuiSetStyle(BUTTON, TEXT_COLOR_FOCUSED, ColorToInt(PAPER));
  GuiSetStyle(BUTTON, BASE_COLOR_PRESSED, ColorToInt(ACCENT));
  GuiSetStyle(BUTTON, BORDER_COLOR_PRESSED, ColorToInt(ACCENT));
  GuiSetStyle(BUTTON, TEXT_COLOR_PRESSED, ColorToInt(INK));

  GuiSetStyle(SLIDER, BASE_COLOR_PRESSED, ColorToInt(ACCENT));
  GuiSetStyle(SLIDER, TEXT_COLOR_FOCUSED, ColorToInt(ACCENT));
  GuiSetStyle(SLIDER, TEXT_COLOR_PRESSED, ColorToInt(ACCENT));
  GuiSetStyle(SLIDER, SLIDER_PADDING, 3);

  GuiSetStyle(DROPDOWNBOX, TEXT_ALIGNMENT, TEXT_ALIGN_LEFT);
  GuiSetStyle(DROPDOWNBOX, TEXT_PADDING, 8);
}

void
render_dropdown (void) {
  int previous = dropdown_active;
  if (GuiDropdownBox((Rectangle){MARGIN, TOOLBAR_Y, 260, CELL_HEIGHT}, midi_output_labels, &dropdown_active, dropdown_edit_mode)) {
    dropdown_edit_mode = !dropdown_edit_mode;
  }
  if (dropdown_active != previous) select_midi_output(dropdown_active);
}

void
render_header (void) {
  DrawRectangle(0, 0, WINDOW_WIDTH, HEADER_HEIGHT, INK);

  double phase = (Pt_Time() - loop.loop_start) / loop_length_ms();
  int beat = phase * beats_per_loop;
  for (int k = 0; k < beats_per_loop; k++) {
    int x = WINDOW_WIDTH - MARGIN - 8 - (beats_per_loop - 1 - k) * 14;
    DrawRectangle(x, 14, 8, 8, transport_playing && k == beat ? ACCENT : INK_LIGHT);
  }
}

void
render_toolbar (void) {
  if (GuiButton((Rectangle){CHANNEL_X, TOOLBAR_Y, 100, CELL_HEIGHT}, transport_playing ? "STOP" : "PLAY")) toggle_transport();

  float bpm_value = bpm;
  GuiSliderBar((Rectangle){GRID_X, TOOLBAR_Y, 200, CELL_HEIGHT}, "BPM", TextFormat("%03d", (int) bpm), &bpm_value, MIN_BPM, MAX_BPM);
  if ((int) bpm_value != (int) bpm) set_bpm((int) bpm_value);

  if (GuiButton((Rectangle){WINDOW_WIDTH - MARGIN - 180, TOOLBAR_Y, 85, CELL_HEIGHT}, "RANDOMIZE")) randomize_tracks_seqs();
  if (GuiButton((Rectangle){WINDOW_WIDTH - MARGIN - 85, TOOLBAR_Y, 85, CELL_HEIGHT}, "CLEAR")) clear_tracks_seqs();
}

void
render_track (Track *track) {
  float y = GRID_Y + track->id * ROW_HEIGHT;

  DrawText(TextFormat("%02d", track->id + 1), MARGIN, y + 4, 20, INK);
  DrawText(note_name(track->midi_note), 52, y + 9, 10, MUTED);

  int note = track->midi_note;
  GuiSpinner((Rectangle){90, y, 100, CELL_HEIGHT}, NULL, &note, 0, 127, false);
  if (note != (int) track->midi_note) set_track_note(track, note);

  int steps = track->steps;
  GuiSpinner((Rectangle){200, y, 100, CELL_HEIGHT}, NULL, &steps, 1, MAX_STEPS, false);
  if (steps != (int) track->steps) set_track_steps(track, steps);

  int channel = track->midi_channel;
  GuiSpinner((Rectangle){CHANNEL_X, y, 100, CELL_HEIGHT}, NULL, &channel, 1, MIDI_CHANNELS, false);
  if (channel != (int) track->midi_channel) set_track_channel(track, channel);

  unsigned int playing_step = (track->current_step + track->steps - 1) % track->steps;
  for (unsigned int j = 0; j < track->steps; j++) {
    int x0 = GRID_X + (j * GRID_WIDTH) / track->steps;
    int x1 = GRID_X + ((j + 1) * GRID_WIDTH) / track->steps;
    Rectangle cell = {x0, y, x1 - x0 - GRID_GAP, CELL_HEIGHT};

    bool on = bit_by_position(track->sequence, j);
    if (GuiToggle(cell, NULL, &on)) {
      track->sequence ^= (uint64_t) 1 << j;
      selected_step = (track->id * MAX_STEPS) + j;
    }

    if (transport_playing && j == playing_step) DrawRectangleRec(cell, on ? ACCENT : Fade(ACCENT, 0.25f));
    if (track->id * MAX_STEPS + j == (unsigned int) selected_step) DrawRectangleLinesEx(cell, 2, ACCENT);
  }

  if (track->id < TRACKS_LENGTH - 1) DrawRectangle(MARGIN, y + ROW_HEIGHT - 5, WINDOW_WIDTH - 2 * MARGIN, 1, RULE);
}

void
render_step (void) {
  Step *step = &steps[selected_step];
  const Track *track = &tracks[selected_step / MAX_STEPS];

  DrawText("STEP", MARGIN, STEP_LABELS_Y, 10, MUTED);
  DrawText("DETUNE", 90, STEP_LABELS_Y, 10, MUTED);
  DrawText("VELOCITY", 200, STEP_LABELS_Y, 10, MUTED);
  DrawRectangle(MARGIN, STEP_Y - 8, WINDOW_WIDTH - 2 * MARGIN, 2, INK);

  DrawText(TextFormat("%02d", selected_step % MAX_STEPS + 1), MARGIN, STEP_Y + 4, 20, INK);
  DrawText(note_name(track->midi_note + step->detune), 52, STEP_Y + 9, 10, MUTED);

  int detune = step->detune;
  GuiSpinner((Rectangle){90, STEP_Y, 100, CELL_HEIGHT}, NULL, &detune, -36, 36, false);
  if (detune != step->detune) step->detune = detune;

  int velocity = step->velocity;
  GuiSpinner((Rectangle){200, STEP_Y, 100, CELL_HEIGHT}, NULL, &velocity, 1, 127, false);
  if (velocity != (int) step->velocity) step->velocity = velocity;
}

void
render_tracks (void) {
  DrawText("NO", MARGIN, LABELS_Y, 10, MUTED);
  DrawText("NOTE", 90, LABELS_Y, 10, MUTED);
  DrawText("STEPS", 200, LABELS_Y, 10, MUTED);
  DrawText("CH", CHANNEL_X, LABELS_Y, 10, MUTED);
  for (int k = 0; k < beats_per_loop; k++) {
    DrawText(TextFormat("%d", k + 1), GRID_X + k * GRID_WIDTH / beats_per_loop, LABELS_Y, 10, MUTED);
  }
  DrawRectangle(MARGIN, GRID_Y - 8, WINDOW_WIDTH - 2 * MARGIN, 2, INK);

  for (int i = 0; i < TRACKS_LENGTH; i++) {
    render_track(&tracks[i]);
  }
}

void
render_footer (void) {
  const char *output = dropdown_active > 0 ? midi_output_devices[dropdown_active - 1].name : "NONE";
  const char *info = TextFormat("%d/4  %.0f MS / LOOP", beats_per_loop, loop_length_ms());

  DrawRectangle(MARGIN, FOOTER_Y, WINDOW_WIDTH - 2 * MARGIN, 1, INK);
  DrawRectangle(MARGIN, FOOTER_Y + 11, 6, 6, midi_output_stream ? ACCENT : MUTED);
  DrawText(TextFormat("OUT  %s", TextToUpper(output)), MARGIN + 12, FOOTER_Y + 10, 10, INK);
  DrawText(info, WINDOW_WIDTH - MARGIN - MeasureText(info, 10), FOOTER_Y + 10, 10, MUTED);
}

int
main (void) {
  InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "SEQ/08");
  SetTargetFPS(60);
  setup_style();
  Pm_Initialize();

  init_tracks();
  init_steps();

  list_midi_devices();
  randomize_tracks_seqs();

  if (Pt_Start(CLOCK_RESOLUTION_MS, tick, NULL) != ptNoError) {
    CloseWindow();
    return 1;
  }

  while (!WindowShouldClose()) {
    BeginDrawing();
    ClearBackground(PAPER);

    pthread_mutex_lock(&lock);

    if (IsKeyPressed(KEY_SPACE)) toggle_transport();

    render_header();
    render_footer();
    if (dropdown_edit_mode) GuiLock();
    render_toolbar();
    render_tracks();
    render_step();
    GuiUnlock();
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
