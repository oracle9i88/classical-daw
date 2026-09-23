# Collecting a saved frozen session

`daw_session_collect` creates a self-contained directory from a saved session.
It preserves the exact score, saved instrument states and pre-fader float audio,
plus route order, part IDs and saved gain/balance/mute/solo/master targets. The
directory can be renamed or moved without retaining the original source path.

## Workflow

First save live mix changes using `save NEW_FILENAME` in the native player.
Then run collection in a separate terminal against that saved file:

```sh
build/daw_session_collect /path/to/source/my-mix.dawsession /path/to/new-project
build/daw_session_collect --check /path/to/new-project/session.dawsession
build/daw_session_play --stream /path/to/new-project/session.dawsession
```

The destination must not exist. Its parent directory must already exist. All
routes need a saved state and matching frozen audio; render an unfrozen session
first. Keep source files unchanged during collection and checking. Playback may
read them, but do not run concurrent source edits or external file replacements.
Collection takes the on-disk snapshot, not unsaved changes in a running player.

The result contains `session.dawsession`, `score.dawproj` and numbered per-route
`.aupreset`/`.dawfreeze` files. Shared source states become independent copies.
Files are copied rather than hard-linked to their originals. Existing WAV/MIDI
exports, recovery directories and undo history are not included. Old WAV mixes
could represent different settings; export a new mix from the collected session:

```sh
build/daw_session_render --stream-frozen /path/to/new-project/session.dawsession /path/to/new-bounce
```

Collection, `--check`, offline playback checks and `--stream-frozen` do not load
plugins or open audio hardware. Normal playback opens hardware. Captured audio
can be reused without installed instruments; changing notes and rerendering
still requires compatible, licensed plugins and any assets they reference.
Opaque plugin states are copied exactly; their internal external dependencies
are not discovered or collected. This does not package the plugins themselves.

## Validation and publication

The engine validates session/score syntax, per-part routing, duration, saved
state binding, every frozen-file checksum, frame count and finite sample data.
All tracks are checked, including muted tracks. Audio copy/validation uses
bounded buffers; score/MIDI planning still has its existing bounded allocations.
Limits are 64 routes, 48 kHz stereo, two hours including five seconds of render
tail, a 1 MiB session, 64 MiB score, 16 MiB per state and 64 MiB total states.
`referenced_bytes` includes score, session, states and frozen media; shared
references are counted per route before collection.

The command exclusively creates `NEW_DIRECTORY.collecting`, copies dependencies,
then validates those staged bytes before publishing with a no-overwrite directory
rename. Existing destinations, dangling links and earlier staging directories
are preserved. Ordinary errors clean only files created by this attempt. A crash
may leave `.collecting`; retain or inspect it, then use a different destination
or explicitly remove an unwanted leftover. It is not a published project.

Exclusive publication uses macOS `renamex_np` or Linux `renameat2`. Unsupported
platforms/filesystems fail explicitly; there is no overwrite fallback. Local
verification currently covers macOS only. No fsync/power-loss durability,
malicious concurrent filesystem-tampering protection, plugin compatibility
certification or full live-document transaction is claimed. `--check` checks the
saved dependency graph and audio, not musical correctness or plugin rerendering.
The Mac player separately retains its SWAM saved-state pitch-range check.

## Reproduce local verification

```sh
build/daw_session_bundle_tests
python3 scripts/check_session_collection.py out/swam-note-audit-20260923/corrected
ASAN_OPTIONS=detect_leaks=0 python3 scripts/check_session_collection.py out/swam-note-audit-20260923/corrected --build build-sanitize
```

The unit fixture covers shared states, conflicting source/target role filenames,
reversed routes, exact opaque bytes and raw samples, relocation, stale/corrupt/
missing/symlink dependencies, incomplete routes, owned-staging cleanup, existing
destination protection and competing collectors.

The opt-in real-duet checker works in temporary copies. It saves a changed cello
solo mix, collects it, hides the original directory and renames the collected
directory using a Chinese name. It compares saved targets and offline playback
metrics, then checks every exported file byte-for-byte against the pre-move
reference. Original source hashes must remain unchanged. No AU is instantiated,
no device is opened, and no network or workflow is used.

Verified locally on macOS, 2026-09-23: normal and ASan/UBSan CTest each passed
39/39; the real-duet checker passed all 10 CLI operations in both builds. The
moved session retained 1,517,594 frames, zero clipped samples, and identical
playback metrics and complete export bytes. This verifies the short captured
duet, not long orchestral storage throughput or fresh plugin rendering.
