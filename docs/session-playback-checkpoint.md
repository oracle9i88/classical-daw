# Native session playback checkpoint — 2026-09-23

Paused at the user's request before shutting down the machine. This is a work
checkpoint, **not a completed native playback release**.

## Implemented locally

- `SessionPlayer` consumes aligned immutable pre-fader stereo 48 kHz audio.
  Up to 64 tracks / 512 MiB of audio, loaded before callbacks begin.
- Bounded SPSC commands: play/pause/stop, frame seek, track gain/balance,
  mute/solo and master gain. Mute overrides solo, matching offline sessions.
- 240-frame ramps for mix changes; short transitions for transport changes.
  Output clamps above unity for monitoring safety and counts affected samples.
  No automatic loudness normalization, plugin instantiation or source rewrites.
- A borrowed `AudioOutputSource` interface and CoreAudio adapter connection.
- `daw_session_play SESSION` starts paused with textual controls. Mix edits
  are temporary. Existing `daw_session_edit` persists offline settings separately.
- `daw_session_play --check SESSION` validates frozen source/state identity and
  CRC, checks SWAM note ranges, processes the full session without a device or
  instrument plugin, and fails if the monitoring safety clamp was needed.
- `--device-check` tries a bounded paused-output lifecycle test. It requires
  real callback evidence, not merely successful AudioOutputUnitStart.

## Verified

- Normal CTest: 30/30 passed.
- ASan/UBSan CTest: 30/30 passed.
- Player tests cover stereo alignment, offline mix parity, mute/solo priority,
  gains, seek/pause/EOF, command bounds, fade transitions, non-finite input,
  clipping reports, and zero C++ new/delete during guarded render calls.
  This is not a full timing/concurrency/device-switch certification.
- `python3 scripts/check_session_playback.py
  out/swam-note-audit-20260923/corrected`: PASS, 12 local CLI calls, no plugins
  or device opened. Checks quantized offline WAV agreement, gain ratio, mute,
  solo, stale sources, corrupt/symlink caches, overload failure, source hashes.
- Actual duet: 1,517,594 frames, 2 tracks, full callback simulation reached EOF;
  peak 0.142110429556, RMS 0.0250396287613, zero clipped samples.

## Unresolved: hardware output

Sandboxed execution cannot discover the output AU. Approved unsandboxed testing
found an existing wrong callback property in the old CoreAudio adapter:
`kAudioOutputUnitProperty_SetInputCallback` returned OSStatus -10879.
The installed Apple SDK documents that property as an input/capture notification.
It was replaced by `kAudioUnitProperty_SetRenderCallback`, Input scope, bus 0.

With that correction, start succeeds, but the real device check still receives
**zero callbacks**, including a bounded 2-second run-loop startup test. No audible
hardware playback has therefore been demonstrated. Do not call this feature
complete or publish a claim that native playback works on this Mac.

Latest CoreAudio/CLI changes compiled in the normal build; the final callback
property/run-loop changes still need rebuilding in the sanitizer build. They
do not affect the platform-neutral player tests above.

## Resume here

1. Inspect the selected output device, active stream format and AU state with
   bounded diagnostics. Resolve the missing callbacks without changing global
   system volume or device settings. Do not autoplay at startup.
2. Consider making the same callback-readiness check mandatory for interactive
   startup; currently only `--device-check` detects zero-callback startup.
3. Validate actual paused callbacks, then controlled playback and interactive
   controls. Review thread/lifetime safety and callback bounds before release.
4. Rebuild affected normal/sanitizer targets, rerun affected checks and update
   README/roadmap only when hardware evidence supports the claims.

Useful commands from repository root:

```sh
cmake --build build --parallel 4
build/daw_session_play --check out/swam-note-audit-20260923/corrected/session.dawsession
build/daw_session_play --device-check out/swam-note-audit-20260923/corrected/session.dawsession
python3 scripts/check_session_playback.py out/swam-note-audit-20260923/corrected
```

No GitHub workflows were invoked. Development commits must retain `[skip ci]`.
The user confirmed low perceived volume was the computer's own volume setting;
do not raise instrument gain or normalize the project based on that incident.
