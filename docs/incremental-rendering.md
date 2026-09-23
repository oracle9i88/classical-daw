# Incremental native session rendering

Changing one instrument's notes no longer requires discarding every captured
performance. The explicit incremental mode compares a previous saved session
with the current one, reuses validated unchanged stems and renders changed
parts through their instruments. It writes a new independent output bundle.

## Commands

Keep the previous rendered bundle intact. Make a separate copy for score or
instrument-state edits, then inspect the reuse decisions without loading AUs:

```sh
build/daw_session_render --incremental-check previous/session.dawsession edited/session.dawsession
build/daw_session_render --incremental previous/session.dawsession edited/session.dawsession new-bounce
```

The check prints `REUSE` or `RENDER` and a reason for each stable part ID, plus
counts. It checks routing, state bytes, performance and any cache selected for
reuse; it does not certify plugin availability or restoration for dirty tracks.
The render command loads instruments only for tracks marked `RENDER`. Even a
muted dirty track is rendered so its new frozen audio is ready for later unmute.
The output directory must be new. Both inputs must remain unchanged throughout.

Current routing and saved mix settings control the output. UI route reordering
is matched by part ID, not track index or a filename. New parts render; removed
parts are omitted. A changed instrument, missing saved state, different state
bytes or absent previous frozen reference requires rendering. Missing or corrupt
*referenced candidate* audio causes an error before loading any instrument; it
does not silently fall back to a potentially different plugin performance.
Caches for changed or removed parts are not needed or validated.

The existing `--frozen` / `--stream-frozen` commands remain strict: they still
reject any whole-score identity change. They do not automatically enable this
selective reuse policy. The current session's stale frozen references are ignored
by incremental rendering; only explicitly supplied previous-session candidates
can supply unchanged audio.

## What counts as unchanged

Reuse requires all of:

- Same stable part ID and supported instrument kind.
- Exact saved instrument-state bytes, regardless of state filenames.
- Identical ordered MIDI messages at identical 48 kHz sample positions,
  including attack/release velocity, channels, pedals, expression, pressure,
  pitch bend and same-frame ordering.
- Identical shared release boundary, total frame count and five-second tail.
- Previous cache's complete original score/state identity, CRC, frame count
  and finite sample data pass validation.

The comparison uses the same `makeMidiSampleSequence` as the offline AU host.
Tempo edits affecting event times or the shared end invalidate the corresponding
tracks; changes to global duration normally invalidate all stems. Equal channel
numbers across independent parts do not cause controller inheritance. Bank and
program changes are compared conservatively even though the fixed-preset host
currently ignores them.

Names, lyrics, enharmonic/tie spelling and meter notation can change without
rerendering when the resulting audible event schedule remains identical. Mix
targets also do not invalidate pre-fader audio. Sub-sample timing edits that
round to the same ordered sample events are equivalent under this render
contract. The host currently supplies no tempo/meter transport callback to AUs.
This comparison must be extended if future processing adds host transport,
automation, effects, sample-rate options or other sound-affecting inputs.

Reused float audio is streamed unchanged into a new cache bound to the new
score/state identity. The new cache header/CRC may differ while its audio
payload stays byte-identical. New MIDI files reflect the edited score. WAV
stems and mix use current gain/balance/mute/solo/master settings. Each stem's
`report.json` entry records `audio_source: frozen` or `plugin`; reused plugin
diagnostic counters are not presented as a fresh rendering measurement.

This is explicit offline reuse of captured performances, not a plugin
determinism guarantee or a live unified score-edit transaction. Updating an
installed plugin does not replace unchanged frozen audio: use ordinary
`--stream` rendering if a fresh instrument performance is wanted. No crossfade,
time stretch or partial-track splice is attempted. Streaming export retains its
existing two-hour/64-route limits and output failure cleanup behavior.

## Local verification

```sh
build/daw_session_performance_tests
python3 scripts/check_session_incremental.py out/swam-note-audit-20260923/corrected
ASAN_OPTIONS=detect_leaks=0 python3 scripts/check_session_incremental.py out/swam-note-audit-20260923/corrected --build build-sanitize
# Explicit real Pianoteq rendering; no speaker output:
python3 scripts/check_session_incremental.py out/swam-note-audit-20260923/corrected --render-changed
```

Default CLI checks use temporary copies and load no instruments. They compare
unchanged incremental output against strict frozen output, check notation-only
identity rebinding with exact audio payloads, note/tempo/state invalidation,
reordered routes, corrupt/stale/missing-cache rejection, saved-session reopening
and output protection. Original source hashes must remain unchanged.

The optional real check changes the first piano note and renders only that
instrument. It requires audible changed piano output, byte-identical cello
payload and stem WAV, correct report provenance, successful playback preflight
and exact audio/state/MIDI reproduction via subsequent strict frozen export.

Verified locally on macOS, 2026-09-23: normal and ASan/UBSan suites each passed
40/40; plugin-free incremental checks passed all 17 CLI operations in both
builds. The real Pianoteq variant passed 21 operations with one changed-piano
bounce, zero cello plugin loads, no output device opened and unchanged original
source hashes. The existing frozen-mix checker also passed all 29 operations,
including buffered/streamed byte parity and stale/corrupt/overload cleanup.
This covers the short captured duet, not a long orchestral
incremental-render benchmark or live editing.
