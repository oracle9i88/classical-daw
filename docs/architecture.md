# Classical DAW Alpha architecture

This document records the boundaries that keep the audio engine safe to extend.

## Time domains

- `Tick` is the musical edit domain. Alpha fixes the project resolution at 960
  ticks per quarter note. The SMF reader accepts source PPQ 1–32767 and converts
  absolute start/end/tempo ticks once, using nearest-tick rounding (half up).
  `MidiImportReport` reports non-integral conversions and skipped event classes;
  it and the destination remain unchanged if import fails. Deltas are never
  individually rounded, avoiding accumulated timing drift.
- `SampleIndex` is the audio domain. `Timeline` converts between ticks and
  samples through a piecewise-constant `TempoMap`.
- The future tempo map must keep conversions deterministic and must never let a
  UI floating-point value become the source of truth for audio scheduling.

## Realtime boundary

The future CoreAudio callback must only consume preallocated buffers and a
bounded stream of timestamped events. It must not allocate memory, take a
contended lock, read files, parse JSON, scan plugins, or make network calls.
Project edits will be represented as commands and applied at an audio-block
boundary. Disk reads belong in a worker plus a ring buffer; peak generation and
autosave belong outside the callback.

## Project and engine split

The UI will submit commands such as `AddTrack`, `MoveClip`, `InsertNote`,
`SetTempo`, `SetParameter`, `StartTransport`, and `Render`. The engine will
publish events and immutable state snapshots for the UI. The current project
serializer provides a versioned score save/reload boundary; a full project
package will eventually add MIDI data, media, peak caches, autosave snapshots,
and plugin state.

`ScoreHistory` is the UI/control-thread edit-history boundary around this score
state. It owns bounded copies, retains the initial state, clears the redo
branch after a successful commit, and leaves the history unchanged on a failed
operation. Its current state can be copied into an atomic `.recovery` sidecar
and loaded into a new clean history; the full undo/redo stack is still an
in-memory concern. It must never be called from the realtime callback. The
project loader checks the primary file, a `.recovery` sidecar, and an
interrupted `.tmp` candidate in that order, while recovery writes use the same
atomic replacement boundary and never run from realtime code.

The current score boundary is intentionally small: `Score -> Part -> Measure ->
ScoreNote` keeps written pitch spelling, tick onset/duration, velocity, rests,
chords, ties, and one optional lyric syllable. MusicXML note `dynamics` expresses
velocity as a percentage of MIDI forte 90; this is not a hairpin/direction model.
The MusicXML adapter runs outside the
realtime path and accepts multiple ordered 960-PPQ parts, retaining multiple
voices/staves inside each measure. It can be replaced by a full XML reader later
without changing the callback or transport contracts.

MIDI channel streams can contain a held tone under later attacks. MusicXML
export uses `backup` to retain those absolute onsets; it does not shorten the
held tone or pretend that a later attack is simultaneous. Same-onset chord
tones are emitted in descending duration order, and import tracks the furthest
note end/forward position when advancing to the next measure. This preserves
performance timing without claiming to infer the composer's notated voices.

The Score-to-MIDI adapter is also outside the realtime path. It validates the
ordered score parts, emits one deterministic Type 1 MIDI track per part, omits
rests, preserves absolute tick timing and velocity, and assigns each part a
stable channel (`part index % 16`). Export rejects more than 16 parts because
the current bridge has no channel-allocation map and must not silently collide
parts. Tuplet metadata is represented by the already-resolved tick durations;
contiguous tie chains are matched by part/staff/voice/sounding pitch and become
one sustained MIDI note using the initial velocity. An invalid chain fails
without changing the caller's output. The current channel policy also assigns
channel 9 to the tenth part, which is inappropriate for a GM melodic part and
remains an explicit routing limitation.
the score's single meter is emitted as the first MIDI time-signature event,
while meter changes, lyric text, and instrument programs remain future fields.

The inverse MIDI-to-Score adapter follows the ordered SMF tracks and creates one
score part per non-empty track; tempo-only tracks are skipped. It retains track
names, note timing, pitch, velocity, and channel (as `voice = channel + 1`),
then assigns notes to a measure grid using the first MIDI meter event, defaulting
to 4/4. It accepts only the engine's 960-PPQ domain and carries the first valid
tempo into `Score::bpm`; canonical sharp spellings are used until a key-aware
notation layer is available. The SMF file reader normalizes source PPQ before
invoking this adapter. Notes crossing barlines are split into notation segments
with ties, and simultaneous segments in a voice receive chord markers. Both directions remain worker-thread operations
and never run in the realtime callback.

This is not yet an orchestral interchange model. SMF control/program changes,
pitch bend, pressure, SysEx, later meter events, and release velocities are not
retained. The SMF tempo map survives file import, but the single-BPM Score cannot
retain its later changes. Keep the import report available to callers rather
than treating a successful parse as proof of a lossless musical round-trip.

The platform-neutral M0 transport now has a bounded SPSC command ring and a
block scheduler. It is deliberately separate from CoreAudio: device callbacks
call `processBlock`, while UI/device threads enqueue `Start`, `Stop`,
`SeekSamples`, and `SetTempo` commands. A sequence-published atomic snapshot
keeps UI reads from racing the audio-owned transport state. The scheduler also
accepts a fixed-capacity queue of absolute sample-timestamped `VoiceEvent`
values, sorts them into a preallocated pending array, dispatches events due in
the current block to the prepared renderer, and counts late/overflow events.
The macOS adapter owns the default output lifecycle, enumerates output-capable
devices, and allows explicit device selection before starting. The CoreMIDI adapter separately owns client
and port lifecycle, enumerates endpoints, and requires explicit source
connections; its receive callback only increments an atomic packet counter.
CoreAudio device-change notifications/reconnect and the CoreMIDI control-thread
timestamp bridge remain future work.

## Delivery slices

1. **Alpha (current):** tempo/tick/sample conversion, SMF type 0/1 note
   round-trip, deterministic offline sine render and PCM16 WAV output.
2. **M0:** CoreAudio output, device enumeration, block scheduling, xrun status,
   and a lock-free transport command queue.
3. **M1:** MusicXML/MIDI project import, piano roll, tempo/time-signature
   markers, project save/recovery, and a native timeline UI.
4. **M2:** audio input recording, takes/comping, AU/VST3 scanning in a worker,
   plugin latency compensation, automation, buses, stems, and offline bounce.
5. **M3:** notation view, articulations, orchestral instrument adapters, freeze,
   crash-safe plugin worker, and AI services isolated from realtime processing.

The Alpha renderer is intentionally a diagnostic voice. It must not be
described as a finished sampler, orchestral engine, or professional mix bus.
