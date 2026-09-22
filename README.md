# Classical DAW Alpha

This repository is the first compileable slice of a macOS-oriented classical
music workstation. The engine is self-contained C++17 with no third-party
source copied into the tree. It is intentionally an offline core, suitable for
validating score timing and audio rendering before platform integration.

## What works in Alpha

- A fixed 960 ticks-per-quarter-note score timeline and piecewise constant BPM
  tempo map, with tick/second/sample conversion.
- MIDI note, track, and file data structures.
- Standard MIDI File type 0/1 read/write for notes, track names, and tempo
  events (unknown events are skipped on read).
- A deliberately limited MusicXML `score-partwise` slice: one part with
  multiple voices/staves, 960-PPQ notes and rests, chords, ties, tuplets,
  meter, tempo, and deterministic import/export.
- A platform-neutral realtime transport skeleton with a bounded SPSC command
  ring, block scheduler, and xrun counter.
- On macOS, an optional `daw_coreaudio` target owns the default-output
  device lifecycle and connects that scheduler to a silence-safe callback with
  a fixed polyphonic sine diagnostic voice.
- A versioned, line-oriented project file format with strict validation and
  atomic replacement, preserving the current score model for save/reload.
- Deterministic offline mono sine rendering and 16-bit PCM WAV export.
- CTest coverage for tempo conversion, MIDI/MusicXML round-trips, project
  persistence, realtime primitives, and render smoke tests.

The renderer is a diagnostic instrument, not an orchestral sampler. The
MusicXML reader is a fail-closed first slice; it does not yet cover `.mxl`,
multiple parts, key changes, dynamics, or full XML validation.
There is no production instrument library, score engraving, automation, mixer,
plugin hosting, undo stack, autosave/recovery workflow, or multi-part project
package yet.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

## Planned macOS slices

1. Add CoreMIDI input/output and timestamped event scheduling.
2. Add autosave/recovery and persisted undo history around the versioned project
   file format.
3. Add bar/beat and time-signature maps plus a piano roll/score UI (SwiftUI or
   Qt front end over the C++ engine).
4. Replace the diagnostic oscillator with instrument voices, mixer buses,
   metering, bounce jobs, and production project packaging.

The project is released under **AGPL-3.0-or-later**; see `LICENSE`. The Alpha
contains only original project code and the C++ standard library. Any future
audio backend, plugin SDK, instrument, or UI dependency must be recorded in a
third-party notices file and checked separately before distribution.

The current study notes and acceptance gates are in
[`docs/research/2026-09-22-daw-architecture.md`](docs/research/2026-09-22-daw-architecture.md)
and [`docs/roadmap.md`](docs/roadmap.md).

The no-build web prototype lives in [`web/`](web/) with its own deployment
instructions. Run `python3 -m http.server 8080 --directory web` from the
repository root for a local preview.
