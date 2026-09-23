# Classical DAW Alpha

This repository is the first compileable slice of a macOS-oriented classical
music workstation. The engine is self-contained C++17 with no third-party
source copied into the tree. It is intentionally an offline core, suitable for
validating score timing and audio rendering before platform integration.

## What works in Alpha

- A fixed 960 ticks-per-quarter-note score timeline and piecewise constant BPM
  tempo map, with tick/second/sample conversion.
- MIDI note, track, and file data structures.
- Standard MIDI File type 0/1 read/write for notes, release velocities, the five
  non-note channel-voice message types, track names, tempo, and complete meter
  maps retaining numerator, denominator, clocks per click, and notated 32nds
  per quarter. Import accepts positive PPQ divisions 1–32767,
  normalizes absolute note boundaries and tempo/channel/meter positions to 960 PPQ, and
  reports rounding, meter-event coalescing, and skipped events. SMPTE timing is
  not supported.
- A deliberately limited MusicXML `score-partwise` slice: multiple ordered
  parts with multiple voices/staves, 960-PPQ notes and rests, chords, ties, tuplets,
  meter, step-tempo changes, note velocity, and one escaped lyric text per note with deterministic
  import/export. MusicXML import accepts positive decimal source divisions,
  inherits them per part, and supports changes at measure starts. Durations and
  playback offsets must convert exactly to the internal 960-tick grid; export
  remains at 960 divisions. Source decimals are parsed as exact fractions, with
  at most 18 fractional places after trimming trailing zeros and a signed-64-bit
  mantissa. Values outside these bounds, nonintegral internal ticks and mid-measure
  resolution changes are explicitly rejected.
- A deterministic Score-to-SMF bridge for exporting ordered score parts as a
  Type 1 MIDI track per part, retaining note timing, pitch, velocity, voices as
  events, and the complete step-tempo and meter maps. Imported notes retain their original MIDI channels and
  same-tick event order, including bank/program/CC/pressure/bend messages and
  event-only tracks. Newly authored notes use their part's default channel;
  more than 16 parts require explicit note channels. Tuplet spelling is represented by its
  resolved tick durations.
- Tied score notes produce one sustained MIDI note and one renderer attack,
  retaining the first note's velocity. Matching requires contiguous segments
  in the same part/staff/voice and pitch; broken tie chains fail explicitly.
- MusicXML export retains later note attacks beneath held tones using backward
  time-cursor moves, and orders unequal-duration chord tones longest first.
  Imported measure positions account for every voice's end, not just the last
  serialized voice.
- MusicXML meter changes at stored measure boundaries survive across synchronized
  parts. Short measures, empty measures and their trailing silence retain an
  explicit extent, including the final measure. Conflicting part meters or
  unsupported mid-measure declarations fail rather than shifting notes.
- A matching MIDI-to-Score bridge for importing non-empty 960-PPQ tracks as
  ordered score parts for each non-empty track, retaining track names, note
  timing, pitch, velocity, and MIDI channels as score voices and playback metadata. Empty tempo-only
  tracks are skipped. It retains the initial and later MIDI meters, with 4/4
  in effect until an explicit meter arrives, and retains the initial tempo plus
  all later tempo changes through native project persistence. Long notes are
  split into tied notation segments on the variable measure grid. Changes to
  numerator, denominator, or notation ratio start a new bar at their exact tick,
  truncating the old bar if needed; click-only and repeated signatures do not
  restart bars. Fractional 960-PPQ measure lengths fail Score import explicitly.
  The file reader normalizes
  other source PPQs before this bridge runs.
- A platform-neutral realtime transport skeleton with a bounded SPSC command
  ring, block scheduler, and xrun counter.
- On macOS, an optional `daw_coreaudio` target owns the default-output
  device lifecycle, enumerates output devices, supports explicit device
  selection before start, and connects that scheduler to a silence-safe
  callback with a fixed polyphonic sine diagnostic voice.
- A versioned, line-oriented project file format with strict validation and
  atomic replacement. Version 6 adds explicit measure durations, including final
  partial measures and their trailing silence. Zero means the old unspecified
  extent. Version 5 added all four meter fields and the later meter
  map; version 4 added the complete tempo map; version 3 added note routing, message order, release
  velocity, and per-part MIDI events through save/reload, undo/redo, and recovery;
  the reader accepts versions 1–5 with unspecified measure durations. Versions
  1–4 default older meter metadata to 24 clocks
  per click and 8 notated 32nds per quarter with no later meters. Versions 1–3
  remain constant-tempo scores.
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
- `renderMidiFile` applies CC64 sustain, CC7 volume, CC11 expression, fixed
  +/-2-semitone pitch bend, and CC120/121/123 across tracks sharing a channel.
  It uses the complete MIDI tempo map and reports unsupported messages,
  terminal held voices, ignored release velocities, and clipping.
- Deterministic offline mono sine rendering and 16-bit PCM WAV export.
- CTest coverage for tempo conversion, MIDI↔Score/MusicXML round-trips,
  Score-to-MIDI export, project persistence/recovery, bounded edit history,
  realtime primitives, and render smoke tests.

The renderer is a diagnostic instrument, not an orchestral sampler. The
MusicXML reader is a fail-closed first slice; it does not yet cover `.mxl`,
key changes, dynamic directions/hairpins, multiple lyric verses/syllabic metadata, or full XML
validation.
The MIDI bridge requires explicit note channels for scores with more than 16
parts; it does not allocate multiple ports or carry MIDI lyrics. MIDI import
uses canonical sharp pitch spellings; key-aware enharmonic spelling remains
future work. Meter maps allow at most one million later changes at strictly
increasing positive ticks. The SMF reader separately caps raw time-signature
messages at 1,000,001 before coalescing messages at the same normalized tick.
SysEx is counted but not retained. The offline
sine renderer interprets the controls listed above; program changes, pressure,
pan, effects, RPN/bend-range changes and other controls remain unsupported and
are counted. This does not extend the realtime CoreAudio synth. Tempo changes
survive MIDI ↔ Score ↔ native project v6 and affect offline rendering. These
are discrete tempo steps; continuous tempo ramps are not implemented.
Same-channel/pitch note overlaps use LIFO pairing and are reported as ambiguous;
orphan note-offs, unclosed notes, and notes collapsed by PPQ rounding are rejected.
Conversion into Score rejects same-channel/pitch overlaps, which its current
voice model cannot safely distinguish across barlines. Sounding MIDI notes must
have attack velocity 1–127; MusicXML can retain zero dynamics, but the MIDI bridge
rejects it rather than encoding an unintended note-off.
Channel 9 is not yet reserved for percussion by the score exporter: instrument
routing for newly authored notes must be implemented before using this bridge
for a full orchestra. Explicit imported routes are retained.
The current MusicXML notation export omits raw MIDI channel events, note routing,
source-message order and release velocity; `MusicXmlExportReport` counts these
omissions. Step tempos are retained at exact ticks, including changes during held notes.
The reader respects playback offsets and accepts simple numeric metronome marks;
repeat-specific tempos, metric modulation, offsets not exactly representable as
internal ticks and offsets crossing measure boundaries fail explicitly. Tempo changes outside the stored
score extent fail export. No change is silently moved to the start of a measure.
Meter n/d changes at measure starts are retained; nonstandard notation
ratios (`bb != 8`), unrebared changes and conflicting part timelines fail explicitly.
Nondefault MIDI clock settings and redundant n/d events are counted in
`omitted_meter_playback_metadata`. The XML reader accepts leading time declarations
and positive integer measure numbers; staff-specific, composite and mid-measure
meters remain unsupported. Use native projects and MIDI to retain raw playback
fields. The web prototype has
its own model and does not yet share these C++ event-preservation features.
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

Render a local MIDI to a sine diagnostic WAV and JSON report (the output
directory must not exist; optional sample rate defaults to 48000):

```sh
./build/daw_midi_render /path/to/score.midi /path/to/new-diagnostic-output 48000
python3 scripts/check_midi_render_cli.py
```

This is an offline check of note timing and supported performance controls.
It does not load orchestral sounds. Voices release over 35 ms after key/pedal
release; the CLI includes a 100 ms ending tail. Missing final pedal-up is
handled by releasing held voices at the last note/event and reporting the count.
The in-memory output has a default 512 MiB frame budget (about 46.6 minutes
at 48 kHz mono); oversized input fails before audio allocation. C++ hosts can
pass a different frame budget. Streaming long-form bounce remains future work.
See the [render contract and local evidence](docs/research/2026-09-23-midi-control-rendering.md).

Timing is rounded once per absolute boundary to the nearest engine tick, with
half-tick ties rounded up. The maximum error is half a 960-PPQ tick; short notes
that collapse to zero duration fail rather than being silently lengthened.
For an independent event comparison, use a Python environment containing the
optional developer dependency `mido`:

```sh
python3 scripts/check_midi_import.py /path/to/score.midi
python3 scripts/check_midi_performance.py /path/to/score.midi
```

The engine itself has no Python or mido dependency. Local LilyPond evidence and
remaining interchange gaps, plus the commands for the MIDI→MusicXML→MIDI
comparison tool `scripts/check_score_interchange.cpp`, are recorded in
[`docs/research/2026-09-23-lilypond-midi.md`](docs/research/2026-09-23-lilypond-midi.md).
The mido comparisons include all four meter fields and their normalized tick
positions. The performance comparator independently checks channel-message data and
relative order plus canonical tempo and meter maps through direct SMF and native-project
round-trips (normalizing zero-velocity note-on to its equivalent note-off).
Other metadata and SysEx remain outside the comparison.
See [the MIDI performance preservation record](docs/research/2026-09-23-midi-performance-events.md)
for the native-project format change, message ordering rules, and remaining playback limits.
The [tempo persistence record](docs/research/2026-09-23-score-tempo-persistence.md)
documents project v4, legacy compatibility and the multi-tempo corpus checks.
The [meter persistence record](docs/research/2026-09-23-score-meter-persistence.md)
describes project v5, raw meter limits, measure-grid rules and MusicXML boundaries.
The [MusicXML meter interchange record](docs/research/2026-09-23-musicxml-meter-interchange.md)
updates that XML boundary, documents project v6 measure extents and provides
the optional offline W3C schema check.
The [MusicXML tempo interchange record](docs/research/2026-09-23-musicxml-tempo-interchange.md)
documents intra-measure speed changes, offset rules and exact tempo-map checks
on the three real MIDI files.
The [MusicXML divisions record](docs/research/2026-09-23-musicxml-divisions-normalization.md)
covers exact source-unit conversion and equivalent mixed-resolution corpus fixtures.
The [fractional timing record](docs/research/2026-09-23-musicxml-fractional-timing.md)
extends the source units to bounded, exactly parsed decimals.

## Planned macOS slices

1. Add timestamped CoreMIDI event scheduling and CoreAudio device-change
   notifications with automatic reconnect.
2. Add engine-level scheduled autosave and persistence for the full undo history
   around the versioned project file and recovery sidecar.
3. Expose the existing tempo/meter maps in a bar/beat timeline and piano
   roll/score UI (SwiftUI or Qt front end over the C++ engine).
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
