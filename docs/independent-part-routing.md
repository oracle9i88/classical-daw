# Independent score parts beyond 16 MIDI channels

## Defect and correction

`planSession` offered up to 64 separate instrument routes but called the standard
score-to-MIDI bridge in its shared-output mode. That mode deliberately rejects a
score with more than 16 parts if any sounding note has no explicit MIDI channel.
Consequently, a newly authored 17-part ensemble could not reach session rendering,
even though each part would have its own AU instance. A regression fixture
reproduced the failure before the correction.

The bridge now has an explicit `ScoreMidiChannelPolicy::IndependentParts`, used
by native session planning. For an unspecified note channel (`-1`), the derived
channel is `score_part_index % 16`; the index is zero-based in the score, not in
the session route list. This keeps the previous defaults for parts 1–16 and
uses channel 0 again for part 17. All explicit note channels, controller bytes,
event order and release velocities are retained. Planning does not mutate the
score or write these derived channels into the saved project.

The resulting MIDI model is only an intermediate: the session planner splits
it into one track per instrument and routes by stable part ID. It must never be
sent unsplit to a shared MIDI destination. Same-channel controllers then affect
only their own instrument instance. A part's imported multichannel content stays
multichannel; this change does not infer controller channels, split MIDI Type 0
by instrument, set plugin receive channels, implement MIDI ports or assign GM
percussion. SWAM expression validation remains local to each routed instance.

`scoreToMidiFile` defaults to `SharedOutput`, so ordinary SMF export and the mono
diagnostic score renderer keep their existing behavior. Larger imported scores
with explicit channels still export under the previous rules; a large authored
score with unspecified channels still fails shared-output export rather than
silently merging instrument/controller states. Independent planning is capped
at 64 parts, and a failed/unknown policy leaves the caller's MIDI object unchanged.

Existing valid sessions retain their timing/channels and frozen source identity.
No project/session/file format or cache migration is required. Changing score
part order can change the derived channel of an unspecified note, just as it
could within the first 16 parts before this fix. Set explicit note channels
when that routing must stay invariant under score-part reordering; changing only
the session route order does not change it.

## Evidence — 2026-09-23

- Normal and ASan/UBSan suites: **38/38 each**.
- Fixtures with **16, 17, 32 and 64 parts**, reversed session route order, repeated
  channels and one explicit channel override preserve part identity, tied-note
  duration/release velocity, three step tempos, meter change and terminal silence.
- Native project/session save and reopen plus every per-part MIDI write/read
  retain the expected sample-event sequence and common end. The authored 64-part
  MusicXML notation subset also round-trips. That XML test deliberately excludes
  performance-only MIDI metadata, which the current XML bridge does not preserve.
- A diagnostic sine check makes part 1 silent with CC7=0 while part 17, also on
  channel 0, remains audible in its own route. A SWAM route missing initial CC11
  still fails even when another part initializes CC11 on that same channel.
  A 64-part all-silent arrangement keeps its notated length; 65 routes are rejected.
- The native `daw_session_render --check` accepted the generated 64-part session:
  **480,000 frames at 48 kHz**, including its five-second tail, without loading
  plugins. Existing CLI checks passed, and the corrected real piano/cello frozen
  export passed all 29 integration operations with byte-identical buffered versus
  streamed bundles. Original source hashes stayed unchanged.

Reproduce locally:

```sh
cmake -S . -B build
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-sanitize --output-on-failure
build/daw_session_routing_tests --fixture out/new-64-part-routing
build/daw_session_render --check out/new-64-part-routing/session.dawsession
python3 scripts/check_session_cli.py
python3 scripts/check_session_freeze.py out/swam-note-audit-20260923/corrected --stream
```

The optional fixture directory must be new. It contains a synthetic engineering
score, session and isolated MIDI files, not an orchestral recording. The host
still supports only the validated Pianoteq and SWAM Cello adapters; this work
does not establish 64 simultaneous live plugins, a full orchestral library or
64-plugin resource/latency performance. Existing state, event, duration and
memory budgets still apply.
