# Iteration roadmap

The public repository is an Alpha engine, not a commercial-ready DAW. Each
stage below has a concrete acceptance gate; a feature list or screenshot does
not count as completion.

## Completed in this iteration

- Correctly parse one-byte MIDI channel messages (Program Change and Channel
  Pressure) without consuming the next event.
- Exercise running status, SysEx, text meta events, a real 960-tick note, and a
  truncated channel event in regression fixtures.
- Reject invalid format-0 files containing multiple tracks.
- Reject tempo values that cannot be encoded in MIDI's 24-bit microseconds
  field and guard MIDI tick/duration arithmetic against signed overflow.
- Add a first MusicXML vertical slice: multiple ordered `score-partwise` parts
  with multiple voices/staves, notes, rests, chords, ties, tuplets, meter,
  tempo, and 960-PPQ import/export, including one lyric text per note.
- Add a validated Score-to-SMF bridge that exports ordered multi-part scores to
  deterministic Type 1 MIDI tracks, assigns stable channels 0–15, rejects more
  than 16 parts, and does not damage an existing destination on conversion
  failure.
- Add the platform-neutral M0 transport skeleton: bounded SPSC command ring,
  block scheduler, and xrun counter with no callback allocation or locks.
- Add the macOS CoreAudio default-output adapter with explicit start/stop and
  silence-safe callback behavior; instrument rendering is still separate.
- Route note events through a fixed polyphonic sine diagnostic voice so the
  callback produces real samples while the production instrument layer is
  still pending.
- Add a versioned score project file with strict validation, atomic replacement,
  and round-trip tests under normal and sanitizer builds.
- Run the normal CTest suite and an AddressSanitizer/UndefinedBehaviorSanitizer
  build on every local iteration.

## Next gates

### M0: realtime audio

CoreAudio device enumeration and reconnect, then extend the existing fixed
block scheduler, bounded lock-free command/event queue, and xrun counter. The
test harness must prove
that the callback performs no allocation, file I/O, JSON parsing, or contended
locks, and that a 48 kHz / 256-frame stream survives a device switch.

### M1: classical editing slice

Extend the current Score/Part/Measure slice into Staff/Voice/TimedEvent, then
implement MusicXML import/export with tempo, meter, dynamics, ties, tuplets,
and exact tick accounting. Extend the current versioned score file with
autosave, recovery, and persisted undo history. The first user-facing vertical
slice is MusicXML import → timeline edit → deterministic WAV export.

### M2: production workflow

Add recording/takes, CoreMIDI, buses, automation, plugin scanning in a worker,
latency compensation, stems, and offline bounce. Add a real instrument adapter
before calling the renderer an orchestral solution.

### M3: notation and assisted composition

Add score editing/engraving, articulations, orchestral instrument libraries,
freeze/render caches, and AI services behind the same command boundary. AI must
not run in the realtime callback or become a substitute for deterministic
project state.
