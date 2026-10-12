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
#define VOICES_PER_TRACK    8
#define VOICES_LENGTH       (TRACKS_LENGTH * VOICES_PER_TRACK)
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
#define NUMBER_WIDTH  28
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

// A voice is one row of steps with its own note and length. Each track owns
// VOICES_PER_TRACK voices that all play on the track's channel; voice 0 is the
// row shown in the main view, the rest are edited in the track's focus view.
typedef struct Voice {
  unsigned int id; // global index, track = id / VOICES_PER_TRACK
  unsigned int steps;
  unsigned int midi_note;
  unsigned int current_step;
  uint64_t sequence;
  bool playing;
  unsigned int playing_note;    // note and channel of the sounding note, so note off matches it
  unsigned int playing_channel; // even if detune, note or channel changed since note on
  bool note_edited;             // untouched voices follow the track note + offset when focused
  bool muted;
  bool soloed;
} Voice;

typedef struct Track {
  unsigned int id;
  unsigned int midi_channel; // 1-16
  bool muted;
  bool soloed;
} Track;

typedef struct Step {
  signed int detune;
  unsigned int velocity;
} Step;

double bpm = 120.0; // guarded by lock
const int beats_per_loop = 4;
bool running = true;            // guarded by lock; once false, tick never touches MIDI again
bool transport_playing = false; // guarded by lock; play/stop state

Loop loop;
Track tracks[TRACKS_LENGTH];
Voice voices[VOICES_LENGTH];
Step steps[VOICES_LENGTH * MAX_STEPS];

int dropdown_active = 0;
bool dropdown_edit_mode = false;

int selected_step = 0;
int focused_track = -1; // -1 shows all tracks, otherwise the voices of this track

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

Track *
voice_track (const Voice *voice) {
  return &tracks[voice->id / VOICES_PER_TRACK];
}

Voice *
track_voice (unsigned int track, unsigned int index) {
  return &voices[track * VOICES_PER_TRACK + index];
}

bool
track_audible (const Track *track) {
  bool any_solo = false;
  for (size_t i = 0; i < TRACKS_LENGTH; i++) any_solo |= tracks[i].soloed;
  return !track->muted && (!any_solo || track->soloed);
}

bool
voice_audible_in_track (const Voice *voice) {
  unsigned int track = voice->id / VOICES_PER_TRACK;
  bool any_solo = false;
  for (size_t i = 0; i < VOICES_PER_TRACK; i++) any_solo |= track_voice(track, i)->soloed;
  return !voice->muted && (!any_solo || voice->soloed);
}

bool
voice_audible (const Voice *voice) {
  return track_audible(voice_track(voice)) && voice_audible_in_track(voice);
}

void
randomize_voices (unsigned int first, unsigned int count, unsigned int stride) {
  for (unsigned int i = 0; i < count; i++) {
    Voice *voice = &voices[first + i * stride];
    voice->sequence = rand_u64() & steps_mask(voice->steps);
  }
}

void
clear_voices (unsigned int first, unsigned int count) {
  for (unsigned int i = 0; i < count; i++) {
    voices[first + i].sequence = 0;
  }
}

void
init_tracks (void) {
  for (size_t i = 0; i < TRACKS_LENGTH; i++) {
    tracks[i].id = i;
    tracks[i].midi_channel = 1;
  }
  for (size_t i = 0; i < VOICES_LENGTH; i++) {
    voices[i].id = i;
    voices[i].steps = 16;
    voices[i].midi_note = 40 + i / VOICES_PER_TRACK + i % VOICES_PER_TRACK;
  }
}

void
init_steps (void) {
  for (size_t i = 0; i < VOICES_LENGTH * MAX_STEPS; i++) {
    steps[i].detune = 0;
    steps[i].velocity = 100;
  }
}

void
release_voice (Voice *voice) {
  if (voice->playing) {
    send_midi_note_off(midi_output_stream, voice->playing_channel - 1, voice->playing_note);
    voice->playing = false;
  }
}

void
release_all_voices (void) {
  for (size_t i = 0; i < VOICES_LENGTH; i++) {
    release_voice(&voices[i]);
  }
}

void
release_inaudible_voices (void) {
  for (size_t i = 0; i < VOICES_LENGTH; i++) {
    if (!voice_audible(&voices[i])) release_voice(&voices[i]);
  }
}

void
run_step (Voice *voice) {
  release_voice(voice);
  if (bit_by_position(voice->sequence, voice->current_step) && voice_audible(voice)) {
    signed int detune = steps[(voice->id * MAX_STEPS) + voice->current_step].detune;
    unsigned int velocity = steps[(voice->id * MAX_STEPS) + voice->current_step].velocity;
    voice->playing_note = voice->midi_note + detune;
    voice->playing_channel = voice_track(voice)->midi_channel;
    send_midi_note_on(midi_output_stream, voice->playing_channel - 1, voice->playing_note, velocity);
    voice->playing = true;
  }
  voice->current_step++;
}

void
sync_voices (void) {
  for (size_t i = 0; i < VOICES_LENGTH; i++) {
    voices[i].current_step = 0;
    run_step(&voices[i]);
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
set_voice_steps (Voice *voice, unsigned int steps) {
  double step_length = loop_length_ms() / steps;
  unsigned int step = (Pt_Time() - loop.loop_start) / step_length;
  voice->steps = steps;
  voice->current_step = step + 1;
}

void
set_voice_note (Voice *voice, unsigned int note) {
  release_voice(voice);
  voice->midi_note = note;
  voice->note_edited = true;
}

void
set_track_channel (Track *track, unsigned int channel) {
  for (size_t i = 0; i < VOICES_PER_TRACK; i++) release_voice(track_voice(track->id, i));
  track->midi_channel = channel;
}

void
toggle_audibility_flag (bool *flag) {
  *flag = !*flag;
  release_inaudible_voices();
}

void
open_focus (unsigned int track) {
  Voice *root = track_voice(track, 0);
  for (unsigned int i = 1; i < VOICES_PER_TRACK; i++) {
    Voice *voice = track_voice(track, i);
    if (voice->note_edited) continue;
    unsigned int note = root->midi_note + i;
    voice->midi_note = note > 127 ? 127 : note;
  }
  focused_track = track;
  selected_step = root->id * MAX_STEPS + selected_step % MAX_STEPS;
}

void
close_focus (void) {
  selected_step = track_voice(focused_track, 0)->id * MAX_STEPS + selected_step % MAX_STEPS;
  focused_track = -1;
}

void
toggle_focus (void) {
  if (focused_track >= 0) {
    close_focus();
  } else {
    open_focus(selected_step / MAX_STEPS / VOICES_PER_TRACK);
  }
}

void
play (void) {
  loop.loop_start = Pt_Time();
  transport_playing = true;
  sync_voices();
}

void
stop (void) {
  transport_playing = false;
  release_all_voices();
  for (size_t i = 0; i < VOICES_LENGTH; i++) {
    voices[i].current_step = 0;
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
      sync_voices();
    } else {
      for (size_t i = 0; i < VOICES_LENGTH; i++) {
        double step_length = loop_ms / (double) voices[i].steps;
        int loop_current_step = elapsed / step_length;
        if (voices[i].current_step <= loop_current_step) {
          voices[i].current_step = loop_current_step;
          run_step(&voices[i]);
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
  release_all_voices();
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
  release_all_voices();
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

bool
command_down (void) {
  return IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
}

bool
option_down (void) {
  return IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);
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

  if (focused_track >= 0) {
    const Track *track = &tracks[focused_track];
    DrawText(TextFormat("FOCUS  TRACK %02d  CH %d%s", track->id + 1, track->midi_channel, track_audible(track) ? "" : "  (MUTED)"), MARGIN, 13, 10, PAPER);
  }

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
  GuiSliderBar((Rectangle){GRID_X, TOOLBAR_Y, 150, CELL_HEIGHT}, "BPM", TextFormat("%03d", (int) bpm), &bpm_value, MIN_BPM, MAX_BPM);
  if ((int) bpm_value != (int) bpm) set_bpm((int) bpm_value);

  if (GuiButton((Rectangle){WINDOW_WIDTH - MARGIN - 250, TOOLBAR_Y, 80, CELL_HEIGHT}, focused_track >= 0 ? "CLOSE FOCUS" : "FOCUS")) toggle_focus();

  if (GuiButton((Rectangle){WINDOW_WIDTH - MARGIN - 165, TOOLBAR_Y, 80, CELL_HEIGHT}, "RANDOMIZE")) {
    if (focused_track >= 0) {
      randomize_voices(track_voice(focused_track, 0)->id, VOICES_PER_TRACK, 1);
    } else {
      randomize_voices(0, TRACKS_LENGTH, VOICES_PER_TRACK);
    }
  }
  if (GuiButton((Rectangle){WINDOW_WIDTH - MARGIN - 80, TOOLBAR_Y, 80, CELL_HEIGHT}, "CLEAR")) {
    if (focused_track >= 0) {
      clear_voices(track_voice(focused_track, 0)->id, VOICES_PER_TRACK);
    } else {
      clear_voices(0, VOICES_LENGTH);
    }
  }
}

// Row number: click mutes, cmd-click solos, opt-click opens (main view) or closes (focus view) focus.
// Rows that will not sound are drawn inverted; soloed rows are drawn in the accent color.
void
render_number (Rectangle bounds, unsigned int number, bool *muted, bool *soloed, bool audible, int focus_target) {
  if (!GuiIsLocked() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(GetMousePosition(), bounds)) {
    if (option_down()) {
      if (focus_target >= 0) {
        open_focus(focus_target);
      } else {
        close_focus();
      }
      return;
    }
    if (command_down()) {
      toggle_audibility_flag(soloed);
    } else {
      toggle_audibility_flag(muted);
    }
  }

  if (!audible) DrawRectangleRec(bounds, INK);
  Color color = !audible ? PAPER : *soloed ? ACCENT : INK;
  DrawText(TextFormat("%02d", number), bounds.x + 3, bounds.y + 4, 20, color);
}

// Voices 1-7 of a track drawn as thin lanes over its main view row, one lane per voice.
void
render_voice_lanes (unsigned int track, float y) {
  for (unsigned int i = 1; i < VOICES_PER_TRACK; i++) {
    const Voice *voice = track_voice(track, i);
    if (!voice->sequence) continue;
    float lane_y = y + 3 + (i - 1) * 3;
    Color color = voice_audible_in_track(voice) ? ACCENT : Fade(ACCENT, 0.35f);
    for (unsigned int j = 0; j < voice->steps; j++) {
      if (!bit_by_position(voice->sequence, j)) continue;
      int x0 = GRID_X + (j * GRID_WIDTH) / voice->steps;
      int x1 = GRID_X + ((j + 1) * GRID_WIDTH) / voice->steps;
      DrawRectangle(x0, lane_y, x1 - x0 - GRID_GAP, 2, color);
    }
  }
}

unsigned int
extra_voices_in_use (unsigned int track) {
  unsigned int count = 0;
  for (unsigned int i = 1; i < VOICES_PER_TRACK; i++) count += track_voice(track, i)->sequence != 0;
  return count;
}

void
render_row (Voice *voice, unsigned int row) {
  float y = GRID_Y + row * ROW_HEIGHT;
  Track *track = voice_track(voice);
  Rectangle number_bounds = {MARGIN - 3, y, NUMBER_WIDTH, CELL_HEIGHT};

  if (focused_track >= 0) {
    render_number(number_bounds, row + 1, &voice->muted, &voice->soloed, voice_audible_in_track(voice), -1);
  } else {
    render_number(number_bounds, row + 1, &track->muted, &track->soloed, track_audible(track), track->id);
  }

  DrawText(note_name(voice->midi_note), 52, y + 9, 10, MUTED);
  unsigned int extra = focused_track >= 0 ? 0 : extra_voices_in_use(track->id);
  if (extra) DrawText(TextFormat("+%u", extra), 52, y + 20, 10, ACCENT);

  int note = voice->midi_note;
  GuiSpinner((Rectangle){90, y, 100, CELL_HEIGHT}, NULL, &note, 0, 127, false);
  if (note != (int) voice->midi_note) set_voice_note(voice, note);

  int steps = voice->steps;
  GuiSpinner((Rectangle){200, y, 100, CELL_HEIGHT}, NULL, &steps, 1, MAX_STEPS, false);
  if (steps != (int) voice->steps) set_voice_steps(voice, steps);

  int channel = track->midi_channel;
  GuiSpinner((Rectangle){CHANNEL_X, y, 100, CELL_HEIGHT}, NULL, &channel, 1, MIDI_CHANNELS, false);
  if (channel != (int) track->midi_channel) set_track_channel(track, channel);

  unsigned int playing_step = (voice->current_step + voice->steps - 1) % voice->steps;
  for (unsigned int j = 0; j < voice->steps; j++) {
    int x0 = GRID_X + (j * GRID_WIDTH) / voice->steps;
    int x1 = GRID_X + ((j + 1) * GRID_WIDTH) / voice->steps;
    Rectangle cell = {x0, y, x1 - x0 - GRID_GAP, CELL_HEIGHT};

    bool on = bit_by_position(voice->sequence, j);
    if (GuiToggle(cell, NULL, &on)) {
      voice->sequence ^= (uint64_t) 1 << j;
      selected_step = (voice->id * MAX_STEPS) + j;
    }

    if (transport_playing && j == playing_step) DrawRectangleRec(cell, on ? ACCENT : Fade(ACCENT, 0.25f));
    if (voice->id * MAX_STEPS + j == (unsigned int) selected_step) DrawRectangleLinesEx(cell, 2, ACCENT);
  }

  if (focused_track < 0) render_voice_lanes(track->id, y);

  if (row < TRACKS_LENGTH - 1) DrawRectangle(MARGIN, y + ROW_HEIGHT - 5, WINDOW_WIDTH - 2 * MARGIN, 1, RULE);
}

void
render_step (void) {
  Step *step = &steps[selected_step];
  const Voice *voice = &voices[selected_step / MAX_STEPS];

  DrawText("STEP", MARGIN, STEP_LABELS_Y, 10, MUTED);
  DrawText("DETUNE", 90, STEP_LABELS_Y, 10, MUTED);
  DrawText("VELOCITY", 200, STEP_LABELS_Y, 10, MUTED);
  DrawRectangle(MARGIN, STEP_Y - 8, WINDOW_WIDTH - 2 * MARGIN, 2, INK);

  DrawText(TextFormat("%02d", selected_step % MAX_STEPS + 1), MARGIN, STEP_Y + 4, 20, INK);
  DrawText(note_name(voice->midi_note + step->detune), 52, STEP_Y + 9, 10, MUTED);

  int detune = step->detune;
  GuiSpinner((Rectangle){90, STEP_Y, 100, CELL_HEIGHT}, NULL, &detune, -36, 36, false);
  if (detune != step->detune) step->detune = detune;

  int velocity = step->velocity;
  GuiSpinner((Rectangle){200, STEP_Y, 100, CELL_HEIGHT}, NULL, &velocity, 1, 127, false);
  if (velocity != (int) step->velocity) step->velocity = velocity;

  const char *hint = focused_track >= 0 ? "CLICK NO: MUTE   CMD-CLICK: SOLO   OPT-CLICK / F: CLOSE FOCUS   SPACE: PLAY"
                                        : "CLICK NO: MUTE   CMD-CLICK: SOLO   OPT-CLICK / F: FOCUS   SPACE: PLAY";
  DrawText(hint, WINDOW_WIDTH - MARGIN - MeasureText(hint, 10), STEP_Y + 9, 10, MUTED);
}

void
render_tracks (void) {
  DrawText(focused_track >= 0 ? "VOICE" : "NO", MARGIN, LABELS_Y, 10, MUTED);
  DrawText("NOTE", 90, LABELS_Y, 10, MUTED);
  DrawText("STEPS", 200, LABELS_Y, 10, MUTED);
  DrawText("CH", CHANNEL_X, LABELS_Y, 10, MUTED);
  for (int k = 0; k < beats_per_loop; k++) {
    DrawText(TextFormat("%d", k + 1), GRID_X + k * GRID_WIDTH / beats_per_loop, LABELS_Y, 10, MUTED);
  }
  DrawRectangle(MARGIN, GRID_Y - 8, WINDOW_WIDTH - 2 * MARGIN, 2, INK);

  for (unsigned int row = 0; row < TRACKS_LENGTH; row++) {
    if (focused_track >= 0) {
      render_row(track_voice(focused_track, row), row);
    } else {
      render_row(track_voice(row, 0), row);
    }
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
  randomize_voices(0, TRACKS_LENGTH, VOICES_PER_TRACK);

  if (Pt_Start(CLOCK_RESOLUTION_MS, tick, NULL) != ptNoError) {
    CloseWindow();
    return 1;
  }

  while (!WindowShouldClose()) {
    BeginDrawing();
    ClearBackground(PAPER);

    pthread_mutex_lock(&lock);

    if (!dropdown_edit_mode) {
      if (IsKeyPressed(KEY_SPACE)) toggle_transport();
      if (IsKeyPressed(KEY_F)) toggle_focus();
    }

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
