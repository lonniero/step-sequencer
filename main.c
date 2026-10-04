#define RAYGUI_IMPLEMENTATION

#include <stdio.h>
#include <string.h>
#include "raylib.h"
#include <raygui.h>
#include "sequencer.h"

#define SCREEN_W 1100
#define SCREEN_H 680
#define MARGIN 16
#define BAND_H 64              // white masthead
#define TOOLBAR_Y 80
#define TOOLBAR_H 30
#define HEADER_Y 128
#define ROWS_Y 152
#define ROW_H 56
#define CELL_H (ROW_H - 22)    // leaves room for the data line under each row
#define CELL_GAP 2
#define SPINE_W 28             // rotated text down the right edge
#define GRID_X (MARGIN + 446)
#define GRID_W (SCREEN_W - GRID_X - MARGIN - SPINE_W)
#define STATUS_H 26

// Left column x offsets
#define COL_TRK   (MARGIN)
#define COL_STEPS (MARGIN + 50)
#define COL_NOTE  (MARGIN + 126)
#define COL_NAME  (MARGIN + 200)
#define COL_CH    (MARGIN + 236)
#define COL_VEL   (MARGIN + 302)
#define COL_MUTE  (MARGIN + 374)
#define COL_RND   (MARGIN + 402)

// Monochrome palette
#define C_BG    (Color){11, 11, 11, 255}
#define C_INK   (Color){230, 230, 230, 255}
#define C_MID   (Color){150, 150, 150, 255}
#define C_DIM   (Color){80, 80, 80, 255}
#define C_FAINT (Color){38, 38, 38, 255}

static const int default_steps[TRACK_COUNT] = {32, 6, 8, 8, 8, 3, 5, 2};
// C minor pentatonic spread over a few octaves: fast tracks high, slow ones low
static const int default_notes[TRACK_COUNT] = {75, 72, 67, 63, 60, 55, 48, 36};

// Black, white and greys only. Active states invert instead of using colour.
static void
apply_mono_style (void) {
  GuiSetStyle(DEFAULT, BORDER_COLOR_NORMAL, 0x3c3c3cff);
  GuiSetStyle(DEFAULT, BASE_COLOR_NORMAL, 0x0b0b0bff);
  GuiSetStyle(DEFAULT, TEXT_COLOR_NORMAL, 0x969696ff);
  GuiSetStyle(DEFAULT, BORDER_COLOR_FOCUSED, 0xe6e6e6ff);
  GuiSetStyle(DEFAULT, BASE_COLOR_FOCUSED, 0x0b0b0bff);
  GuiSetStyle(DEFAULT, TEXT_COLOR_FOCUSED, 0xe6e6e6ff);
  GuiSetStyle(DEFAULT, BORDER_COLOR_PRESSED, 0xe6e6e6ff);
  GuiSetStyle(DEFAULT, BASE_COLOR_PRESSED, 0xe6e6e6ff);
  GuiSetStyle(DEFAULT, TEXT_COLOR_PRESSED, 0x0b0b0bff);
  GuiSetStyle(DEFAULT, BORDER_COLOR_DISABLED, 0x1e1e1eff);
  GuiSetStyle(DEFAULT, BASE_COLOR_DISABLED, 0x0b0b0bff);
  GuiSetStyle(DEFAULT, TEXT_COLOR_DISABLED, 0x333333ff);
  GuiSetStyle(DEFAULT, LINE_COLOR, 0x3c3c3cff);
  GuiSetStyle(DEFAULT, BACKGROUND_COLOR, 0x0b0b0bff);
  GuiSetStyle(SLIDER, SLIDER_WIDTH, 4);
  GuiSetStyle(DEFAULT, TEXT_SIZE, 10);
  GuiSetStyle(VALUEBOX, SPINNER_BUTTON_WIDTH, 16);
  GuiSetStyle(VALUEBOX, SPINNER_BUTTON_SPACING, 2);
}

static const char *
note_name (int note) {
  static const char *names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  return TextFormat("%s%d", names[note % 12], note / 12 - 1); // MIDI 60 = C4
}

// ---- Type and graphic devices ---------------------------------------------
// The default raylib font is a pixel font; scaling it by whole multiples keeps
// it crisp and gives the chunky masthead type.

static void
draw_text (const char *text, float x, float y, int size, Color color) {
  DrawTextEx(GuiGetFont(), text, (Vector2){x, y}, size, size / 10.0f, color);
}

static void
draw_small_text (const char *text, float x, float y, Color color) {
  draw_text(text, x, y, 10, color);
}

// Text running up the page, read bottom to top.
static void
draw_text_vertical (const char *text, float x, float y, int size, Color color) {
  DrawTextPro(GuiGetFont(), text, (Vector2){x, y}, (Vector2){0, 0}, -90, size, size / 10.0f, color);
}

static void
draw_hatch (Rectangle r, float spacing, Color color) {
  BeginScissorMode(r.x, r.y, r.width, r.height);
  for (float d = -r.height; d < r.width; d += spacing) {
    DrawLineEx((Vector2){r.x + d, r.y + r.height}, (Vector2){r.x + d + r.height, r.y}, 2, color);
  }
  EndScissorMode();
}

// Print-style registration mark: crosshair in a circle.
static void
draw_reg_mark (float x, float y, Color color) {
  DrawCircleLines(x, y, 5, color);
  DrawLineEx((Vector2){x - 9, y}, (Vector2){x + 9, y}, 1, color);
  DrawLineEx((Vector2){x, y - 9}, (Vector2){x, y + 9}, 1, color);
}

// A barcode spelled out by the actual step patterns: thick bar = step on,
// hairline = step off, gap between tracks. It changes as you edit.
static void
draw_pattern_barcode (const SeqConfig *cfg, float x, float y, float h, Color color) {
  for (int i = 0; i < TRACK_COUNT; i++) {
    const TrackConfig *t = &cfg->tracks[i];
    for (int s = 0; s < t->steps; s++) {
      bool on = pattern_get(t->pattern, s);
      DrawRectangle(x, y, on ? 3 : 1, h, color);
      x += on ? 5 : 3;
    }
    x += 6;
  }
}

// Beat ruler above the grid: a tick per beat, labelled with the bar every 4.
static void
draw_ruler (Rectangle grid, int beats) {
  float y = grid.y - 6;
  DrawLineEx((Vector2){grid.x, y}, (Vector2){grid.x + grid.width, y}, 1, C_DIM);
  for (int i = 0; i <= beats; i++) {
    float x = grid.x + grid.width * i / beats;
    bool major = i % 4 == 0;
    DrawLineEx((Vector2){x, y - (major ? 6 : 3)}, (Vector2){x, y}, 1, major ? C_MID : C_DIM);
    if (major && i < beats) draw_small_text(TextFormat("%02d", i / 4 + 1), x + 3, y - 17, C_DIM);
  }
}

static void
draw_masthead (const SeqConfig *cfg) {
  DrawRectangle(0, 0, SCREEN_W, BAND_H, C_INK);

  // Big index, then the stacked descriptor.
  draw_text("RGM-08", MARGIN, 12, 40, C_BG);
  float x = MARGIN + 190;
  DrawRectangle(x, 12, 2, 40, C_BG);
  draw_small_text("POLYRHYTHMIC", x + 10, 12, C_BG);
  draw_small_text("SEQUENCE UNIT", x + 10, 24, C_BG);
  draw_small_text(">> MIDI OUT", x + 10, 36, C_BG);
  draw_small_text(TextFormat("%02d TRK / %03d BPM", TRACK_COUNT, cfg->bpm), x + 10, 48, C_BG);

  // Run state as a solid inverted tab.
  x += 130;
  DrawRectangle(x, 12, 64, 40, C_BG);
  draw_text(cfg->playing ? "RUN" : "HLT", x + 9, 22, 20, C_INK);

  x += 84;
  draw_small_text("PATTERN", x, 12, C_BG);
  draw_small_text("PHASE", x, 24, C_BG);
  draw_small_text("REPEAT", x, 36, C_BG);
  draw_small_text("+++", x, 48, C_BG);

  // Barcode with a checksum under it, then a hatch block.
  float code_x = x + 70;
  draw_pattern_barcode(cfg, code_x, 12, 30, C_BG);
  uint64_t sum = 0;
  for (int i = 0; i < TRACK_COUNT; i++) sum ^= cfg->tracks[i].pattern * (i + 1);
  draw_small_text(TextFormat("%016llX", (unsigned long long)sum), code_x, 48, C_BG);

  float hatch_w = 70;
  draw_hatch((Rectangle){SCREEN_W - MARGIN - hatch_w, 12, hatch_w, 40}, 6, C_BG);
}

// ---- Model helpers ----------------------------------------------------------

static void
randomize_track (TrackConfig *t) {
  t->pattern = pattern_random(t->steps);
}

static SeqConfig
default_config (void) {
  SeqConfig c = { .bpm = 96, .beats = 16, .gate = 0.5f, .playing = false };
  for (int i = 0; i < TRACK_COUNT; i++) {
    c.tracks[i] = (TrackConfig){
      .steps = default_steps[i],
      .note = default_notes[i],
      .channel = 1,
      .velocity = 100,
      .muted = false,
    };
    randomize_track(&c.tracks[i]);
  }
  return c;
}

// Build the "a;b;c" string raygui's dropdown wants. ';' inside a device
// name would split it, so swap those out.
static void
build_device_list (MidiDevice devices[], int count, char *out, size_t len) {
  out[0] = '\0';
  if (count == 0) {
    snprintf(out, len, "NO OUTPUTS");
    return;
  }
  for (int i = 0; i < count; i++) {
    for (char *c = devices[i].name; *c; c++) if (*c == ';') *c = ',';
    if (i > 0) strncat(out, ";", len - strlen(out) - 1);
    strncat(out, TextToUpper(devices[i].name), len - strlen(out) - 1);
  }
}

// Draws one track's steps across the grid; returns true if the pattern was edited.
static bool
draw_step_row (Rectangle area, TrackConfig *t, int current_step, bool sounding,
               bool playing, bool input_enabled) {
  bool changed = false;
  float cell_w = area.width / t->steps;
  Vector2 mouse = GetMousePosition();

  for (int s = 0; s < t->steps; s++) {
    Rectangle cell = { area.x + s * cell_w, area.y, cell_w - CELL_GAP, area.height };
    bool on = pattern_get(t->pattern, s);
    bool is_current = playing && s == current_step;
    bool hover = input_enabled && CheckCollisionPointRec(mouse, cell);

    if (on) {
      Color fill = t->muted ? C_FAINT : C_MID;
      if (is_current && sounding && !t->muted) fill = C_INK;
      DrawRectangleRec(cell, fill);
    } else {
      DrawRectangleLinesEx(cell, 1, C_FAINT);
    }
    if (is_current) DrawRectangle(cell.x, cell.y + cell.height + 2, cell.width, 2, C_INK);
    if (hover) DrawRectangleLinesEx(cell, 1, C_INK);

    if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
      t->pattern = pattern_toggle(t->pattern, s);
      changed = true;
    }
  }
  return changed;
}

int
main (void) {
  SetConfigFlags(FLAG_MSAA_4X_HINT);
  InitWindow(SCREEN_W, SCREEN_H, "RGM-08");
  SetTargetFPS(60);
  apply_mono_style();

  SeqConfig initial = default_config();
  bool midi_ok = sequencer_init(&initial);

  MidiDevice devices[MAX_DEVICES];
  int device_count = sequencer_list_devices(devices, MAX_DEVICES);
  static char device_list[MAX_DEVICES * (DEVICE_NAME_LEN + 1)];
  build_device_list(devices, device_count, device_list, sizeof device_list);

  int device_active = 0;
  for (int i = 0; i < device_count; i++) {
    if (devices[i].id == sequencer_current_device()) device_active = i;
  }
  bool device_edit = false;

  // Spinners need to know which one (if any) is in text edit mode.
  bool steps_edit[TRACK_COUNT] = {0}, note_edit[TRACK_COUNT] = {0};
  bool chan_edit[TRACK_COUNT] = {0}, vel_edit[TRACK_COUNT] = {0};
  bool bpm_edit = false, beats_edit = false;

  SeqConfig cfg;
  SeqStatus st;
  char error[128];

  while (!WindowShouldClose()) {
    sequencer_get(&cfg, &st);
    bool changed = false;

    bool typing = device_edit || bpm_edit || beats_edit;
    for (int i = 0; i < TRACK_COUNT; i++) {
      typing |= steps_edit[i] || note_edit[i] || chan_edit[i] || vel_edit[i];
    }

    // Keyboard shortcuts (not while typing in a value box)
    if (!typing) {
      if (IsKeyPressed(KEY_SPACE)) { cfg.playing = !cfg.playing; changed = true; }
      if (IsKeyPressed(KEY_R)) {
        for (int i = 0; i < TRACK_COUNT; i++) randomize_track(&cfg.tracks[i]);
        changed = true;
      }
      if (IsKeyPressed(KEY_HOME) || IsKeyPressed(KEY_ENTER)) sequencer_rewind();
      for (int i = 0; i < TRACK_COUNT; i++) {
        if (IsKeyPressed(KEY_ONE + i)) { cfg.tracks[i].muted = !cfg.tracks[i].muted; changed = true; }
      }
    }

    BeginDrawing();
    ClearBackground(C_BG);

    draw_masthead(&cfg);

    // The dropdown overlaps everything below it while open.
    if (device_edit) GuiLock();

    // ---- Toolbar ---------------------------------------------------------
    float x = MARGIN, y = TOOLBAR_Y;
    if (GuiToggle((Rectangle){x, y, 90, TOOLBAR_H},
                  cfg.playing ? "STOP" : "PLAY", &cfg.playing)) changed = true;
    x += 96;
    if (GuiButton((Rectangle){x, y, 30, TOOLBAR_H}, "|<")) sequencer_rewind();
    x += 36;

    int bpm = cfg.bpm, beats = cfg.beats;
    draw_small_text("BPM", x, y + 10, C_MID);
    if (GuiSpinner((Rectangle){x + 28, y + 3, 80, 24}, NULL, &bpm, 20, 300, bpm_edit)) bpm_edit = !bpm_edit;
    x += 28 + 80 + 12;
    draw_small_text("BEATS", x, y + 10, C_MID);
    if (GuiSpinner((Rectangle){x + 40, y + 3, 70, 24}, NULL, &beats, 1, 64, beats_edit)) beats_edit = !beats_edit;
    if (bpm != cfg.bpm || beats != cfg.beats) { cfg.bpm = bpm; cfg.beats = beats; changed = true; }
    x += 40 + 70 + 40;

    float gate = cfg.gate;
    GuiSlider((Rectangle){x, y + 5, 100, 20}, "GATE", TextFormat("%.2f", gate), &gate, 0.05f, 1.0f);
    if (gate != cfg.gate) { cfg.gate = gate; changed = true; }
    x += 100 + 44;

    if (GuiButton((Rectangle){x, y, 100, TOOLBAR_H}, "RND ALL")) {
      for (int i = 0; i < TRACK_COUNT; i++) randomize_track(&cfg.tracks[i]);
      changed = true;
    }
    x += 106;
    if (GuiButton((Rectangle){x, y, 70, TOOLBAR_H}, "ALL OFF")) sequencer_panic();

    // ---- Column headers --------------------------------------------------
    draw_small_text("TRK", COL_TRK, HEADER_Y + 4, C_DIM);
    draw_small_text("STEPS", COL_STEPS, HEADER_Y + 4, C_DIM);
    draw_small_text("NOTE", COL_NOTE, HEADER_Y + 4, C_DIM);
    draw_small_text("CH", COL_CH, HEADER_Y + 4, C_DIM);
    draw_small_text("VEL", COL_VEL, HEADER_Y + 4, C_DIM);
    draw_small_text("M", COL_MUTE + 8, HEADER_Y + 4, C_DIM);
    draw_small_text("R", COL_RND + 8, HEADER_Y + 4, C_DIM);
    DrawLineEx((Vector2){MARGIN, ROWS_Y - 6}, (Vector2){GRID_X - 16, ROWS_Y - 6}, 1, C_DIM);

    // ---- Tracks ----------------------------------------------------------
    Rectangle grid = {GRID_X, ROWS_Y, GRID_W, TRACK_COUNT * ROW_H - 8};
    for (int i = 0; i < TRACK_COUNT; i++) {
      TrackConfig *t = &cfg.tracks[i];
      float ry = ROWS_Y + i * ROW_H;
      float cy = ry + CELL_H / 2.0f - 12; // centered on the step cells
      TrackConfig before = *t;

      // Index block: solid when live, outline when muted.
      Rectangle idx = {COL_TRK, ry, 40, CELL_H};
      if (t->muted) DrawRectangleLinesEx(idx, 1, C_DIM);
      else DrawRectangleRec(idx, C_INK);
      draw_text(TextFormat("%02d", i + 1), idx.x + 6, idx.y + 7, 20, t->muted ? C_DIM : C_BG);

      if (GuiSpinner((Rectangle){COL_STEPS, cy, 70, 24}, NULL, &t->steps, 1, MAX_STEPS, steps_edit[i]))
        steps_edit[i] = !steps_edit[i];
      if (GuiSpinner((Rectangle){COL_NOTE, cy, 70, 24}, NULL, &t->note, 0, 127, note_edit[i]))
        note_edit[i] = !note_edit[i];
      GuiLabel((Rectangle){COL_NAME, cy, 32, 24}, note_name(t->note));
      if (GuiSpinner((Rectangle){COL_CH, cy, 60, 24}, NULL, &t->channel, 1, 16, chan_edit[i]))
        chan_edit[i] = !chan_edit[i];
      if (GuiSpinner((Rectangle){COL_VEL, cy, 64, 24}, NULL, &t->velocity, 1, 127, vel_edit[i]))
        vel_edit[i] = !vel_edit[i];

      GuiToggle((Rectangle){COL_MUTE, cy, 24, 24}, "M", &t->muted);
      if (GuiButton((Rectangle){COL_RND, cy, 24, 24}, "R")) randomize_track(t);

      // Shrinking the step count drops bits that are now out of range.
      if (t->steps != before.steps && t->steps < 64) t->pattern &= (1ULL << t->steps) - 1;

      // Tag line under the controls.
      draw_small_text(TextFormat("/// T%02d.CH%02d.N%03d", i + 1, t->channel, t->note),
                      COL_STEPS, ry + CELL_H + 6, C_DIM);

      Rectangle row = {GRID_X, ry, GRID_W, CELL_H};
      if (draw_step_row(row, t, st.current_step[i], st.sounding[i],
                        cfg.playing, !GuiIsLocked())) changed = true;

      // Data line: step position, raw pattern, step period
      draw_small_text(TextFormat("%02d/%02d  0x%016llX  %8.1f MS", cfg.playing ? st.current_step[i] + 1 : 0,
                                 t->steps, (unsigned long long)t->pattern, seq_loop_ms(&cfg) / t->steps),
                      GRID_X, ry + CELL_H + 6, C_DIM);

      if (memcmp(&before, t, sizeof before) != 0) changed = true;
    }

    // Playhead: all rows share the same loop, so one line crosses them all.
    draw_ruler(grid, cfg.beats);
    float px = grid.x + (float)st.phase * grid.width;
    DrawLineEx((Vector2){px, grid.y - 12}, (Vector2){px, grid.y + grid.height}, 1,
               cfg.playing ? C_INK : C_DIM);
    // Position as bar.beat, 1-based
    int beat = (int)(st.phase * cfg.beats);
    draw_small_text(TextFormat("T+%02d.%d", beat / 4 + 1, beat % 4 + 1),
                    GRID_X + GRID_W - 44, HEADER_Y - 10, C_MID);

    // Registration marks framing the grid.
    draw_reg_mark(GRID_X - 8, ROWS_Y - 26, C_DIM);
    draw_reg_mark(GRID_X + GRID_W + 8, ROWS_Y - 26, C_DIM);
    draw_reg_mark(GRID_X - 8, grid.y + grid.height + 20, C_DIM);
    draw_reg_mark(GRID_X + GRID_W + 8, grid.y + grid.height + 20, C_DIM);

    // Spine: rotated slogan up the right edge.
    draw_text_vertical("PHASE IS THE ONLY CONSTANT  ///  08 VOICES  ///  NO CLOCK BUT THIS ONE",
                       SCREEN_W - MARGIN - 14, grid.y + grid.height, 10, C_DIM);

    GuiUnlock();

    // ---- Status strip (inverted) ------------------------------------------
    sequencer_last_error(error, sizeof error);
    const char *status_text;
    if (!midi_ok) status_text = TextFormat("ERR >> MIDI INIT: %s", error);
    else if (error[0]) status_text = TextFormat("ERR >> %s", error);
    else if (sequencer_current_device() < 0) status_text = "ERR >> NO OUTPUT OPEN";
    else status_text = TextFormat("OUT >> %s", devices[device_active].name);
    float sy = SCREEN_H - STATUS_H;
    DrawRectangle(0, sy, SCREEN_W, STATUS_H, C_INK);
    draw_small_text(TextToUpper(status_text), MARGIN, sy + 8, C_BG);
    draw_small_text("SPC PLAY/STOP  ///  RET REWIND  ///  R RND  ///  1-8 MUTE",
                    SCREEN_W - MARGIN - 380, sy + 8, C_BG);

    // ---- Device picker (drawn last so it opens on top) -------------------
    Rectangle dev_rect = {SCREEN_W - MARGIN - 200, TOOLBAR_Y, 200, TOOLBAR_H};
    GuiLabel((Rectangle){dev_rect.x - 34, dev_rect.y, 32, dev_rect.height}, "OUT");
    if (device_count == 0) GuiDisable();
    int previous = device_active;
    if (GuiDropdownBox(dev_rect, device_list, &device_active, device_edit)) {
      device_edit = !device_edit;
      if (!device_edit && device_active != previous) sequencer_open_device(devices[device_active].id);
    }
    GuiEnable();

    EndDrawing();

    if (changed) sequencer_set(&cfg);
  }

  sequencer_shutdown();
  CloseWindow();
  return 0;
}
