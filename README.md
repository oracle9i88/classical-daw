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
- A deliberately limited MusicXML `score-partwise` slice: multiple ordered
  parts with multiple voices/staves, 960-PPQ notes and rests, chords, ties, tuplets,
  meter, tempo, and one escaped lyric text per note with deterministic
  import/export.
- A deterministic Score-to-SMF bridge for exporting ordered score parts as a
  Type 1 MIDI track per part, retaining note timing, pitch, velocity, voices as
  events, and BPM. Parts receive stable channels 0–15; export rejects more than
  16 parts instead of colliding channels. Tuplet spelling is represented by its
  resolved tick durations.
- A matching MIDI-to-Score bridge for importing non-empty 960-PPQ tracks as
  ordered score parts, retaining track names, note timing, pitch, velocity, and
  MIDI channels as score voices. It supplies a default 4/4 meter and uses the
  first valid MIDI tempo, so an imported file can continue through MusicXML or
  project persistence.
- A platform-neutral realtime transport skeleton with a bounded SPSC command
  ring, block scheduler, and xrun counter.
- On macOS, an optional `daw_coreaudio` target owns the default-output
  device lifecycle and connects that scheduler to a silence-safe callback with
  a fixed polyphonic sine diagnostic voice.
- A versioned, line-oriented project file format with strict validation and
  atomic replacement, preserving the current score model for save/reload.
- A bounded UI/control-thread `ScoreHistory` with undo/redo, redo-branch
  invalidation, and failure-preserving operations. It is an in-memory edit
  history; it is not used by the realtime callback and is not persisted yet.
- Deterministic offline mono sine rendering and 16-bit PCM WAV export.
- CTest coverage for tempo conversion, MIDI/MusicXML round-trips, Score-to-MIDI
  export, project persistence, realtime primitives, and render smoke tests.

The renderer is a diagnostic instrument, not an orchestral sampler. The
MusicXML reader is a fail-closed first slice; it does not yet cover `.mxl`,
key changes, dynamics, multiple lyric verses/syllabic metadata, or full XML
validation.
The MIDI bridge currently rejects scores with more than 16 parts because the
existing MIDI model does not yet carry a channel-allocation map. It also does
not yet carry meter, lyrics, or instrument programs. MIDI import uses canonical
sharp pitch spellings and a default 4/4 meter; key-aware enharmonic spelling,
MIDI meter maps, and program metadata remain future fields.
There is no production instrument library, score engraving, automation, mixer,
plugin hosting, autosave/recovery workflow, persisted undo history, or multi-part
project package yet.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

## Planned macOS slices

1. Add CoreMIDI input/output and timestamped event scheduling.
2. Add autosave/recovery and persistence for the in-memory undo history around
   the versioned project file format.
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
