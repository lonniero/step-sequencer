#define RAYGUI_IMPLEMENTATION

#include <math.h>
#include <porttime.h>
#include <pthread.h>
#include <raygui.h>
#include <stdint.h>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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
#define PATTERNS_LENGTH     8
#define CHAIN_MAX_LENGTH    16
#define CHAIN_MAX_REPEATS   16

#define WINDOW_WIDTH  800
#define WINDOW_HEIGHT 564
#define MARGIN        20
#define HEADER_HEIGHT 36
#define TOOLBAR_Y     52
#define PATTERN_Y     92
#define LABELS_Y      142
#define NUMBER_WIDTH  28
#define GRID_X        330
#define GRID_Y        164
#define GRID_WIDTH    450
#define GRID_GAP      2
#define ROW_HEIGHT    38
#define CELL_HEIGHT   28
#define STEP_LABELS_Y 472
#define STEP_Y        494
#define FOOTER_Y      534

#define PATTERN_BUTTON_WIDTH 23
#define PATTERN_BUTTON_STEP  26
#define CHAIN_BUTTON_X       240
#define CHAIN_BAR_WIDTH      40

#define PAPER      (Color){232, 230, 223, 255}
#define PAPER_DARK (Color){214, 211, 202, 255}
#define INK        (Color){21, 21, 21, 255}
#define INK_LIGHT  (Color){58, 58, 58, 255}
#define MUTED      (Color){140, 138, 132, 255}
#define RULE       (Color){201, 198, 188, 255}
#define ACCENT     (Color){206, 112, 78, 255}

// Focus view palette: warm paper, accent rules and active steps.
#define FOCUS_PAPER      (Color){240, 212, 196, 255}
#define FOCUS_PAPER_DARK (Color){228, 192, 172, 255}

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

// A pattern is one bar of every voice's steps. The live voices and steps arrays
// hold the current pattern; switching stores them here and loads the next one.
// Notes, channels and mutes belong to the tracks and are not part of a pattern.
typedef struct Pattern {
  uint64_t sequence[VOICES_LENGTH];
  unsigned int steps[VOICES_LENGTH];
  Step step_data[VOICES_LENGTH * MAX_STEPS];
} Pattern;

typedef struct ChainEntry {
  unsigned int pattern;
  unsigned int repeats; // bars
} ChainEntry;

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

// All guarded by lock: tick switches patterns at the loop boundary.
Pattern patterns[PATTERNS_LENGTH];
int current_pattern = 0;
int queued_pattern = -1; // switches at the next loop boundary while playing
ChainEntry chain[CHAIN_MAX_LENGTH];
unsigned int chain_length = 0;
bool chain_mode = false;
unsigned int chain_position = 0;
unsigned int chain_bar = 0;     // bars already played of the current chain entry
bool chain_restart = false;     // start the chain from the top at the next loop boundary

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
    tracks[i].midi_channel = i + 1;
  }
  for (size_t i = 0; i < VOICES_LENGTH; i++) {
    voices[i].id = i;
    voices[i].steps = 16;
    voices[i].midi_note = 60 + i % VOICES_PER_TRACK;
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
init_patterns (void) {
  for (size_t p = 0; p < PATTERNS_LENGTH; p++) {
    for (size_t i = 0; i < VOICES_LENGTH; i++) {
      patterns[p].sequence[i] = 0;
      patterns[p].steps[i] = 16;
    }
    for (size_t i = 0; i < VOICES_LENGTH * MAX_STEPS; i++) {
      patterns[p].step_data[i] = (Step){.detune = 0, .velocity = 100};
    }
  }
}

void
store_pattern (unsigned int index) {
  Pattern *pattern = &patterns[index];
  for (size_t i = 0; i < VOICES_LENGTH; i++) {
    pattern->sequence[i] = voices[i].sequence;
    pattern->steps[i] = voices[i].steps;
  }
  for (size_t i = 0; i < VOICES_LENGTH * MAX_STEPS; i++) pattern->step_data[i] = steps[i];
}

void
load_pattern (unsigned int index) {
  const Pattern *pattern = &patterns[index];
  for (size_t i = 0; i < VOICES_LENGTH; i++) {
    voices[i].sequence = pattern->sequence[i];
    voices[i].steps = pattern->steps[i];
  }
  for (size_t i = 0; i < VOICES_LENGTH * MAX_STEPS; i++) steps[i] = pattern->step_data[i];
}

void
switch_pattern (unsigned int index) {
  if ((int) index == current_pattern) return;
  store_pattern(current_pattern);
  load_pattern(index);
  current_pattern = index;
}

bool
pattern_has_steps (unsigned int index) {
  for (size_t i = 0; i < VOICES_LENGTH; i++) {
    uint64_t sequence = (int) index == current_pattern ? voices[i].sequence : patterns[index].sequence[i];
    if (sequence) return true;
  }
  return false;
}

void
copy_current_pattern_to (unsigned int index) {
  if ((int) index == current_pattern) return;
  store_pattern(current_pattern);
  patterns[index] = patterns[current_pattern];
}

// Clicking a pattern leaves chain mode; while playing the switch waits for the bar to end.
void
select_pattern (unsigned int index) {
  chain_mode = false;
  if (!transport_playing) {
    switch_pattern(index);
    queued_pattern = -1;
  } else {
    queued_pattern = (int) index == current_pattern ? -1 : (int) index;
  }
}

void
start_chain (void) {
  chain_position = 0;
  chain_bar = 0;
  chain_restart = false;
  switch_pattern(chain[0].pattern);
}

void
toggle_chain_mode (void) {
  if (!chain_length) return;
  chain_mode = !chain_mode;
  queued_pattern = -1;
  if (!chain_mode) return;
  if (transport_playing) {
    chain_restart = true;
  } else {
    start_chain();
  }
}

void
append_to_chain (unsigned int pattern) {
  if (chain_length == CHAIN_MAX_LENGTH) return;
  chain[chain_length++] = (ChainEntry){.pattern = pattern, .repeats = 1};
}

void
remove_from_chain (unsigned int position) {
  for (unsigned int i = position; i + 1 < chain_length; i++) chain[i] = chain[i + 1];
  chain_length--;
  if (!chain_length) {
    chain_mode = false;
  } else if (chain_position >= chain_length) {
    chain_position = 0;
    chain_bar = 0;
  }
}

// Called at every loop boundary before the voices restart.
void
advance_pattern (void) {
  if (chain_mode && chain_length) {
    if (chain_restart) {
      start_chain();
      return;
    }
    if (++chain_bar >= chain[chain_position].repeats) {
      chain_bar = 0;
      chain_position = (chain_position + 1) % chain_length;
    }
    switch_pattern(chain[chain_position].pattern);
  } else if (queued_pattern >= 0) {
    switch_pattern(queued_pattern);
    queued_pattern = -1;
  }
}

void
play (void) {
  if (chain_mode && chain_length) {
    start_chain();
  } else if (queued_pattern >= 0) {
    switch_pattern(queued_pattern);
    queued_pattern = -1;
  }
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
      advance_pattern();
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

// Called with the palette swapped to draw muted rows inverted.
void
setup_style (Color paper, Color paper_dark, Color ink, Color step_on) {
  GuiSetStyle(DEFAULT, BACKGROUND_COLOR, ColorToInt(paper));
  GuiSetStyle(DEFAULT, LINE_COLOR, ColorToInt(ink));
  GuiSetStyle(DEFAULT, TEXT_SIZE, 10);
  GuiSetStyle(DEFAULT, TEXT_SPACING, 1);
  GuiSetStyle(DEFAULT, BORDER_WIDTH, 1);

  GuiSetStyle(DEFAULT, BORDER_COLOR_NORMAL, ColorToInt(ink));
  GuiSetStyle(DEFAULT, BASE_COLOR_NORMAL, ColorToInt(paper));
  GuiSetStyle(DEFAULT, TEXT_COLOR_NORMAL, ColorToInt(ink));
  GuiSetStyle(DEFAULT, BORDER_COLOR_FOCUSED, ColorToInt(ink));
  GuiSetStyle(DEFAULT, BASE_COLOR_FOCUSED, ColorToInt(paper_dark));
  GuiSetStyle(DEFAULT, TEXT_COLOR_FOCUSED, ColorToInt(ink));
  GuiSetStyle(DEFAULT, BORDER_COLOR_PRESSED, ColorToInt(ink));
  GuiSetStyle(DEFAULT, BASE_COLOR_PRESSED, ColorToInt(ink));
  GuiSetStyle(DEFAULT, TEXT_COLOR_PRESSED, ColorToInt(paper));
  GuiSetStyle(DEFAULT, BORDER_COLOR_DISABLED, ColorToInt(MUTED));
  GuiSetStyle(DEFAULT, BASE_COLOR_DISABLED, ColorToInt(paper));
  GuiSetStyle(DEFAULT, TEXT_COLOR_DISABLED, ColorToInt(MUTED));

  GuiSetStyle(LABEL, TEXT_COLOR_FOCUSED, ColorToInt(ink));
  GuiSetStyle(LABEL, TEXT_COLOR_PRESSED, ColorToInt(ink));

  GuiSetStyle(BUTTON, BASE_COLOR_FOCUSED, ColorToInt(ink));
  GuiSetStyle(BUTTON, TEXT_COLOR_FOCUSED, ColorToInt(paper));
  GuiSetStyle(BUTTON, BASE_COLOR_PRESSED, ColorToInt(ACCENT));
  GuiSetStyle(BUTTON, BORDER_COLOR_PRESSED, ColorToInt(ACCENT));
  GuiSetStyle(BUTTON, TEXT_COLOR_PRESSED, ColorToInt(ink));

  GuiSetStyle(SLIDER, BASE_COLOR_PRESSED, ColorToInt(ACCENT));
  GuiSetStyle(SLIDER, TEXT_COLOR_FOCUSED, ColorToInt(ACCENT));
  GuiSetStyle(SLIDER, TEXT_COLOR_PRESSED, ColorToInt(ACCENT));
  GuiSetStyle(SLIDER, SLIDER_PADDING, 3);

  GuiSetStyle(DROPDOWNBOX, TEXT_ALIGNMENT, TEXT_ALIGN_LEFT);
  GuiSetStyle(DROPDOWNBOX, TEXT_PADDING, 8);

  GuiSetStyle(TOGGLE, BASE_COLOR_PRESSED, ColorToInt(step_on));
}

bool
in_focus (void) {
  return focused_track >= 0;
}

Color
background_color (void) {
  return in_focus() ? FOCUS_PAPER : PAPER;
}

// Heavy rules and the header bar turn accent in focus view.
Color
rule_color (void) {
  return in_focus() ? ACCENT : INK;
}

void
apply_style (bool inverted) {
  Color paper = background_color();
  Color paper_dark = in_focus() ? FOCUS_PAPER_DARK : PAPER_DARK;
  if (inverted) {
    setup_style(INK, INK_LIGHT, paper, in_focus() ? ACCENT : paper);
  } else {
    setup_style(paper, paper_dark, INK, in_focus() ? ACCENT : INK);
  }
}

bool
command_down (void) {
  return IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
}

bool
option_down (void) {
  return IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);
}

// The channel and note spinners share a column; holding cmd swaps them.
// Tracks show the channel by default, focused voices (which share a channel) the note.
bool
show_note_column (void) {
  return in_focus() != command_down();
}

void
render_dropdown (void) {
  int previous = dropdown_active;
  if (GuiDropdownBox((Rectangle){MARGIN, TOOLBAR_Y, 180, CELL_HEIGHT}, midi_output_labels, &dropdown_active, dropdown_edit_mode)) {
    dropdown_edit_mode = !dropdown_edit_mode;
  }
  if (dropdown_active != previous) select_midi_output(dropdown_active);
}

// Projects are plain text: a magic/version line, then one record per line.
// Unknown or out-of-range records are skipped, so older files keep loading.
#define PROJECT_MAGIC    "SEQ08"
#define PROJECT_VERSION  1
#define AUTOSAVE_SECONDS 30

typedef enum FileAction {
  FILE_ACTION_NONE,
  FILE_ACTION_SAVE,
  FILE_ACTION_SAVE_AS,
  FILE_ACTION_OPEN,
} FileAction;

char project_path[PATH_MAX] = ""; // empty until saved or opened
FileAction pending_file_action = FILE_ACTION_NONE; // run after the frame, outside the lock
char status_message[128] = "";
double status_until = 0;

void
set_status (const char *message) {
  snprintf(status_message, sizeof status_message, "%s", message);
  status_until = GetTime() + 3;
}

const char *
path_basename (const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

void
autosave_path (char *out, size_t size) {
  const char *home = getenv("HOME");
  snprintf(out, size, "%s/Library/Application Support/StepSequencer", home ? home : ".");
  mkdir(out, 0755);
  strlcat(out, "/last-session.seq", size);
}

bool
write_project (const char *path) {
  store_pattern(current_pattern);
  FILE *file = fopen(path, "w");
  if (!file) return false;

  fprintf(file, "%s %d\n", PROJECT_MAGIC, PROJECT_VERSION);
  fprintf(file, "bpm %d\n", (int) bpm);
  fprintf(file, "current_pattern %d\n", current_pattern);
  fprintf(file, "chain_mode %d\n", chain_mode);
  if (project_path[0]) fprintf(file, "project_path %s\n", project_path);
  if (dropdown_active > 0) fprintf(file, "output %s\n", midi_output_devices[dropdown_active - 1].name);
  for (size_t i = 0; i < TRACKS_LENGTH; i++) {
    fprintf(file, "track %zu %u %d %d\n", i, tracks[i].midi_channel, tracks[i].muted, tracks[i].soloed);
  }
  for (size_t i = 0; i < VOICES_LENGTH; i++) {
    const Voice *voice = &voices[i];
    fprintf(file, "voice %zu %u %d %d %d\n", i, voice->midi_note, voice->note_edited, voice->muted, voice->soloed);
  }
  for (size_t p = 0; p < PATTERNS_LENGTH; p++) {
    const Pattern *pattern = &patterns[p];
    for (size_t i = 0; i < VOICES_LENGTH; i++) {
      if (pattern->sequence[i] || pattern->steps[i] != 16) {
        fprintf(file, "pattern %zu %zu %u %llx\n", p, i, pattern->steps[i], (unsigned long long) pattern->sequence[i]);
      }
    }
    for (size_t k = 0; k < VOICES_LENGTH * MAX_STEPS; k++) {
      const Step *step = &pattern->step_data[k];
      if (step->detune || step->velocity != 100) fprintf(file, "step %zu %zu %d %u\n", p, k, step->detune, step->velocity);
    }
  }
  for (unsigned int i = 0; i < chain_length; i++) {
    fprintf(file, "chain %u %u\n", chain[i].pattern, chain[i].repeats);
  }

  bool ok = !ferror(file);
  return fclose(file) == 0 && ok;
}

void
reset_project (void) {
  memset(tracks, 0, sizeof tracks);
  memset(voices, 0, sizeof voices);
  init_tracks();
  init_steps();
  init_patterns();
  current_pattern = 0;
  queued_pattern = -1;
  chain_length = 0;
  chain_mode = false;
  chain_position = 0;
  chain_bar = 0;
  chain_restart = false;
  focused_track = -1;
  selected_step = 0;
}

void
select_output_by_name (const char *name) {
  for (int i = 0; i < midi_output_count; i++) {
    if (strcmp(midi_output_devices[i].name, name) == 0) {
      dropdown_active = i + 1;
      select_midi_output(dropdown_active);
      return;
    }
  }
}

// restore_session also restores which project file the session belonged to.
bool
read_project (const char *path, bool restore_session) {
  FILE *file = fopen(path, "r");
  if (!file) return false;

  char line[PATH_MAX + 32];
  char magic[16];
  int version;
  if (!fgets(line, sizeof line, file) || sscanf(line, "%15s %d", magic, &version) != 2 ||
      strcmp(magic, PROJECT_MAGIC) != 0 || version > PROJECT_VERSION) {
    fclose(file);
    return false;
  }

  stop();
  reset_project();
  project_path[0] = '\0';
  char output[256] = "";

  while (fgets(line, sizeof line, file)) {
    line[strcspn(line, "\n")] = '\0';
    unsigned int a, b, c, d, e;
    int n;
    unsigned long long sequence;

    if (sscanf(line, "bpm %d", &n) == 1) {
      if (n >= MIN_BPM && n <= MAX_BPM) bpm = n;
    } else if (sscanf(line, "current_pattern %u", &a) == 1) {
      if (a < PATTERNS_LENGTH) current_pattern = a;
    } else if (sscanf(line, "chain_mode %u", &a) == 1) {
      chain_mode = a;
    } else if (strncmp(line, "project_path ", 13) == 0) {
      if (restore_session) snprintf(project_path, sizeof project_path, "%s", line + 13);
    } else if (strncmp(line, "output ", 7) == 0) {
      snprintf(output, sizeof output, "%s", line + 7);
    } else if (sscanf(line, "track %u %u %u %u", &a, &b, &c, &d) == 4) {
      if (a < TRACKS_LENGTH && b >= 1 && b <= MIDI_CHANNELS) {
        tracks[a].midi_channel = b;
        tracks[a].muted = c;
        tracks[a].soloed = d;
      }
    } else if (sscanf(line, "voice %u %u %u %u %u", &a, &b, &c, &d, &e) == 5) {
      if (a < VOICES_LENGTH && b <= 127) {
        voices[a].midi_note = b;
        voices[a].note_edited = c;
        voices[a].muted = d;
        voices[a].soloed = e;
      }
    } else if (sscanf(line, "pattern %u %u %u %llx", &a, &b, &c, &sequence) == 4) {
      if (a < PATTERNS_LENGTH && b < VOICES_LENGTH && c >= 1 && c <= MAX_STEPS) {
        patterns[a].steps[b] = c;
        patterns[a].sequence[b] = sequence & steps_mask(c);
      }
    } else if (sscanf(line, "step %u %u %d %u", &a, &b, &n, &c) == 4) {
      if (a < PATTERNS_LENGTH && b < VOICES_LENGTH * MAX_STEPS && n >= -36 && n <= 36 && c >= 1 && c <= 127) {
        patterns[a].step_data[b] = (Step){.detune = n, .velocity = c};
      }
    } else if (sscanf(line, "chain %u %u", &a, &b) == 2) {
      if (a < PATTERNS_LENGTH && b >= 1 && b <= CHAIN_MAX_REPEATS && chain_length < CHAIN_MAX_LENGTH) {
        chain[chain_length++] = (ChainEntry){.pattern = a, .repeats = b};
      }
    }
  }
  fclose(file);

  load_pattern(current_pattern);
  if (!chain_length) chain_mode = false;
  if (output[0]) select_output_by_name(output);
  if (!restore_session) snprintf(project_path, sizeof project_path, "%s", path);
  return true;
}

void
autosave (void) {
  char path[PATH_MAX];
  autosave_path(path, sizeof path);
  write_project(path);
}

// Native file dialogs via AppleScript; blocks the UI thread but not playback.
// Returns false when cancelled.
bool
choose_path (bool save, char *out, size_t size) {
  char command[PATH_MAX + 256];
  if (save) {
    const char *name = project_path[0] ? path_basename(project_path) : "Untitled.seq";
    if (strpbrk(name, "\"\\'")) name = "Untitled.seq";
    snprintf(command, sizeof command,
             "/usr/bin/osascript -e 'activate' -e 'POSIX path of (choose file name with prompt \"Save sequencer project\" default name \"%s\")' 2>/dev/null",
             name);
  } else {
    snprintf(command, sizeof command, "/usr/bin/osascript -e 'activate' -e 'POSIX path of (choose file with prompt \"Open sequencer project\")' 2>/dev/null");
  }

  FILE *process = popen(command, "r");
  if (!process) return false;
  bool ok = fgets(out, size, process) != NULL;
  pclose(process);
  out[strcspn(out, "\n")] = '\0';
  return ok && out[0];
}

void
run_file_action (FileAction action) {
  char path[PATH_MAX] = "";
  if (action == FILE_ACTION_OPEN) {
    if (!choose_path(false, path, sizeof path)) return;
    pthread_mutex_lock(&lock);
    if (read_project(path, false)) {
      set_status(TextFormat("OPENED %s", path_basename(path)));
    } else {
      set_status("COULD NOT OPEN THAT FILE");
    }
    pthread_mutex_unlock(&lock);
    return;
  }

  if (action == FILE_ACTION_SAVE && project_path[0]) {
    snprintf(path, sizeof path, "%s", project_path);
  } else {
    if (!choose_path(true, path, sizeof path)) return;
    size_t length = strlen(path);
    if ((length < 4 || strcmp(path + length - 4, ".seq") != 0) && length + 4 < sizeof path) strlcat(path, ".seq", sizeof path);
  }

  pthread_mutex_lock(&lock);
  char previous[PATH_MAX];
  snprintf(previous, sizeof previous, "%s", project_path);
  snprintf(project_path, sizeof project_path, "%s", path);
  if (write_project(path)) {
    set_status(TextFormat("SAVED %s", path_basename(path)));
  } else {
    snprintf(project_path, sizeof project_path, "%s", previous);
    set_status("COULD NOT SAVE THERE");
  }
  pthread_mutex_unlock(&lock);
}

bool
header_button (Rectangle bounds, const char *label) {
  Color text = in_focus() ? INK : PAPER;
  bool hover = !GuiIsLocked() && CheckCollisionPointRec(GetMousePosition(), bounds);
  if (hover) DrawRectangleLinesEx(bounds, 1, text);
  int width = MeasureText(label, 10);
  DrawText(label, bounds.x + (bounds.width - width) / 2, bounds.y + (bounds.height - 10) / 2, 10, text);
  return hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

void
render_header (void) {
  DrawRectangle(0, 0, WINDOW_WIDTH, HEADER_HEIGHT, rule_color());

  if (focused_track >= 0) {
    const Track *track = &tracks[focused_track];
    DrawText(TextFormat("FOCUS  TRACK %02d  CH %d%s", track->id + 1, track->midi_channel, track_audible(track) ? "" : "  (MUTED)"), MARGIN, 13, 10, INK);
  } else {
    const char *name = project_path[0] ? path_basename(project_path) : "UNTITLED";
    DrawText(TextToUpper(name), MARGIN, 13, 10, Fade(PAPER, 0.6f));
  }

  bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
  if (header_button((Rectangle){WINDOW_WIDTH - MARGIN - 160, 8, 44, 20}, "OPEN")) pending_file_action = FILE_ACTION_OPEN;
  if (header_button((Rectangle){WINDOW_WIDTH - MARGIN - 110, 8, 44, 20}, shift ? "SAVE AS" : "SAVE")) {
    pending_file_action = shift ? FILE_ACTION_SAVE_AS : FILE_ACTION_SAVE;
  }

  double phase = (Pt_Time() - loop.loop_start) / loop_length_ms();
  int beat = phase * beats_per_loop;
  for (int k = 0; k < beats_per_loop; k++) {
    int x = WINDOW_WIDTH - MARGIN - 8 - (beats_per_loop - 1 - k) * 14;
    bool lit = transport_playing && k == beat;
    DrawRectangle(x, 14, 8, 8, in_focus() ? (lit ? PAPER : Fade(INK, 0.35f)) : (lit ? ACCENT : INK_LIGHT));
  }
}

void
render_toolbar (void) {
  if (GuiButton((Rectangle){210, TOOLBAR_Y, 80, CELL_HEIGHT}, transport_playing ? "STOP" : "PLAY")) toggle_transport();

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
void
handle_number_click (Rectangle bounds, bool *muted, bool *soloed, int focus_target) {
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

  bool audible, soloed;
  if (focused_track >= 0) {
    handle_number_click(number_bounds, &voice->muted, &voice->soloed, -1);
    audible = voice_audible_in_track(voice);
    soloed = voice->soloed;
  } else {
    handle_number_click(number_bounds, &track->muted, &track->soloed, track->id);
    audible = track_audible(track);
    soloed = track->soloed;
  }

  // Rows that will not sound are drawn inverted; soloed rows have an accent number.
  if (!audible) {
    DrawRectangle(MARGIN - 3, y - 3, WINDOW_WIDTH - 2 * MARGIN + 6, CELL_HEIGHT + 6, INK);
    apply_style(true);
  }
  DrawText(TextFormat("%02d", row + 1), number_bounds.x + 3, y + 4, 20, soloed ? ACCENT : audible ? INK : PAPER);

  DrawText(note_name(voice->midi_note), 52, y + 9, 10, MUTED);
  unsigned int extra = focused_track >= 0 ? 0 : extra_voices_in_use(track->id);
  if (extra) DrawText(TextFormat("+%u", extra), 52, y + 20, 10, ACCENT);

  if (show_note_column()) {
    int note = voice->midi_note;
    GuiSpinner((Rectangle){90, y, 100, CELL_HEIGHT}, NULL, &note, 0, 127, false);
    if (note != (int) voice->midi_note) set_voice_note(voice, note);
  } else {
    int channel = track->midi_channel;
    GuiSpinner((Rectangle){90, y, 100, CELL_HEIGHT}, NULL, &channel, 1, MIDI_CHANNELS, false);
    if (channel != (int) track->midi_channel) set_track_channel(track, channel);
  }

  int steps = voice->steps;
  GuiSpinner((Rectangle){200, y, 100, CELL_HEIGHT}, NULL, &steps, 1, MAX_STEPS, false);
  if (steps != (int) voice->steps) set_voice_steps(voice, steps);

  unsigned int playing_step = (voice->current_step + voice->steps - 1) % voice->steps;
  for (unsigned int j = 0; j < voice->steps; j++) {
    int x0 = GRID_X + (j * GRID_WIDTH) / voice->steps;
    int x1 = GRID_X + ((j + 1) * GRID_WIDTH) / voice->steps;
    Rectangle cell = {x0, y, x1 - x0 - GRID_GAP, CELL_HEIGHT};

    bool on = bit_by_position(voice->sequence, j);
    if (GuiToggle(cell, NULL, &on)) {
      if (!command_down()) voice->sequence ^= (uint64_t) 1 << j; // cmd-click selects without toggling
      selected_step = (voice->id * MAX_STEPS) + j;
    }

    // In focus view active steps are accent, so the playhead and selection use ink instead.
    Color mark = in_focus() ? INK : ACCENT;
    if (transport_playing && j == playing_step) DrawRectangleRec(cell, on ? mark : Fade(mark, in_focus() ? 0.15f : 0.25f));
    if (voice->id * MAX_STEPS + j == (unsigned int) selected_step) DrawRectangleLinesEx(cell, 2, mark);
  }

  if (focused_track < 0) render_voice_lanes(track->id, y);
  if (!audible) apply_style(false);

  if (row < TRACKS_LENGTH - 1) DrawRectangle(MARGIN, y + ROW_HEIGHT - 5, WINDOW_WIDTH - 2 * MARGIN, 1, RULE);
}

bool
mouse_over_grid (void) {
  Vector2 mouse = GetMousePosition();
  return mouse.x >= GRID_X && mouse.y >= GRID_Y && mouse.y < GRID_Y + TRACKS_LENGTH * ROW_HEIGHT;
}

void
render_step (void) {
  Step *step = &steps[selected_step];
  const Voice *voice = &voices[selected_step / MAX_STEPS];

  DrawText("STEP", MARGIN, STEP_LABELS_Y, 10, MUTED);
  DrawText("DETUNE", 90, STEP_LABELS_Y, 10, MUTED);
  DrawText("VELOCITY", 200, STEP_LABELS_Y, 10, MUTED);
  DrawRectangle(MARGIN, STEP_Y - 8, WINDOW_WIDTH - 2 * MARGIN, 2, rule_color());

  DrawText(TextFormat("%02d", selected_step % MAX_STEPS + 1), MARGIN, STEP_Y + 4, 20, INK);
  DrawText(note_name(voice->midi_note + step->detune), 52, STEP_Y + 9, 10, MUTED);

  int detune = step->detune;
  GuiSpinner((Rectangle){90, STEP_Y, 100, CELL_HEIGHT}, NULL, &detune, -36, 36, false);
  if (detune != step->detune) step->detune = detune;

  int velocity = step->velocity;
  GuiSpinner((Rectangle){200, STEP_Y, 100, CELL_HEIGHT}, NULL, &velocity, 1, 127, false);
  if (velocity != (int) step->velocity) step->velocity = velocity;

  const char *hint = focused_track >= 0 ? "CLICK NO: MUTE  CMD+CLICK: SOLO  OPT+CLICK: CLOSE  HOLD CMD: CH"
                                        : "CLICK NO: MUTE  CMD+CLICK: SOLO  OPT+CLICK: FOCUS  HOLD CMD: NOTE";
  if (mouse_over_grid()) hint = "CLICK: TOGGLE STEP  CMD+CLICK: SELECT STEP TO EDIT BELOW";
  if (GetMousePosition().y < HEADER_HEIGHT) hint = "CMD+S: SAVE  SHIFT+CMD+S: SAVE AS  CMD+O: OPEN";
  Vector2 mouse = GetMousePosition();
  if (mouse.y >= PATTERN_Y && mouse.y < PATTERN_Y + CELL_HEIGHT) {
    if (mouse.x < CHAIN_BUTTON_X) hint = "CLICK: SWITCH PATTERN  SHIFT+CLICK: ADD TO CHAIN  OPT+CLICK: COPY HERE";
    else if (mouse.x < GRID_X) hint = "CLICK: PLAY THE CHAIN INSTEAD OF ONE PATTERN";
    else hint = "CLICK: +1 BAR  SHIFT+CLICK: -1 BAR  OPT+CLICK: REMOVE";
  }
  DrawText(hint, WINDOW_WIDTH - MARGIN - MeasureText(hint, 10), STEP_Y + 9, 10, MUTED);
}

Rectangle
pattern_button_bounds (unsigned int index) {
  return (Rectangle){MARGIN + index * PATTERN_BUTTON_STEP, PATTERN_Y, PATTERN_BUTTON_WIDTH, CELL_HEIGHT};
}

bool
clicked (Rectangle bounds) {
  return !GuiIsLocked() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(GetMousePosition(), bounds);
}

bool
shift_down (void) {
  return IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
}

// Bars are CHAIN_BAR_WIDTH wide until the whole chain would overflow the grid width.
float
chain_bar_width (void) {
  unsigned int bars = 0;
  for (unsigned int i = 0; i < chain_length; i++) bars += chain[i].repeats;
  float fit = (float) GRID_WIDTH / bars;
  return fit < CHAIN_BAR_WIDTH ? fit : CHAIN_BAR_WIDTH;
}

void
draw_centered_text (const char *text, Rectangle bounds, Color color) {
  int width = MeasureText(text, 10);
  DrawText(text, bounds.x + (bounds.width - width) / 2, bounds.y + (bounds.height - 10) / 2, 10, color);
}

// Pattern buttons: click switches (queued until the bar ends while playing),
// shift-click appends to the chain, opt-click copies the current pattern there.
void
render_pattern_buttons (void) {
  bool blink = fmod(GetTime(), 0.5) < 0.25;
  for (unsigned int i = 0; i < PATTERNS_LENGTH; i++) {
    Rectangle bounds = pattern_button_bounds(i);
    if (clicked(bounds)) {
      if (shift_down()) {
        append_to_chain(i);
      } else if (option_down()) {
        copy_current_pattern_to(i);
      } else {
        select_pattern(i);
      }
    }

    bool current = (int) i == current_pattern;
    bool queued = (int) i == queued_pattern;
    bool hover = !GuiIsLocked() && CheckCollisionPointRec(GetMousePosition(), bounds);
    Color fill = current ? INK : queued && blink ? ACCENT : hover ? PAPER_DARK : background_color();
    Color text = current ? PAPER : pattern_has_steps(i) || queued ? INK : MUTED;
    DrawRectangleRec(bounds, fill);
    DrawRectangleLinesEx(bounds, 1, INK);
    draw_centered_text(TextFormat("%c", 'A' + i), bounds, text);
  }

  Rectangle chain_button = {CHAIN_BUTTON_X, PATTERN_Y, GRID_X - CHAIN_BUTTON_X - 30, CELL_HEIGHT};
  if (clicked(chain_button)) toggle_chain_mode();
  DrawRectangleRec(chain_button, chain_mode ? ACCENT : background_color());
  DrawRectangleLinesEx(chain_button, 1, chain_length ? INK : MUTED);
  draw_centered_text("CHAIN", chain_button, chain_length ? INK : MUTED);
}

// The chain as a timeline: each entry is as wide as the bars it plays, with a tick per bar.
// Click adds a bar, shift-click removes one, opt-click removes the entry.
void
render_chain (void) {
  if (!chain_length) {
    DrawText("SHIFT+CLICK A PATTERN TO ADD IT TO THE CHAIN", GRID_X, PATTERN_Y + 9, 10, MUTED);
    return;
  }

  float bar_width = chain_bar_width();
  float x = GRID_X;
  double phase = (Pt_Time() - loop.loop_start) / loop_length_ms();
  if (phase > 1) phase = 1;

  for (unsigned int i = 0; i < chain_length; i++) {
    ChainEntry *entry = &chain[i];
    Rectangle bounds = {x, PATTERN_Y, entry->repeats * bar_width - GRID_GAP, CELL_HEIGHT};
    x += entry->repeats * bar_width;

    if (clicked(bounds)) {
      if (option_down()) {
        remove_from_chain(i);
        return; // entries shifted, draw them next frame
      } else if (shift_down()) {
        if (entry->repeats > 1) entry->repeats--;
      } else if (entry->repeats < CHAIN_MAX_REPEATS) {
        entry->repeats++;
      }
      bounds.width = entry->repeats * bar_width - GRID_GAP;
    }

    bool active = chain_mode && i == chain_position;
    DrawRectangleRec(bounds, active ? Fade(ACCENT, 0.3f) : background_color());
    if (active && transport_playing && !chain_restart) {
      float progress = (chain_bar + phase) * bar_width;
      DrawRectangle(bounds.x, bounds.y, progress < bounds.width ? progress : bounds.width, bounds.height, ACCENT);
    }
    for (unsigned int bar = 1; bar < entry->repeats; bar++) {
      DrawRectangle(bounds.x + bar * bar_width - 1, bounds.y + CELL_HEIGHT - 6, 1, 6, MUTED);
    }
    DrawRectangleLinesEx(bounds, 1, INK);

    const char *label = TextFormat("%c x%u", 'A' + entry->pattern, entry->repeats);
    if (MeasureText(label, 10) + 6 > bounds.width) label = TextFormat("%c", 'A' + entry->pattern);
    if (MeasureText(label, 10) + 4 <= bounds.width) draw_centered_text(label, bounds, INK);
  }
}

void
render_patterns (void) {
  render_pattern_buttons();
  render_chain();
}

void
render_tracks (void) {
  DrawText(focused_track >= 0 ? "VOICE" : "NO", MARGIN, LABELS_Y, 10, MUTED);
  DrawText(show_note_column() ? "NOTE" : "CH", 90, LABELS_Y, 10, MUTED);
  DrawText("STEPS", 200, LABELS_Y, 10, MUTED);
  for (int k = 0; k < beats_per_loop; k++) {
    DrawText(TextFormat("%d", k + 1), GRID_X + k * GRID_WIDTH / beats_per_loop, LABELS_Y, 10, MUTED);
  }
  DrawRectangle(MARGIN, GRID_Y - 8, WINDOW_WIDTH - 2 * MARGIN, 2, rule_color());

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
  const char *info = TextFormat("PATTERN %c  %d/4  %.0f MS / LOOP", 'A' + current_pattern, beats_per_loop, loop_length_ms());

  DrawRectangle(MARGIN, FOOTER_Y, WINDOW_WIDTH - 2 * MARGIN, 1, rule_color());
  DrawRectangle(MARGIN, FOOTER_Y + 11, 6, 6, midi_output_stream ? ACCENT : MUTED);
  DrawText(TextFormat("OUT  %s", TextToUpper(output)), MARGIN + 12, FOOTER_Y + 10, 10, INK);
  DrawText(info, WINDOW_WIDTH - MARGIN - MeasureText(info, 10), FOOTER_Y + 10, 10, MUTED);
  if (GetTime() < status_until) DrawText(status_message, 300, FOOTER_Y + 10, 10, ACCENT);
}

int
main (void) {
  InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "SEQ/08");
  SetTargetFPS(60);
  Pm_Initialize();

  init_tracks();
  init_steps();
  init_patterns();

  list_midi_devices();
  char session[PATH_MAX];
  autosave_path(session, sizeof session);
  if (!read_project(session, true)) randomize_voices(0, TRACKS_LENGTH, VOICES_PER_TRACK);
  double next_autosave = GetTime() + AUTOSAVE_SECONDS;

  if (Pt_Start(CLOCK_RESOLUTION_MS, tick, NULL) != ptNoError) {
    CloseWindow();
    return 1;
  }

  while (!WindowShouldClose()) {
    BeginDrawing();
    pthread_mutex_lock(&lock);

    ClearBackground(background_color());
    apply_style(false);

    if (!dropdown_edit_mode) {
      if (IsKeyPressed(KEY_SPACE)) toggle_transport();
      if (IsKeyPressed(KEY_F) && !command_down()) toggle_focus();
      if (command_down() && IsKeyPressed(KEY_S)) {
        pending_file_action = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT) ? FILE_ACTION_SAVE_AS : FILE_ACTION_SAVE;
      }
      if (command_down() && IsKeyPressed(KEY_O)) pending_file_action = FILE_ACTION_OPEN;
    }
    if (GetTime() >= next_autosave) {
      autosave();
      next_autosave = GetTime() + AUTOSAVE_SECONDS;
    }

    render_header();
    render_footer();
    if (dropdown_edit_mode) GuiLock();
    render_toolbar();
    render_patterns();
    render_tracks();
    render_step();
    GuiUnlock();
    render_dropdown();

    pthread_mutex_unlock(&lock);

    EndDrawing();

    if (pending_file_action != FILE_ACTION_NONE) {
      FileAction action = pending_file_action;
      pending_file_action = FILE_ACTION_NONE;
      run_file_action(action);
    }
  }

  pthread_mutex_lock(&lock);
  autosave();
  pthread_mutex_unlock(&lock);
  stop_sequencer();
  Pt_Stop();
  close_midi_output_stream();
  Pm_Terminate();
  CloseWindow();
  return 0;
}
