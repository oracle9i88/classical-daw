# Streamed native instrument and frozen export

The native offline producer now connects the two-hour session plan to AU
rendering, raw frozen tracks, PCM16 stems and a PCM16 master without allocating
an entire audio waveform. This completes the initial disk-based producer and
playback connection; live AU input/monitoring is still separate work.

```sh
# Fresh render with installed/licensed Pianoteq and/or SWAM Cello.
build/daw_session_render --stream path/to/session.dawsession out/new-performance

# Reuse the exact captured performances, without loading either plugin.
build/daw_session_render --stream-frozen out/new-performance/session.dawsession out/new-mix

# Reopen with the existing shared playback/mix/history/recovery controls.
build/daw_session_play --stream out/new-mix/session.dawsession
```

Outputs must be new directories. Original inputs are read-only. Existing default
render and `--frozen` commands retain their buffered limits and format. `--check`
still uses that default plan and is not a preflight for long streamed exports.
Both new modes accept up to 345,600,000 frames at stereo 48 kHz (two hours **including
the five-second release tail**), subject to the existing score/MIDI/state bounds.

## Clock, mix and publication

Each part has its own AU instance. That instance continuously renders 256-frame
blocks using absolute sample timestamps and the existing scheduled MIDI events;
it is not recreated at chunk boundaries. Pedal, expression, pitch bend, note-off,
tempo scheduling and the common end time retain the existing behavior. A sink
exception aborts the instance, which cannot be reused. Diagnostics are published
only after the entire render and every sink call succeed. There is no audio
device output during this offline operation.

Each raw chunk is written to DAWFRZ01 before track gain/balance/mute/solo, then
the post-track chunk is written to its PCM16 stem. A second disk pass sums the
raw frozen tracks in route order, applies the same static mix and master gain,
and writes the master in blocks of at most 8192 frames. This avoids summing
already-quantized PCM stems. Frozen reuse retains the cached preset/version and
source binding. There is no format migration or implicit plugin fallback.

The new WAV writer uses exactly the buffered writer's PCM16 conversion for
finite samples within unity. Over-unity stems or masters fail with a gain
diagnostic instead of clipping. Invalid chunks cannot partially enter a WAV.
Each WAV/frozen output is staged in an exclusive `.writing` directory and only
published after exact frame-count completion and successful close, using a
no-overwrite hard link. Existing files, dangling links, competing outputs and
old staging directories are preserved. Unsupported hard links fail explicitly.

Ordinary exceptions remove this export's files. `session.dawsession` is written
last. A killed process can still leave an incomplete directory, and there is no
fsync/power-loss durability or atomic whole-bundle commit. Do not reuse partial
output directories as complete sessions.

Audio buffers are fixed by block size, but score events, source bindings, plugin
internals, C++ file buffers and OS caches still consume memory. This is not a
whole-process constant-memory or measured RSS claim. Before reuse or master
mixing, each frozen file undergoes a full CRC/finite-value scan. This adds startup
I/O and is not continuous integrity checking against concurrent file edits.
Keep input media immutable while a reader is open. Each new bundle duplicates
raw float audio and PCM stems/master; this is not a deduplicating cache.

## Local verification (2026-09-23)

- Normal and ASan/UBSan CTest: **36/36 each**. WAV tests cover byte parity across
  chunk boundaries, endpoints, invalid/overloaded samples, exact lengths,
  abort cleanup, stale staging and no-overwrite publication.
- An opt-in synthetic run wrote the full **345,600,000 frames**, yielding a
  **1,382,400,044-byte** two-hour WAV. RIFF/data lengths and left/right PCM values
  at block boundaries, 90 minutes and the final frame matched. Both normal and
  sanitizer runs passed; the test deletes its temporary file.
- The corrected real piano/cello fixture rendered **1,517,594 aligned frames**
  through `--stream`, without clipping. All **8/8 cello notes** passed the existing
  sustained-energy and harmonic-pitch audit. This is a 31.6-second musical fixture,
  not a two-hour AU performance or a musical-quality verdict.
- Frozen integration runs check complete bundle byte parity against buffered
  exports, including reports, MIDI, AU state, float audio and WAVs. Solo, mute,
  unmute, gain and balance match; stale source/state, corruption and stem/master
  overload fail with cleanup. Existing destinations and source hashes are preserved.
- The local AU probe passed continuous chunk timing, independently accumulated
  sample meters, MIDI-message counts, empty/throwing sinks and rejection of a
  consumed instance. Separate Pianoteq instances were **not** sample-identical:
  two buffered runs differed by up to 0.0000533245, and buffered versus chunked
  differed by up to 0.0000640536. This is consistent with the already documented
  [piano repeatability limit](pianoteq.md); it is not a cross-instance equality
  guarantee. Exact repeatability here refers to reuse of the same frozen audio.

Reproduce on the corrected local duet (generated media and licensed plugins are
not committed):

```sh
ctest --test-dir build --output-on-failure
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-sanitize --output-on-failure
build/daw_wav_stream_writer_tests --long
ASAN_OPTIONS=detect_leaks=0 build-sanitize/daw_wav_stream_writer_tests --long
python3 scripts/check_session_freeze.py out/swam-note-audit-20260923/corrected --stream
build/daw_session_render --stream out/swam-note-audit-20260923/corrected/session.dawsession out/new-stream-validation
python3 scripts/check_session_freeze.py out/new-stream-validation --stream
python3 scripts/check_cello_notes.py out/new-stream-validation/track-2.mid out/new-stream-validation/track-2.wav
build/daw_au_chunk_probe out/swam-note-audit-20260923/corrected/track-1.mid out/swam-note-audit-20260923/corrected/track-1.aupreset
```

The cello audit needs optional `mido`/`numpy`. The AU chunk probe is an explicit
local Pianoteq check for chunk timing/meters, repeatability observations,
empty/throwing sinks, report publication and consumed-instance rejection;
it is excluded from CTest/CI.
No workflow dispatch or retry is required for any check here.

Remaining acceptance work: real long musical AU runs, sustained slow-storage
behavior, plugin process isolation/timeouts and cancellation, and realtime
instrument routing. The existing SWAM initial-versus-restored waveform variation
is not fixed by chunking; frozen reuse preserves the already-captured performance.
