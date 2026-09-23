# Native frozen-track playback

The macOS CLI can play the validated pre-fader audio from an existing session
bundle, with live track mixing and transport. Piano and cello retain their own
audio buffers and share one frame position. This does not run instrument AUs in
realtime: changing score notes or instrument parameters still requires a new
offline render. The browser and a graphical macOS mixer are not integrated yet.

## Run

Build locally and open a previously rendered bundle:

```sh
cmake -S . -B build
cmake --build build --parallel 4
build/daw_session_play path/to/session.dawsession
```

The player starts paused. Type commands followed by Enter:

```text
play
solo cello 1
gain cello -3
balance cello 0.2
master -6
pause
seek 12
play
solo cello 0
mute piano 1
status
stop
quit
```

Use the stable part IDs printed at startup; IDs with spaces can be quoted.
Gain/master are bounded to -60..+12 dB; balance is -1..1; mute/solo require 0/1.
Any solo selects solo tracks only; mute always wins, even on a soloed track.
`pause` retains position, `stop` returns to frame zero. Seek takes seconds in
the session's actual duration, including its render tail. End of audio stops
playback; use `stop` or `seek 0` before replaying from the beginning.

Commands are temporary monitoring adjustments. No source files are changed.
Use `daw_session_edit` to save static mix settings to a new sibling session
and the offline renderer to export WAV. There is no live save/undo control yet.
No automatic normalization is applied. A monitoring clamp prevents samples
outside -1..1 reaching the device; `status` reports pre-clamp last-block peak
and cumulative affected sample count. Lower gains if that count increases.
This clamp is not a mastering limiter. The separate offline bounce still
rejects overload rather than silently applying the monitoring clamp.

## Validation and memory limits

Every route requires a saved state and frozen audio reference. Before opening
the device, the loader validates source identity, CRC, finite samples, common
frame count and SWAM transposition/note range. Stale/corrupt/missing files fail;
there is no automatic plugin reload. All audio is preloaded, limited to 64
tracks / 512 MiB total float samples, plus transient metadata/source overhead.
Long sessions need future streaming support.

The audio thread uses bounded SPSC commands, immutable audio, fixed track state
and lock-free status fields. It does not read files or instantiate plugins.
Track/master changes use 240-frame ramps (5 ms at 48 kHz); transport discontinuities
use short output transitions. Status fields are independent atomic observations.
The source must outlive the output callback; attachment/replacement is only
allowed while output is stopped. One thread owns the command producer.

## CoreAudio correction

The engine's 256-frame processing quantum must not be used as the device AU's
maximum slice capacity. On the tested Mac, the output device used **44.1 kHz /
512 frames**, while the session used **48 kHz**. The old maximum became only
279 frames after conversion and produced **-10874 (TooManyFramesToProcess)**
without invoking the render callback.

The adapter now reads the current device buffer/rate, reserves bounded capacity
with conversion headroom, and splits received buffers into at most 256-frame
engine blocks. The tested AU reported a prepared maximum of 4459 frames.
It preserves the system device sample rate, buffer size and volume. AudioUnit
performs output rate conversion. The callback uses the input-scope render
callback property (the prior capture notification property was also wrong).

Startup requires actual successful callbacks within two seconds. It reports
device/rate/capacity/error diagnostics on failure and disposes the output.
This readiness check now applies to interactive playback as well as probes.
Device changes/disconnect recovery during playback remain unimplemented;
stop and restart after changing devices. Callback errors are not a complete
hardware-underrun/performance measurement.

## Reproducible local checks

```sh
ctest --test-dir build --output-on-failure
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-sanitize --output-on-failure
python3 scripts/check_session_playback.py out/swam-note-audit-20260923/corrected
build/daw_session_play --check out/swam-note-audit-20260923/corrected/session.dawsession
build/daw_session_play --device-check out/swam-note-audit-20260923/corrected/session.dawsession
build/daw_output_probe
```

`--check` processes the entire source through the player without a device or
plugins. It reports peak/RMS, frame position and overload count. The Python
checker additionally compares the original offline WAV, checks gain/mute/solo,
rejects stale/corrupt/symlink caches and overload, and confirms unchanged source
hashes. It expects the original duet fixture with a -3 dB master.

`--device-check` opens the actual output while paused. `daw_output_probe` drives
the actual player through hardware callbacks, measuring mixed samples before
replacing them with zeros sent to the speaker. It tests play/pause/seek/stop,
mute/solo/gain/master, source/device replacement guards and output restart.
These opt-in hardware checks are deliberately outside CTest and GitHub CI.
They do not capture microphone input or change system volume.

Local results: **30/30 normal + 30/30 ASan/UBSan tests passed**, including variable
hardware slice sizes 1/255/256/279/512/558/4459, no frame/channel loss and guarded
zero C++ new/delete in render. **12** CLI integration operations passed on the
actual 1,517,594-frame duet. The paused device check processed **5573** frames;
the live-control hardware probe processed **7245** before its restart check,
with **zero callback errors and LastRenderError=0**, and all controls passed.

These are actual hardware-callback and numerical checks, not a claim of human
listening approval, sample-perfect device resampling, long-duration stability,
allocation interception for every C library, or realtime instrument-plugin support.
