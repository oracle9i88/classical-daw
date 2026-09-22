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
  single time-signature event. Import accepts positive PPQ divisions 1–32767,
  normalizes absolute note boundaries and tempo positions to 960 PPQ, and
  reports rounding and skipped events. SMPTE timing is not supported.
- A deliberately limited MusicXML `score-partwise` slice: multiple ordered
  parts with multiple voices/staves, 960-PPQ notes and rests, chords, ties, tuplets,
  meter, tempo, note velocity, and one escaped lyric text per note with deterministic
  import/export.
- A deterministic Score-to-SMF bridge for exporting ordered score parts as a
  Type 1 MIDI track per part, retaining note timing, pitch, velocity, voices as
  events, and BPM. Parts receive stable channels 0–15; export rejects more than
  16 parts instead of colliding channels. Tuplet spelling is represented by its
  resolved tick durations.
- Tied score notes produce one sustained MIDI note and one renderer attack,
  retaining the first note's velocity. Matching requires contiguous segments
  in the same part/staff/voice and pitch; broken tie chains fail explicitly.
- MusicXML export retains later note attacks beneath held tones using backward
  time-cursor moves, and orders unequal-duration chord tones longest first.
  Imported measure positions account for every voice's end, not just the last
  serialized voice.
- A matching MIDI-to-Score bridge for importing non-empty 960-PPQ tracks as
  ordered score parts for each non-empty track, retaining track names, note
  timing, pitch, velocity, and MIDI channels as score voices. Empty tempo-only
  tracks are skipped. It carries the first MIDI meter event, or defaults to 4/4
  when the file has none, and uses the first valid MIDI tempo, so an imported
  file can continue through MusicXML or project persistence. Long notes are
  split into tied notation segments at barlines. The file reader normalizes
  other source PPQs before this bridge runs.
- A platform-neutral realtime transport skeleton with a bounded SPSC command
  ring, block scheduler, and xrun counter.
- On macOS, an optional `daw_coreaudio` target owns the default-output
  device lifecycle, enumerates output devices, supports explicit device
  selection before start, and connects that scheduler to a silence-safe
  callback with a fixed polyphonic sine diagnostic voice.
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
  Its receive callback only counts packets and does not call the audio
  renderer.
- The realtime scheduler accepts fixed-capacity sample-timestamped voice
  events, dispatches them at block boundaries, and reports late or dropped
  events without allocating in the callback.
- `renderScore` converts the strict score model to a deterministic offline
  mono WAV diagnostic mix, preserving all score parts before the final clamp.
- Deterministic offline mono sine rendering and 16-bit PCM WAV export.
- CTest coverage for tempo conversion, MIDI↔Score/MusicXML round-trips,
  Score-to-MIDI export, project persistence/recovery, bounded edit history,
  realtime primitives, and render smoke tests.

The renderer is a diagnostic instrument, not an orchestral sampler. The
MusicXML reader is a fail-closed first slice; it does not yet cover `.mxl`,
key changes, dynamic directions/hairpins, multiple lyric verses/syllabic metadata, or full XML
validation.
The MIDI bridge currently rejects scores with more than 16 parts because the
existing MIDI model does not yet carry a channel-allocation map. It also does
not yet carry meter maps, lyrics, or instrument programs. MIDI import uses
canonical sharp pitch spellings and preserves one meter event; key-aware
enharmonic spelling, meter changes, and program metadata remain future fields.
MIDI CC (including sustain pedal), pitch bend, pressure, SysEx, and subsequent
meter events are counted but not retained. The MIDI model retains tempo changes,
but conversion into the current single-BPM Score discards subsequent tempos.
Same-channel/pitch note overlaps use LIFO pairing and are reported as ambiguous;
orphan note-offs, unclosed notes, and notes collapsed by PPQ rounding are rejected.
Conversion into Score rejects same-channel/pitch overlaps, which its current
voice model cannot safely distinguish across barlines. Sounding MIDI notes must
have attack velocity 1–127; MusicXML can retain zero dynamics, but the MIDI bridge
rejects it rather than encoding an unintended note-off.
Channel 9 is not yet reserved for percussion by the score exporter: instrument
routing must be implemented before using this bridge for a full orchestra.
There is no production instrument library, score engraving, automation, mixer,
plugin hosting, scheduled autosave, persisted undo history, or multi-part project
package yet.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Inspect a local MIDI file without playback or modifying it:

```sh
./build/daw_midi_inspect /path/to/score.midi
./build/daw_midi_inspect /path/to/score.midi --notes
```

Timing is rounded once per absolute boundary to the nearest engine tick, with
half-tick ties rounded up. The maximum error is half a 960-PPQ tick; short notes
that collapse to zero duration fail rather than being silently lengthened.
For an independent event comparison, use a Python environment containing the
optional developer dependency `mido`:

```sh
python3 scripts/check_midi_import.py /path/to/score.midi
```

The engine itself has no Python or mido dependency. Local LilyPond evidence and
remaining interchange gaps, plus the commands for the MIDI→MusicXML→MIDI
comparison tool `scripts/check_score_interchange.cpp`, are recorded in
[`docs/research/2026-09-23-lilypond-midi.md`](docs/research/2026-09-23-lilypond-midi.md).

## Planned macOS slices

1. Add timestamped CoreMIDI event scheduling and CoreAudio device-change
   notifications with automatic reconnect.
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
instructions. It supports 4/8/16-bar piano-roll editing, MIDI Type 0/1 import
and Type 1 export, multi-voice/tied MusicXML interchange, JSON recovery, and
PCM16 WAV export. Run
`python3 -m http.server 8080 --directory web` from the repository root for a
local preview.
