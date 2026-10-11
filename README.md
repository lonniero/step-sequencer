# step-sequencer

<img width="1596" height="934" alt="image" src="https://github.com/user-attachments/assets/5907d515-de7e-496f-a1eb-25e019975253" />

---

An 8-track MIDI step sequencer for macOS, written in C with [raylib](https://www.raylib.com/), [raygui](https://github.com/raysan5/raygui) and [PortMidi](https://github.com/PortMidi/portmidi).

- 8 tracks, each with its own MIDI note and step count (1–32), so tracks of different lengths play polyrhythms
- BPM control (40–240), one bar of 4 beats per loop
- Click cells to toggle steps; Randomize and Clear the whole pattern
- Choose any MIDI output device from the dropdown

## Build

Requires macOS and [Homebrew](https://brew.sh).

```sh
brew install raylib portmidi
make run
```

`make release` builds a self-contained binary instead: it compiles raylib and PortMidi from source as static libraries and targets macOS 14+, so it runs without Homebrew. The first run needs `cmake` and takes a couple of minutes.

The binary is written to `build/step-sequencer`.

`make app` packages the release binary as `build/StepSequencer.app`, with the icon generated from `icon.png`.

## Sending MIDI to another app

To play the sequencer through a DAW or soft synth on the same Mac, use the built-in IAC Driver:

1. Open **Audio MIDI Setup** → **Window** → **Show MIDI Studio**
2. Double-click **IAC Driver** and check **Device is online**
3. Pick **IAC Driver Bus 1** in the sequencer and as the MIDI input in your DAW

