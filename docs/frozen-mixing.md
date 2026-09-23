# Saved mix settings and exact frozen-audio reuse

Offline sessions now retain mute, solo and pre-fader float audio. After one
instrument bounce, static mixing changes can reuse that same performance
without instantiating Pianoteq or SWAM. This avoids the observed SWAM
initial-versus-reopened waveform difference when only the mix needs changing.
It does not solve that plugin rerendering difference itself.

## Use the current command-line slice

Create a new bundle with the current renderer; older PCM16-only bundles do not
contain the pre-fader audio needed for this feature:

```sh
build/daw_session_render my-score/duet.dawsession out/full
build/daw_session_edit --show out/full/session.dawsession

# Save settings beside the original score/state/audio. The original is untouched.
build/daw_session_edit out/full/session.dawsession out/full/piano-only.dawsession \
  --solo piano 1 --mute cello 1
build/daw_session_render --frozen out/full/piano-only.dawsession out/piano-only

# Restore the full ensemble using the same frozen performances.
build/daw_session_edit out/piano-only/session.dawsession out/piano-only/restored.dawsession \
  --solo piano 0 --mute cello 0
build/daw_session_render --frozen out/piano-only/restored.dawsession out/restored

# Other supported static edits:
build/daw_session_edit out/full/session.dawsession out/full/balanced.dawsession \
  --gain piano -8 --balance piano -0.3 --gain cello -5 --master -4
build/daw_session_render --frozen out/full/balanced.dawsession out/balanced
```

Use actual stable part IDs shown by `--show`. Boolean options accept only 0/1;
gains accept -60..+12 dB and balance accepts -1..+1. Unknown IDs, invalid values
and existing destinations fail without overwriting the original. Edited session
files must remain in the same directory so their sibling references stay valid.
The editor saves a new settings file; an in-memory undoable mix-command stack
and atomic replacement of an existing mix file are not yet implemented.

## Mute/solo contract

- Without any solo: all non-muted tracks enter the mix.
- With one or more solos: only non-muted solo tracks enter the mix.
- Mute wins over solo. A muted solo still selects solo mode; if it is the only
  solo, the master is silent until another track is soloed or solo is cleared.
- Exported PCM16 stems reflect mute/solo, so they still reconstruct the master.
- Frozen audio is captured **before** gain, balance and mute/solo. Muting never
  destroys the performance; unmuting can reuse its original samples.

A normal plugin bounce still renders muted tracks to capture their original
audio. `--frozen` never instantiates an AU, including for audible tracks. It
does not silently fall back to plugin rendering when a cache is unavailable.

## Persistence and validation

Session v2 adds `mute solo frozen_file` after each v1 route's fields:

```text
route "piano" "pianoteq" -4.5 -0.2 "" "track-1.aupreset" 0 0 "track-1.dawfreeze"
```

The reader still accepts v1, defaulting mute/solo to false with no frozen audio.
Every new bounce writes `.dawfreeze` files and saved-state references. The bundle
report identifies `audio_source` as `plugin` or `frozen`, records audible/mute/solo
state, and reports the original cached plugin version on reuse. No MIDI is sent
in frozen mode; plugin overload diagnostics are null because no plugin ran.
`--check` remains a lightweight routing/source-file preflight and does not verify
frozen audio contents. `--show` reports the presence of references, not their validity.
Saved SWAM state now additionally receives a note-range/transposition check,
even in frozen mode. Historical caches with the missing-low-note fault must be
regenerated; see [the correction](swam-note-coverage.md).

Each frozen file contains stereo 48 kHz IEEE float32 samples, preset/version
metadata, and an exact length-prefixed binding to the source score bytes, stable
part ID, instrument ID and saved AU state bytes. Mixing settings and filenames
are excluded, so they can change without rerendering. Altering any score-file
byte conservatively invalidates all affected reuse checks, even a purely textual
change; per-part musical-change detection remains future work.

CRC32 detects accidental file corruption. It is not a signature or protection
against a malicious author who rewrites the file and checksum. Reuse also checks
file format, exact source identity, sample rate, channel count, frame count,
finite samples, truncation and trailing data. Source files must be regular files,
not symlinks. Missing, stale or corrupt audio fails explicitly and the renderer
cleans its own partial output. Ordinary headroom failures have the same cleanup.
Crash isolation/atomic directory transactions remain outside this slice.

The internal `DAWFRZ01` format is not a WAV replacement for other DAWs; use the
exported WAV stems for interchange. It retains values above unity before faders
without clipping. Audio is limited to 32 Mi frames per track; score input is
bounded to 64 MiB, AU state to 16 MiB per track / 64 MiB total, and frozen identity
to 81 MiB. Each file stores its own source binding, so large scores/states add
disk and temporary memory overhead. Reuse currently reads one complete track,
mixes it and writes a self-contained new bundle; this is not streaming playback
or a disk-deduplicating cache.

Plugin binaries and licensing are not needed to reuse existing frozen audio.
The current remix executable is built on macOS and still links Apple frameworks;
this does not yet expose frozen mixing in the browser or a graphical macOS mixer.
Frozen files embed score and AU-state data and remain ignored by Git alongside
generated audio. They are local project artifacts, not redistributable plugin assets.

## Verification

```sh
ctest --test-dir build --output-on-failure
ctest --test-dir build-sanitize --output-on-failure
python3 scripts/check_session_cli.py
python3 scripts/check_session_freeze.py out/full
```

The final checker expects the original `daw_session_fixture` piano/cello duet.
It copies the input into a temporary directory, never modifies the supplied
bundle, and never invokes normal plugin rendering. It independently checks the
binary CRC with Python's zlib, verifies bit-exact unchanged remix and unmute,
tests solo/silence/gain/balance, and exercises invalid settings, stale sources,
corruption, overload cleanup and existing-file preservation. Unit tests also
cover v1 migration, multiple solos, float values above unity and strict file bounds.

Local results on 2026-09-23: all **28** CTest cases passed in both normal and
AddressSanitizer/UndefinedBehaviorSanitizer builds; the eight plugin-free session
preflight checks passed. The frozen integration checker completed **21** CLI
operations against the actual 1,517,594-frame piano/cello bundle with no AU loads.
Unchanged WAVs/frozen files and restored full mixes matched byte for byte;
stale source/state, corrupt audio and master overload were rejected with no
partial output left behind.

The bounce/editor above provide saved **offline mixing**. A separate native
[frozen-track player](session-playback.md) now exposes realtime transport, shared
CLI mix controls and new-session saving. Live instrument-plugin audition, graphical editing
and audio-device recovery remain future integration work.
