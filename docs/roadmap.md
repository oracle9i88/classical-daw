# Iteration roadmap

The public repository is an Alpha engine, not a commercial-ready DAW. Each
stage below has a concrete acceptance gate; a feature list or screenshot does
not count as completion.

## Completed in this iteration

- Support MusicXML n/d changes at stored measure starts, synchronized part maps,
  partial/empty bars and trailing silence. Persist explicit measure extents in
  project v6, including final partial bars; read v1–5 with legacy unspecified extents.
  Reject unsupported mid-measure, staff-specific and conflicting part meters.
  Add an independent optional W3C XSD validation tool and compare both meter maps
  and measure starts/durations in the local interchange probe. See the
  [MusicXML meter record](research/2026-09-23-musicxml-meter-interchange.md).
- Preserve all four initial and later meter fields through MIDI ↔ Score ↔
  project v6, history and recovery. Allow up to one million strictly increasing
  positive-tick changes, with a separate 1,000,001-message raw SMF import cap.
  Read v1–4 meters with the original numerator/denominator, default click/notation
  metadata 24/8, and no later meter map; retain v4 tempo changes.
- Build variable measure grids from numerator, denominator and notation ratio:
  structural changes start a new bar at their exact tick and truncate a partial
  old bar; repeated and click-only changes do not restart it. Score import
  requires exact integer 960-PPQ measure lengths, including the notation ratio.
- Fail MusicXML export for notation ratios other than 8 or changes not aligned
  to stored bars. Report omitted clock settings and redundant n/d events in
  `omitted_meter_playback_metadata`. Extend mido comparisons to check
  all four meter fields and their positions. See the
  [meter persistence record](research/2026-09-23-score-meter-persistence.md).
- Preserve initial and later step tempos through MIDI ↔ Score ↔ project v6,
  history and recovery; use them for offline rendering. Read v1–3 projects as
  constant-tempo scores. Preserve the complete map through MusicXML, including
  playback offsets and simple numeric metronome directions; verify held-note
  rendering, sparse part declarations and exact tempo parity on real MIDI files.
- Add one offline, tempo-aware MIDI event timeline with shared channel state,
  sustain/volume/expression/bend and all-notes/sound/reset controls; apply it
  to Score rendering and expose a local MIDI-to-WAV diagnostic CLI. Cover
  audible controller behavior, ordering, note identity, and failure guarantees.
- Preserve CC, Program Change, Pitch Bend, poly/channel pressure, original
  note channels, release velocity, and same-tick order through SMF → Score →
  native project → SMF. Keep event-only tracks and legacy read compatibility.
- Expose omitted performance metadata in MusicXML export reports; realtime
  controller playback and equivalent browser support remain pending.
- Import positive SMF PPQ 1–32767 into the 960-PPQ core using absolute boundary
  rounding, with counters for rounding and ignored events. Reject malformed
  note lifecycles, collapsed notes, and VLQs crossing track boundaries.
- Merge contiguous notated ties into one sustained MIDI/render event; split
  imported notes at barlines with tied notation and reject broken tie chains.
- Preserve note velocity in MusicXML via the standard note `dynamics` attribute,
  independently of future dynamic directions and hairpins.
- Preserve held tones under later attacks in MusicXML, write unequal-duration
  chord tones longest first, and measure all voices' ends when advancing bars.
- Add a local MIDI inspection CLI and an optional independent mido comparator;
  no GitHub workflow is needed for local interchange checks.
- Correctly parse one-byte MIDI channel messages (Program Change and Channel
  Pressure) without consuming the next event.
- Exercise running status, SysEx, text meta events, a real 960-tick note, and a
  truncated channel event in regression fixtures.
- Reject invalid format-0 files containing multiple tracks.
- Reject tempo values that cannot be encoded in MIDI's 24-bit microseconds
  field and guard MIDI tick/duration arithmetic against signed overflow.
- Add a first MusicXML vertical slice: multiple ordered `score-partwise` parts
  with multiple voices/staves, notes, rests, chords, ties, tuplets, meter,
  tempo, and 960-PPQ internal timing/export, including one lyric text per note.
  Normalize positive integral MusicXML divisions independently per part and at
  measure starts; reject unrepresentable ticks instead of rounding the score.
- Add a validated Score-to-SMF bridge that exports ordered multi-part scores to
  deterministic Type 1 MIDI tracks, retains explicit channels or assigns the
  default part channel, and does not damage an existing destination on conversion
  failure.
- Add the inverse MIDI-to-Score bridge: non-empty 960-PPQ tracks become ordered
  score parts with track names, note timing, pitch, velocity, and channel-backed
  voices. The importer carries the initial meter (defaulting to 4/4 until an
  explicit event) and later changes alongside the complete step-tempo map.
- Add the platform-neutral M0 transport skeleton: bounded SPSC command ring,
  block scheduler, and xrun counter with no callback allocation or locks.
- Add the macOS CoreAudio default-output adapter with explicit start/stop and
  silence-safe callback behavior; instrument rendering is still separate.
- Route note events through a fixed polyphonic sine diagnostic voice so the
  callback produces real samples while the production instrument layer is
  still pending.
- Add a versioned score project file with strict validation, atomic replacement,
  and round-trip tests under normal and sanitizer builds.
- Add crash-recovery sidecar writing and primary → recovery → temporary loading
  with failure-preserving tests.
- Add a bounded control-thread ScoreHistory with undo/redo, redo-branch
  invalidation, capacity enforcement, and failure-preserving tests. The history
  is explicitly outside the realtime callback; its current state can be written
  to the atomic recovery sidecar and rebuilt as a clean history after loading.
- Add a macOS CoreMIDI lifecycle adapter with endpoint enumeration, explicit
  source connections, and a receive callback that only counts packets.
- Add a debounced browser-local recovery copy to the web prototype with
  explicit restore/discard controls.
- Add fixed-capacity sample-timestamped voice-event dispatch at realtime block
  boundaries, including late/drop counters and sanitizer coverage.
- Add deterministic Score-to-WAV offline rendering and a browser MIDI/WAV
  interchange path.
- Add configurable 4/8/16-bar web editing with quantized drag, keyboard motion,
  and note copy/paste that remains undoable and recoverable.
- Publish transport snapshots atomically so UI reads cannot race the audio
  callback's block state.
- Run the normal CTest suite and an AddressSanitizer/UndefinedBehaviorSanitizer
  build on every local iteration.

## Next gates

### M0: realtime audio

CoreAudio device-change notifications and reconnect remain, then extend the
existing fixed block scheduler, bounded lock-free command/event queue, and xrun counter. The
CoreMIDI adapter still needs a control-thread timestamp bridge into the new
sample-event queue. The test harness must prove
that the callback performs no allocation, file I/O, JSON parsing, or contended
locks, and that a 48 kHz / 256-frame stream survives a device switch.

### M1: classical editing slice

Prioritize MusicXML measure-label support, fractional source timing, richer tempo notation and
editing commands for re-barring after a meter change, then
extend the offline controller support to realtime playback and add an instrument/port routing layer
(including GM percussion and orchestras spanning multiple MIDI ports).
Channel-event data now survives native interchange and the offline sine
renderer applies a documented subset. Bring the independent web model to parity separately.

Extend the current Score/Part/Measure slice into Staff/Voice/TimedEvent, then
implement MusicXML import/export with tempo, meter, dynamics, ties, tuplets,
and exact tick accounting. Extend the current versioned score file with
engine-level scheduled autosave and persisted full undo history around the
recovery sidecar. The first user-facing vertical slice is MusicXML import →
timeline edit → deterministic WAV export.

### M2: production workflow

Add recording/takes, MIDI event scheduling, buses, automation, plugin scanning in a worker,
latency compensation, stems, and offline bounce. Add a real instrument adapter
before calling the renderer an orchestral solution.

### M3: notation and assisted composition

Add score editing/engraving, articulations, orchestral instrument libraries,
freeze/render caches, and AI services behind the same command boundary. AI must
not run in the realtime callback or become a substitute for deterministic
project state.
