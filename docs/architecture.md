# Classical DAW Alpha architecture

This document records the boundaries that keep the audio engine safe to extend.

## Time domains

- `Tick` is the musical edit domain. Alpha fixes the project resolution at 960
  ticks per quarter note. The SMF reader accepts source PPQ 1–32767 and converts
  absolute note/tempo/channel/meter ticks once, using nearest-tick rounding (half up).
  `MidiImportReport` reports non-integral conversions and skipped event classes;
  it and the destination remain unchanged if import fails. Deltas are never
  individually rounded, avoiding accumulated timing drift.
- `SampleIndex` is the audio domain. `Timeline` converts between ticks and
  samples through a piecewise-constant `TempoMap`.
- Tempo-map edits must keep conversions deterministic and must never let a
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
realtime path and normalizes multiple ordered parts into the 960-PPQ core,
retaining multiple voices/staves inside each measure. Each part inherits its own
positive integral source divisions; an undeclared part retains the legacy 960
fallback. Leading declarations can change at measure boundaries but must precede
notes, cursor moves and playback-tempo events. Durations and playback offsets
convert by exact integer ratio: reduce divisions against 960 before multiplying,
reject a nonintegral result, and check signed overflow before multiplication.
No duration rounding is performed. This deliberately rejects fractional source
divisions/durations/playback offsets and mid-measure resolution declarations.
Integral decimal spellings such as `24.000` are accepted. The stored Score and
MusicXML writer remain at 960 units, so no project version change is needed. It can be replaced by a full XML reader later
without changing the callback or transport contracts.

MIDI channel streams can contain a held tone under later attacks. MusicXML
export uses `backup` to retain those absolute onsets; it does not shorten the
held tone or pretend that a later attack is simultaneous. Same-onset chord
tones are emitted in descending duration order, and import tracks the furthest
note end/forward position when advancing to the next measure. This preserves
performance timing without claiming to infer the composer's notated voices.

The Score-to-MIDI adapter is also outside the realtime path. It validates the
ordered score parts, emits one deterministic Type 1 MIDI track per part, omits
rests, and preserves absolute tick timing and velocity. Imported notes retain
explicit `midi_channel` routes; a newly authored note with channel -1 falls back
to its part index. More than 16 parts require explicit note channels. Tuplet
metadata is represented by the already-resolved tick durations;
contiguous tie chains are matched by part/staff/voice/sounding pitch and become
one sustained MIDI note using the initial velocity. An invalid chain fails
without changing the caller's output. The fallback channel policy still assigns
channel 9 to the tenth part, which is inappropriate for a GM melodic part and
remains an explicit routing limitation.
The score's initial meter and every later change are emitted with all four
SMF fields. MIDI lyric text remains outside this adapter.

The inverse MIDI-to-Score adapter follows the ordered SMF tracks and creates one
score part per track containing notes or channel events; metadata-only tracks are skipped. It retains track
names, note timing, pitch, velocity, and channel (as `voice = channel + 1`),
then assigns notes to a variable measure grid using the complete meter map,
defaulting to 4/4 until an explicit meter applies. It accepts only the engine's 960-PPQ domain and carries tick-zero
tempo into `Score::bpm`, with later entries in `Score::tempo_changes`; canonical sharp spellings are used until a key-aware
notation layer is available. The SMF file reader normalizes source PPQ before
invoking this adapter. Notes crossing barlines are split into notation segments
with ties, and simultaneous segments in a voice receive chord markers. Both directions remain worker-thread operations
and never run in the realtime callback.

`MidiChannelEvent` retains the original channel-voice status type and 7-bit data
bytes plus normalized tick and a per-track source ordinal. Original note attack
and release ordinals share that namespace, preserving their order relative to
CC, program, bend, and pressure events at the same tick. For authored events
with order zero, the writer uses new note-off → source-ordered events → new
channel-event vector order → new note-on at a shared tick. Releasing authored
notes before imported reattacks prevents accidental zero-length retriggers;
relative order within imported events is unchanged. Tie splitting
keeps the attack ordinal only on the first segment and the release ordinal/
velocity only on the last segment. Project v3 persists these fields and all
part events; v1/v2 load with their original defaults. Copy-based history and
recovery retain them without touching the realtime callback. Project v4 adds
`tempo_changes N` and `tempo tick bpm` rows after the initial `bpm` field.
Versions 1–3 retain their existing fields and load with an empty later-tempo
vector. `Score::bpm` is the only tick-zero authority; changing it does not
rescale later entries. `scoreTempoMap` validates positive, strictly increasing
absolute ticks and finite BPM values in (0, 1,000,000], with at most one million
later changes. Duplicate or unsorted entries fail rather than being rewritten.
Undo/redo/snapshot swap this vector along with the rest of the score, and
project/recovery snapshots persist it. The in-memory undo stack is not itself
serialized. Tempo changes during a tied note change elapsed time without
splitting the sounding voice or changing its tick duration.

Project v5 extends `meter` to four values: numerator (`n`), denominator (`d`),
MIDI clocks per metronome click (`cc`), and notated 32nds per MIDI quarter (`bb`).
It then writes `meter_changes N` and `meter_change tick n d cc bb` rows before
`parts`. `Score::time_signature` is the tick-zero authority and
`Score::meter_changes` holds at most one million later entries at strictly
increasing positive ticks. `validateMeterMap` rejects zero numerator, non-power-
of-two denominator, zero `bb`, or invalid ordering; any `cc` byte is retained.
The project reader checks the count before allocation. Versions 1–4 retain
their two-field initial meter with `cc = 24`, `bb = 8` and an empty later map;
v4 keeps its existing tempo changes. History snapshots and recovery carry the
four initial fields and the complete later-meter vector.

The SMF reader accepts at most 1,000,001 raw time-signature messages, allowing
one initial event plus the maximum later map. It normalizes absolute positions
and coalesces messages at a shared normalized tick, reporting both rounding
and coalescing. The canonical model still allows only one million positive-tick
changes. A first event at a positive normalized tick leaves the default initial
4/4 in place until that event applies.

`makeMeasureGrid` uses `960 * 32 * n / (d * bb)` ticks per measure. Changes to
`n`, `d`, or `bb` begin a new bar at the exact change tick and close a partial
old bar when necessary. A change to `cc` alone, or a repeated signature, stays
in the map without restarting the bar. MIDI-to-Score import rejects a meter
whose measure length is not an exact integer in the 960-PPQ domain; raw SMF and
native persistence can retain those metadata bytes without rounding them.
Notes crossing the resulting boundaries are split into tied notation segments.
The mido comparison tools check all four meter fields and their normalized
positions through the SMF and native-project boundaries. See the
[meter persistence record](research/2026-09-23-score-meter-persistence.md).

Editors must clear source ordinals on moved/repitched notes or edited endpoints.
The SMF writer rejects stale explicit ordinals that would put a same-pitch
retrigger before the preceding release, preserving an existing destination.

This is not yet an orchestral interchange model. SysEx is not retained.
The SMF tempo and meter maps survive file import, Score conversion and
native persistence. Keep the import report available to callers rather
than treating a successful parse as proof of a lossless musical round-trip.
MusicXML currently omits raw performance metadata,
exposing the omissions in `MusicXmlExportReport`. Canonical n/d changes at stored
measure boundaries are now retained. `bb != 8`, a change inside an unrebared
measure, or a change beyond every part's stored boundaries fails before writing.
MusicXML writes the global step-tempo map once in the longest synchronized part,
using quarter-note metronome marks plus `offset sound="yes"` and `sound tempo`.
The map must fit within stored measures (a mark at the final end is allowed).
Reading merges sparse declarations from all parts at their actual note cursor
plus playback offset: a sound's offset overrides the direction offset; a direction
with no `sound="yes"` offset leaves playback at its current cursor. Same-tick
identical BPM declarations coalesce; conflicting BPM values fail. Marks never
advance the note cursor, split held notes or extend a measure. A first mark after
tick zero leaves the initial default at 120 BPM. Numeric metronomes without an
explicit sound tempo support beat units maxima through 1024th and up to three
dots. Text/ranges, metric modulation, repeat-specific tempos, fractional playback
offsets and offsets across a measure boundary are refused. Sound tempo is
authoritative when both sound and metronome are present. `omitted_tempo_changes`
remains in the API and is zero on successful export. No project version change
is needed: v6 already retains these tempo fields.

The initial nondefault cc setting counts once in `omitted_meter_playback_metadata`;
each later event with nondefault cc or unchanged n/d counts once more. XML carries
the canonical notation map with default cc/bb, not the raw MIDI event sequence.
Native project/SMF paths retain the raw fields;
the independent web model and realtime CoreAudio path do not inherit this support.

MusicXML attributes are parsed in document-event order with the note/backup/forward
cursor. Only leading time declarations are accepted, so a later declaration cannot
silently be applied to earlier notes. Every part has its own initial/default meter
and subsequent map. Shared spans must have matching barlines and effective meters;
a shorter part can be a prefix. Staff-specific time, polymeter, composite signatures
and non-controlling measures are unsupported. The native measure number remains a
positive integer, a narrower range than MusicXML's arbitrary token labels.

`ScoreMeasure::duration` adds an explicit extent including silence; zero means
legacy unspecified. Project v6 writes a `duration` row after each measure `start`
(note durations remain separate). Versions 1–5 load with zero, without inventing
lost extents. Positive durations must contain notes, avoid tick overflow and meet
the following measure start if one exists. MIDI-to-Score stores computed grid spans;
MusicXML import stores actual parsed spans. History and recovery copy/persist them.

XML export uses an explicit duration when present, otherwise the next start or
active nominal length and furthest note. An unspecified final bar of a shorter
part may use the next known shared boundary. Shorter-than-nominal spans are written
with `implicit="yes"` and positive forward moves to preserve silence after the last
voice. On input, implicit measures use their actual note/forward extent; ordinary
measures retain the existing max(nominal, furthest) behavior. Empty implicit input
with no duration-bearing content fails. A final partial bar retains its duration
through project saving, even with no following start to infer it from. Notated
extent does not change the renderer's existing last-performance-event/tail policy.
See the [MusicXML meter record](research/2026-09-23-musicxml-meter-interchange.md).

The offline `renderMidiFile` merges note edges and channel events across tracks,
with tick → track index → source/fallback order, then schedules each boundary
at its nearest output sample. It shares the within-track comparator with the
SMF writer and rejects the same stale retrigger ordinals. Per-note voice IDs
prevent same-pitch retriggers and cross-track unisons from terminating each
other. All tracks share 16 channel states: CC7/11 gain, CC64 sustain, fixed
two-semitone pitch bend, and CC120/121/123 are interpreted. Frequency changes
preserve oscillator phase. The final mix is clamped once, with clipping counted.
`renderNotes` and `renderScore` delegate to this path with their complete tempo
maps. The renderer uses dynamic storage, runs only
offline, and has no connection to the realtime CoreAudio callback.

The sine envelope has an 8 ms attack and 35 ms release after a key/pedal
release. CC123 honors sustain; CC120 kills voices immediately. CC121 resets
expression, bend and pedal, retains channel volume, and releases only keys
already lifted. Volume/expression use a diagnostic linear curve with initial
values 127. At the last note/event, remaining pedal-held voices are released
and counted. Output ends after the requested tail; a tail shorter than 35 ms
can truncate a release. Program/pressure/RPN and other unsupported messages,
nonzero release velocities, and final forced releases are exposed in the
report. This is not a GM implementation or an orchestral sampler.

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
