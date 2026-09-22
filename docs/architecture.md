# Classical DAW Alpha architecture

This document records the boundaries that keep the audio engine safe to extend.

## Time domains

- `Tick` is the musical edit domain. Alpha fixes the project resolution at 960
  ticks per quarter note.
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
publish events and immutable state snapshots for the UI. A project package will
eventually contain a versioned manifest, score/MIDI data, media, peak caches,
autosave snapshots, and plugin state.

The current score boundary is intentionally small: `Score -> Part -> Measure ->
ScoreNote` keeps written pitch spelling, tick onset/duration, rests, chords, and
ties. The MusicXML adapter runs outside the realtime path and only accepts a
single 960-PPQ part/voice. It can be replaced by a full XML reader later
without changing the callback or transport contracts.

The platform-neutral M0 transport now has a bounded SPSC command ring and a
block scheduler. It is deliberately separate from CoreAudio: device callbacks
will call `processBlock`, while UI/device threads enqueue `Start`, `Stop`,
`SeekSamples`, and `SetTempo` commands. CoreAudio device enumeration and
reconnect still require a macOS adapter and are not claimed by this module.

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
