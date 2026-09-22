# Classical DAW Alpha

This repository is the first compileable slice of a macOS-oriented classical
music workstation. The engine is self-contained C++17 with no third-party
source copied into the tree. It is intentionally an offline core, suitable for
validating score timing and audio rendering before platform integration.

## What works in Alpha

- A fixed 960 ticks-per-quarter-note score timeline and piecewise constant BPM
  tempo map, with tick/second/sample conversion.
- MIDI note, track, and file data structures.
- Standard MIDI File type 0/1 read/write for notes, track names, tempo, and a
  single time-signature event (unknown meta/SysEx events are skipped on read;
  unsupported system-common events fail closed).
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
  ordered score parts for each non-empty track, retaining track names, note
  timing, pitch, velocity, and MIDI channels as score voices. Empty tempo-only
  tracks are skipped. It carries the first MIDI meter event, or defaults to 4/4
  when the file has none, and uses the first valid MIDI tempo, so an imported
  file can continue through MusicXML or project persistence.
- A platform-neutral realtime transport skeleton with a bounded SPSC command
  ring, block scheduler, and xrun counter.
- On macOS, an optional `daw_coreaudio` target owns the default-output
  device lifecycle and connects that scheduler to a silence-safe callback with
  a fixed polyphonic sine diagnostic voice.
- A versioned, line-oriented project file format with strict validation and
  atomic replacement, preserving the current score model for save/reload.
- A crash-recovery sidecar writer and loader that tries the primary project,
  `.recovery`, then interrupted `.tmp` data without overwriting the primary
  file or mutating the output on total failure.
- A bounded UI/control-thread `ScoreHistory` with undo/redo, redo-branch
  invalidation, capacity enforcement, and failure-preserving operations. Its
  current state can be snapshotted into the project recovery sidecar; the
  full undo/redo stack remains in memory and is never used by the realtime
  callback.
- On macOS, an optional `daw_coremidi` target owns CoreMIDI client and port
  lifecycle, enumerates sources/destinations, and connects sources explicitly.
  Its receive callback only counts packet lists and does not call the audio
  renderer.
- Deterministic offline mono sine rendering and 16-bit PCM WAV export.
- CTest coverage for tempo conversion, MIDI↔Score/MusicXML round-trips,
  Score-to-MIDI export, project persistence/recovery, bounded edit history,
  realtime primitives, and render smoke tests.

The renderer is a diagnostic instrument, not an orchestral sampler. The
MusicXML reader is a fail-closed first slice; it does not yet cover `.mxl`,
key changes, dynamics, multiple lyric verses/syllabic metadata, or full XML
validation.
The MIDI bridge currently rejects scores with more than 16 parts because the
existing MIDI model does not yet carry a channel-allocation map. It also does
not yet carry meter maps, lyrics, or instrument programs. MIDI import uses
canonical sharp pitch spellings and preserves one meter event; key-aware
enharmonic spelling, meter changes, and program metadata remain future fields.
There is no production instrument library, score engraving, automation, mixer,
plugin hosting, scheduled autosave, persisted undo history, or multi-part project
package yet.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

## Planned macOS slices

1. Add timestamped CoreMIDI event scheduling and CoreAudio device enumeration/
   reconnect.
2. Add engine-level scheduled autosave and persistence for the full undo history
   around the versioned project file and recovery sidecar.
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
