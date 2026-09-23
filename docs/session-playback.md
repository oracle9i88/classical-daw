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
undo
redo
pause
seek 12
play
solo cello 0
mute piano 1
status
mix
save cello-balance.dawsession
stop
quit
```

Use the stable part IDs printed at startup; IDs with spaces can be quoted.
For output interruptions, use `devices`, `disconnect`, and `reconnect [DEVICE_ID]`.
The CLI polls device health while waiting for input; on detected failure it stops
output and retains position/mix/history. Reconnect stays paused until `play`.
See [recovery commands, polling limits and verification](output-recovery.md).
Gain/master are bounded to -60..+12 dB; balance is -1..1; mute/solo require 0/1.
Any solo selects solo tracks only; mute always wins, even on a soloed track.
`pause` retains position, `stop` returns to frame zero. Seek takes seconds in
the session's actual duration, including its render tail. End of audio stops
playback; use `stop` or `seek 0` before replaying from the beginning.

Mix commands pass through the shared control-thread `SessionMixState`, also
used by `daw_session_edit`. `mix` prints the current accepted target settings;
`save NEW_FILENAME` saves them to a new sibling session referencing the same
score, state and frozen audio. Existing files are never overwritten, including
dangling symlinks. Invalid edits or a full audio command queue leave the saved
target state unchanged. The save records accepted targets, not intermediate
gain-ramp samples. Reopen the new session to restore them; use the offline
renderer to export WAV. Original source files remain unchanged.

`undo` and `redo` restore accepted mix targets through the same playback queue,
without seeking or restarting playback. The default history holds the most
recent 128 single-parameter edits. New edits after undo discard the old redo
branch; setting an already-current value is a no-op and preserves that branch.
Invalid edits, queue overflow or preparation/allocation failure leave targets,
history and revision unchanged. Track mute/solo priority remains the same.
`status` reports the mix revision and available undo/redo directions. Successful
edits, undo and redo increment the revision; transport and no-ops do not.

Saving keeps the current target settings and leaves the in-memory history
available. Reopening starts a new history at revision zero. The undo stack is
not serialized, nor is it yet unified with score edits. Multi-parameter grouped
edits, drag coalescing, scheduled whole-project autosave and whole-project
transactions remain pending. Mix-only automatic recovery is described below. Gain ramps still take 5 ms to reach restored targets.

New-session saving writes a complete staged file and atomically publishes it
with an exclusive hard link. Unsupported filesystems fail explicitly; no
power-loss durability is claimed. A stale `FILENAME.saving` directory left by
a crash is preserved, not deleted automatically; choose a different new name.
No automatic normalization is applied. A monitoring clamp prevents samples
outside -1..1 reaching the device; `status` reports pre-clamp last-block peak
and cumulative affected sample count. Lower gains if that count increases.
This clamp is not a mastering limiter. The separate offline bounce still
rejects overload rather than silently applying the monitoring clamp.

## Automatic mix recovery

After each accepted mix edit, undo or redo, the native CLI writes a checkpoint
on its control thread. Playback callbacks never access these files. The first
edit creates a unique sibling `SESSION.mix-recovery-*` directory. Each run owns
its own directory, so two players cannot replace one another's checkpoints.
`--check` and `--device-check` do not create recovery data.

The directory contains the original session and score bytes for identity
checking, plus `latest.mixrecovery`: versioned `DAWMIX01`, a 64-bit revision,
bounded session payload and CRC32 for accidental corruption. A full staged file
is closed then atomically renamed over that run's last checkpoint. If a process
dies during writing, its earlier complete checkpoint remains usable; temporary
partials are never treated as accepted checkpoints. There is no fsync or
power-loss guarantee. The last edit can be lost if the process dies before its
checkpoint finishes. No claim of lossless recovery from every crash is made.

`status` reports both the current revision and the recovery's saved revision.
If disk I/O fails, the accepted mix stays applied and the CLI explicitly prints
`recovery NOT saved`. Use `recovery` to retry, or `save NEW_FILENAME` to save a
normal sibling session. A successful no-op edit also retries a pending checkpoint.
Invalid commands do not create new checkpoints. A stale `.saving` directory is
preserved rather than silently removed; its last complete checkpoint can still
be recovered, and a fresh player run uses a different recovery directory.

At startup the CLI reports existing candidates, but always opens the requested
session. Inspect and explicitly restore to a **new sibling session**:

```sh
build/daw_session_recover --list path/to/session.dawsession
build/daw_session_recover path/to/session.dawsession path/to/session.dawsession.mix-recovery-ID path/to/recovered.dawsession
build/daw_session_play path/to/recovered.dawsession
```

Recovery checks exact original session/score bytes and permits only mix-field
changes. Stale source data, corrupt/truncated records, symlinks and an existing
destination are rejected. Media/plugin-state files remain external references;
normal frozen-playback validation still checks their binding before playback.
This restores mix targets, not transport, undo history, media, plugin state or
a full document transaction. Reopening starts a fresh history. Checkpoints are
retained after save/quit and never automatically pruned: each edited run stores
one score copy (up to 64 MiB), one source session and one latest mix. After
verifying recovery or a manual save, unwanted recovery directories can be removed
manually. Generated recovery data is ignored by Git.

## Validation and memory limits

Every route requires a saved state and frozen audio reference. Before opening
the device, the loader validates source identity, CRC, finite samples, common
frame count and SWAM transposition/note range. Stale/corrupt/missing files fail;
there is no automatic plugin reload. Default resident playback is limited to
64 tracks / 512 MiB total float samples, plus transient metadata/source overhead.
Use the optional streaming path below to avoid the aggregate audio-memory limit.

The audio thread uses bounded SPSC commands, owned audio pages, fixed track state
and lock-free status fields. It does not read files or instantiate plugins.
Track/master changes use 240-frame ramps (5 ms at 48 kHz); transport discontinuities
use short output transitions. Status fields are independent atomic observations.
The source must outlive the output callback; attachment/replacement is only
allowed while output is stopped. One thread owns the command producer.

## Disk streaming

```sh
build/daw_session_play --stream path/to/session.dawsession
build/daw_session_play --stream-check path/to/session.dawsession
build/daw_session_play --stream-device-check path/to/session.dawsession
```

`--stream` uses the existing player, transport, mix edits, undo/redo, saving and
recovery. A single worker reads four fixed 4096-frame pages containing all tracks
at the same position. The render callback only acquires a ready page and reads
samples; it never performs file I/O, allocates, sleeps or waits for the worker.
This takes **128 KiB of audio page buffers per track**, or **8 MiB at 64 tracks**,
independent of session duration. This number excludes score, MIDI plans, plugin
state/identity metadata, file-stream buffers, stacks and OS caches; it is not a
whole-process memory measurement.

Opening still scans every frozen file completely, checking source identity,
CRC, timing and finite samples before audio starts. Validation uses a 64 KiB
scratch block instead of loading the whole waveform or copying a second large
identity string. Large sessions therefore have bounded audio memory, but still
incur startup disk I/O proportional to all files. Keep cache contents immutable
while open. Later truncation/read failures and non-finite samples are detected;
finite in-place mutations after validation are not continuously checksummed.

The consumer publishes the requested page; the worker prefetches it and up to
three following pages. Acquire/release ownership prevents a page being replaced
while rendering it. Seek abandons old page requests; stopped/paused callbacks
also request the target so it can be ready before play. Construction preloads
the first pages, and destruction stops/joins the worker only after audio stops.
Disk reads in progress cannot be forcibly interrupted; storage stalls may delay
shutdown on the control thread.

If a whole aligned page is unavailable, **every track holds the same musical
position** and output fades to silence; position resumes when the page arrives.
It never plays only the tracks that happened to load. Mix ramps and commands
continue while buffering. `status` exposes `buffering`, cumulative
`buffering frames` (output frames spent waiting), and `stream failed`. Disk failure
is terminal for that open stream: pause/stop still work, then reopen after fixing
the files. This avoids musical-position drift but can create an audible gap and
wall-clock delay. It is not a guarantee of dropout-free streaming.

Streaming score planning and `FrozenTrackReader` now allow **two hours per
track including release tail**, at fixed 48 kHz stereo (345,600,000 frames).
The existing DAWFRZ01 uint32 frame field already accommodates this duration;
no file-layout migration is needed, and byte offsets use 64-bit arithmetic.
The 64-track limit is unchanged. Buffer-based APIs and the default offline
session bounce retain the old 32 Mi-frame session budget (about 11.65 minutes
including tail). The new renderer `--stream`/`--stream-frozen` modes also use the
two-hour budget; see [streamed export and verification](streaming-export.md).
Real long plugin performances remain an endurance-test gap. The stream is fixed
48 kHz stereo frozen audio, not realtime AU instruments, recording or editable
clips. Browser/native document integration and GUI controls remain separate work.

`--stream-check` waits for pages on its non-realtime control thread before each
render block to compare offline results deterministically. It cannot measure
hardware underruns or disk deadlines. `--stream-device-check` starts/stops real
output while paused; the explicit probe below also exercises playing/seek/mix.

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
python3 scripts/check_session_live_save.py out/swam-note-audit-20260923/corrected
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
`check_session_live_save.py` additionally tests native commands, save/reopen,
invalid edits and overwrite protection on a temporary copy while staying paused.
It also saves an undone mix, a redone mix and a new history branch, checks their
reopened audio level ratios, and verifies a fresh history after reopening.
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

The history iteration passed **32/32 normal + 32/32 ASan/UBSan tests**. The new
tests cover all five mix parameters, bounded-history eviction, branch semantics,
no-op preservation, queue-full rollback and injected C++ allocation failures
during edit/undo/redo (including unchanged audio targets after failure).
The actual hardware probe also verified undo/redo levels through the shared
mix controller, with 10,032 frames before restart and zero callback errors;
speaker output remained silenced. The native save/reopen checker passed the
undo/redo/branch checks on the real piano/cello bundle without modifying it.

The mix recovery iteration passed **33/33 normal + 33/33 ASan/UBSan tests**.
The opt-in test below runs on a temporary real piano/cello bundle, exercises
write failure with a preserved partial staging file, retries the same revision,
undoes an edit and then force-kills only its own paused player process after
checkpoint revision 8. Restored settings and complete numerical playback output
match the independently edited reference; source hashes are unchanged. Normal
native save/reopen regressions also pass. No sound is emitted by these checks.

```sh
python3 scripts/check_session_recovery.py out/swam-note-audit-20260923/corrected
```

Streaming validation: **34/34 normal + 34/34 ASan/UBSan tests passed**. The stream
suite also passed **ThreadSanitizer**, with 300 page-position changes under a
concurrent worker. It checks exact sample parity, shared mix undo, pause/seek,
EOF, stop/restart, reader bounds/corruption, worker read failure, aligned clock
freeze and zero intercepted C++ allocation/deallocation in render.

The optional capacity test traverses all 1,100,000 frames of 64 synthetic routes
using independent readers of one constant test file: 563,200,000 bytes (~537 MiB)
would be required for resident audio, while streaming audio pages stay at
8,388,608 bytes. It passed normally and under ASan/UBSan, with no buffering after
control-thread prefetch. This is a capacity/numerical check, not a 64-instrument
real-device performance claim or a measured process RSS figure.

The real piano/cello bundle's `--stream-check` matched resident output exactly
in frame count, peak, RMS, clipping and end state. Streamed native save/reopen
and forced-exit recovery checks also passed. Both resident and streaming hardware
probes processed 10,032 frames before restart with zero callback errors; streaming
reported zero buffering frames. Their speaker output was silenced throughout.
Reproduce locally, without GitHub Actions:

```sh
build/daw_streaming_audio_tests --capacity
build/daw_output_probe --stream
python3 scripts/check_session_live_save.py out/swam-note-audit-20260923/corrected --stream
python3 scripts/check_session_recovery.py out/swam-note-audit-20260923/corrected --stream
cmake -S . -B build-stream-tsan -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g" -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread
cmake --build build-stream-tsan --target daw_streaming_audio_tests --parallel 4
build-stream-tsan/daw_streaming_audio_tests
```

## Long-track write and planning boundary

`SessionPlanMode::Streaming` separates the duration budget from buffered audio
allocation. It is selected by all native `--stream*` modes; ordinary render and
resident-playback callers retain the buffered default. Both modes share the
same tempo mapping, score-part routing and common release/tail boundary. A
streaming plan over two hours (including tail), or a non-48-kHz request, fails
before audio is allocated. The limit is a supported budget, not a two-hour
hardware-soak-test claim.

`FrozenTrackWriter` provides a control/worker-thread chunk API for future
streamed render/export producers. Declare total frames, append at most 8192
stereo frames per call, then call `finish()`. The exact count must match. Each
chunk is checked for finite values before writing, retaining float headroom and
precision. CRC32 is accumulated as chunks arrive. Output is first written into
an exclusively reserved `OUTPUT.writing/audio.tmp`; only a complete footer and
closed stream can be published by a no-overwrite hard link. Existing outputs,
dangling symlinks, competing writers and stale staging are not overwritten.
A failed disk write/publication closes the writer to further use; invalid input
before I/O leaves its count unchanged. Destruction cleans only owned staging.
An interrupted process may leave its staging directory; no power-loss/fsync
guarantee is made. The ordinary whole-buffer writer/reader keep their existing
allocation guards. Short files from both writers are byte-identical.

The long-track iteration passed **35/35 normal + 35/35 ASan/UBSan tests**,
including exact two-hour planning, rejection one tick beyond it, common
per-part release times, writer abort/incomplete/competing-output behavior and
old-reader compatibility. An opt-in **45-minute synthetic** file contains
129,600,000 frames and occupies 1,036,800,068 bytes. It is generated and
checksummed in bounded blocks, opened with two independent readers, and tested
at 12 minutes, 30 minutes, near EOF and backwards. Both tracks match the expected
samples/clock with 262,144 bytes of page storage and no buffering after explicit
control-thread prefetch. Normal and ASan/UBSan runs passed. The temporary file
is removed by the test. This is random-access/duration verification, not 45
minutes of uninterrupted wall-clock hardware playback or a real orchestral
render. The existing real piano/cello bundle still produces matching resident
and streamed numerical output.

```sh
build/daw_frozen_stream_writer_tests --long
ASAN_OPTIONS=detect_leaks=0 build-sanitize/daw_frozen_stream_writer_tests --long
```

The producer connection is now implemented by `daw_session_render --stream`
and `--stream-frozen`: [commands, checks and remaining limits](streaming-export.md).
Next work is real long performances and sustained storage validation, alongside
live instrument input and deeper device recovery validation. The initial polled,
paused reconnect is documented [here](output-recovery.md). Short real export and synthetic
duration checks are not a substitute for that endurance test.
