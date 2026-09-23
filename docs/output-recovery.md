# Native output interruption and reconnect

The resident and streamed native CLI now detect output problems on their control
thread and expose an explicit paused reconnect. This preserves the loaded score,
frozen readers, current position, accepted mix targets, undo/redo and recovery
checkpoints without reloading instrument plugins.

```text
devices
disconnect
status
reconnect
status
play
```

`devices` prints available output IDs and the system default. `disconnect` stops
the host output and pauses at its current position. `reconnect` retries the last
configured selection; `reconnect 0` selects the current system default, and
`reconnect DEVICE_ID` explicitly selects an enumerated device. The selection is
for this host only: these commands do not change system volume, default device,
hardware sample rate or buffer size. IDs are ephemeral and not project settings.

Reconnect always leaves the source paused. Only `play` resumes the musical
clock; the CLI rejects play while output is disconnected. Invalid/unavailable
device selection or startup failure leaves the project loaded and paused, with
an error and no automatic retry. If initial device startup fails, ordinary CLI
mode still allows mix edits/saves and another explicit reconnect. Diagnostic
`--device-check` modes retain their nonzero failure exit.

## Detection and state ownership

While waiting for input, the control thread polls every 100 ms. Partial pipe
commands do not block health checks. The host observes the active AU device,
device liveness, nominal sample rate, buffer frame size and callback counters.
In system-default mode it also checks whether the default output changed; an
explicitly selected device is not invalidated solely by an unrelated default
change. Unreadable properties, missing/changing devices, rate/buffer changes or
callback errors latch a fault. Two seconds without new callback frames also
latches a fault. Healthy observations cannot silently clear it; a fresh start
establishes a new baseline after callback readiness has succeeded.

On detection, the CLI stops/disposes the output before taking over the sole
player consumer. `suspendAfterOutputStopped()` then drains accepted commands
without rendering audio, forces pause, settles mix targets and clears transition
samples. It preserves queued seeks/stops and the last position; a queued play
cannot make reconnect audible. The API must **never** be called concurrently
with an active render callback. The audio callback has no new property queries,
file I/O, waits or allocations.

Mix edits and transport seek/stop remain available while disconnected. Accepted
mix changes use the same history, save and recovery code as connected playback.
Stopped-state command draining prevents queue saturation and keeps those edits
from being lost when a device no longer pulls callbacks. This is runtime device
recovery, not persistence of transport position across process crashes.

## Limits

This is polling, not an instantaneous hardware interlock or notification-based
hot swap. A route can change or audio can advance before the next poll; slow
control-thread filesystem work or CoreAudio calls can delay detection further.
Brief changes between observations can be missed. The underlying default output
AU may itself follow a system route change before this host notices. Do not
interpret the paused-reconnect policy as a guarantee of zero sound on a newly
selected system route. Physical unplug/replug, Bluetooth transitions, sleep/wake,
long sessions and driver-specific failures still need manual hardware validation.

The callback-stall timeout intentionally reports failure rather than trying
repeated device starts. Startup still has a bounded two-second callback-readiness
wait. There is no seamless device handover, timestamp reconciliation, automatic
resume, permanent device-UID selection or plugin/live-MIDI recovery in this slice.

## Local evidence — 2026-09-23

- **37/37** normal and **37/37** ASan/UBSan CTest cases passed. Injected snapshots
  cover device disappearance, route/default changes, unreadable properties,
  rate/buffer changes, callback errors, exact stall deadline, latched faults and
  explicit rearm. These tests do not pretend to unplug a physical device.
- Player tests cover a full pending command queue, pause/position retention,
  accepted gain/mute/master edits, disconnected seek/stop and zero residual
  transition audio after restart.
- Actual CoreAudio probes passed in resident and streaming modes: stop while
  playing, retain position, restart paused, explicitly resume, and undo a mix
  edit accepted while disconnected. Device callbacks reported zero errors;
  the observing wrapper zeroed all samples before they reached the speaker.
  The local device used 44.1 kHz with a 48 kHz client stream.
- Both CLI modes passed invalid-device reconnect, successful default reconnect,
  position/history/save preservation, partial input and EOF-without-newline
  checks on a temporary real duet bundle. Source hashes stayed unchanged. No
  system device settings or volume were modified.
- The final probes also selected the same real device explicitly, verified
  no-argument reconnect, and passed the streaming hardware/CLI checks under
  ASan/UBSan. Existing live-save and forced-exit mix-recovery checks passed in
  both modes after the command-input loop change.

Reproduce locally (hardware probes are opt-in and excluded from CTest/CI):

```sh
ctest --test-dir build --output-on-failure
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-sanitize --output-on-failure
build/daw_output_probe
build/daw_output_probe --stream
python3 scripts/check_output_reconnect.py out/swam-note-audit-20260923/corrected
python3 scripts/check_output_reconnect.py out/swam-note-audit-20260923/corrected --stream
```
