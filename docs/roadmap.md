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
- Run the normal CTest suite and an AddressSanitizer/UndefinedBehaviorSanitizer
  build on every local iteration.

## Next gates

### M0: realtime audio

CoreAudio device enumeration and reconnect, a fixed block scheduler, a bounded
lock-free command/event queue, and an xrun counter. The test harness must prove
that the callback performs no allocation, file I/O, JSON parsing, or contended
locks, and that a 48 kHz / 256-frame stream survives a device switch.

### M1: classical editing slice

Define the Score/Part/Staff/Voice/Measure/TimedEvent model, then implement a
single-voice MusicXML import/export round trip with tempo, meter, dynamics,
ties, and exact tick accounting. Add a project package with versioned manifest,
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

