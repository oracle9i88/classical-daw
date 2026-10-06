# Saved multi-part performance editor

2026-10-02; review follow-up verified 2026-10-07. Feature branch, local Alpha. This connects authored multi-part
documents to the existing whole-session runtime; it is not a general orchestra
editor, a signed desktop application or a listening approval.

## Entry and identities

```sh
build/daw_performance_session_import /path/to/session.dawsession new-work
# Explicitly rebuild from the score when the source references frozen audio:
build/daw_performance_session_import /path/to/session.dawsession new-work --unfreeze
build/daw_performance_play new-work
```

Migration uses a saved native score and an explicit saved AU state for every
route. It preserves part IDs, controller messages, effective output gains, balance,
mute/solo and the musical-delay field. No AU or device is opened. Source files
are read-only; destinations must be new. Preset-only routes fail: save the
instrument state first. Frozen references fail without `--unfreeze`; that flag
rebuilds performance from the score and leaves the original cache untouched.
It does not import frozen audio into the live graph or certify that a cached
recording equals a fresh rerender. The receipt fingerprints the source score;
it does not invent a MusicXML repair history for an already-saved native score.

The performance document allows a -60..0 dB master and -60..12 dB routes.
A positive source-session master is moved into **every** route and the saved
master becomes 0 dB, preserving each sum `master + route`. The importer prints
this rebase. If any resulting route exceeds +12 dB, including a muted or
solo-excluded route, import fails at `stage=gain` before saving anything. Keep
that source in the offline/frozen player or explicitly adjust its mix first;
there is no silent attenuation, clipping or raised realtime gain limit.

The compiler supports 1..64 independent parts. Each take has one global
performed-note allocator and a separate notation allocator. Ties can map
multiple notation segments to one performed note, but cannot cross parts.
All parts retain one score end, terminal resets and five-second release tail;
late performance offsets extend the common end. Curves on every part are then
validated against that shared end, so a cello CC11/CC64 point can reach an
extension caused by a late piano note. Step and linear curves may end at the
common boundary; synthetic final pedal releases still win at that boundary.
Curves do not extend the work themselves. Shortening an extension past an
existing curve point rejects the edit without changing document or history.
Routes retain each part's
existing channel bytes, so identical channels in different parts remain isolated
by separate instruments. A curve's part ID is mandatory in a multi-part score.
Curve IDs are globally unique within a take; a lane owns (part, channel, CC).

This saved-session entry is different from `daw_performance_import`: the raw
MusicXML/MIDI importer still selects a single part and applies its reported
audition repairs. There is no automatic multi-instrument MusicXML routing,
new-note editor, MusicXML ID round trip or orchestral ornament expansion here.

## Editing

Use IDs from the listing, rather than copying fixture IDs:

```text
routes
notes-part "cello" 0 20
notes-near 6 8
curve-adopt-part "cello" 1 0 11
curves
route "piano" -6 -0.2 0 0
undo
redo
save "new-work-edited"
```

`edit PERFORMED_ID OFFSET_MS SCALE VELOCITY` edits performance. `pitch
NOTATION_ID STEP ALTER OCTAVE` edits notation. `shape-part PART FROM_SEC TO_SEC
offset_ms|scale|velocity FROM TO` shapes only that part. `curve-step-part` and
`curve-put-part` take `PART ID CHANNEL CC POINT_ID SECONDS VALUE [...]`; adopt
retains imported same-time controller steps. Only CC64 and CC11 lanes are
editable. Replacing a lane explicitly overrides its imported messages.

`route PART GAIN_DB BALANCE MUTE SOLO` retains the existing musical delay.
Route gain accepts -60..12 dB, balance -1..1, and flags 0/1. Master `gain`
remains -60..0 dB. Notation, performance, curve, route mix and master edits use
one bounded 128-command delta history. Part filtering is not another history.
The active take and instrument/topology cannot change during playback.
Scalar route-mix commands validate only the changed gain/balance metadata in
the already-validated editor; they no longer recompile all saved takes.
Live admission still compiles the active whole-session plan, and checkpointing
still validates the complete document. Smooth large-score fader dragging is
not established by this optimization.

## Live path and admission

One `EnsembleAudition` creates one `SessionRenderSource`, one session mailbox,
one applied revision and one PDC graph. It supports at most two live routes:
Pianoteq 9 and SWAM Cello 3, including two instances of the same kind. No per-part
publication, automatic freezing or hidden preset fallback occurs. SWAM needs
initial CC11 and notes within its saved-state/transposition range. Every candidate
is compiled and checked on the control thread before publishing the entire plan.
An already-processed onset, out-of-range SWAM pitch or rejected mailbox leaves
the document, revision and command history unchanged.

`play` opens the device; `--silent` suppresses speaker output while exercising
the same DSP. Live undo/redo uses the same whole-session admission. Track gain,
balance and audibility travel with that plan. The initial mix uses its exact
saved coefficients, without a startup fade. While running, gain/balance edits
begin at the accepted quantum boundary and interpolate the left/right output
coefficients over **480 frames (10 ms at 48 kHz)**, continuing across quanta.
An edit during a ramp starts a fresh 480-frame ramp from the current coefficients,
not the previous target. Muting or solo-excluding a lane gates it immediately;
unmuting fades it in from zero. Quarantine also removes the lane immediately,
including any buffered samples. These are coefficient ramps, not a claim that
every plugin or automation change is click-free. Muted/solo-excluded plugins
and delay lines still advance; L is fixed. Runtime status prints L,
own/compensation latency and each quarantine
state, and is polled during CLI playback. Returned track errors isolate that
lane; a latency generation change stops the whole graph for a stopped rebuild.
Plugin crashes/hangs are not isolated by an in-process adapter.

Musical `track_delay_us` is stored separately; nonzero execution is rejected.
This does not compensate bowed-string attack time. Ensemble seek/range preview
is also explicitly rejected: play begins at zero. The legacy piano document
retains its earlier verified preroll/range path. More than two live routes,
frozen/live mixing and focus switching remain open, so twelve-part editing
turnaround is still unmeasured.

## Storage, recovery and bounce

Explicit routes or part-scoped curves write performance format **v4**. It stores
one `route-N.aupreset` per route in addition to `score.dawproj` and
`performances.dawperformance`. v1/v2/v3 legacy documents remain readable and
retain their original layout if they need no v4 features. Old binaries reject
v4. State limits are 16 MiB per route and 256 MiB total. Score/state/route binding
uses length-delimited FNV-1a for accidental corruption, not authentication.
Plugins must still validate their own state; the data-only importer cannot
certify a saved preset's musical pitch/range or DSP behavior.

Autosave/recovery covers all route states, not just piano. It retains the two
latest complete revisions; the unchanged original is required for recovery.
Undo commands are not serialized and no fsync/power-loss guarantee is added.
Compilation/validation and checkpointing still traverse/copy complete data on
the control thread. Delta history is not incremental score compilation.

```sh
build/daw_performance_render new-work-edited new-bounce
```

The ensemble bounce drives the same realtime-configured AUs/PDC graph manually,
with **no output device**, and removes common L from the file origin. It does
not set the plugins to offline mode. Reports include per-track peaks and
NoteOn/NoteOff counts, mix peak, RMS and clipping. Any quarantined or incomplete
track fails the bounce instead of exporting a partial ensemble silently.
The buffered output limit is 256 MiB; streamed performance bounce and stems
are not integrated. Audio-byte equivalence with realtime or a later SWAM
instance is not promised. Licensed plugin states and audio stay local.
The bounce collector is also used with fake sources: it requests the exact
remaining frames, checks progress/failure/EOF and retains the complete PDC
drain before removing L. Tests cover asymmetric first/last samples with
173/512/700/960-frame delays, as well as early EOF, stalled/overshooting clocks,
latched failure and missing EOF. They test the collector and portable graph,
not plugin DSP or the output device. The production `EnsembleAudition` now
instantiates the portable `BasicEnsembleAudition<RendererFactory>`; its macOS
factory retains AU-specific identity, saved-state and range checks. The same
adapter is tested with fake renderers through the real session mailbox/graph.
`DocumentAudition` similarly uses a tested portable facade to choose legacy
piano or explicit-route playback. The source still dies before its owned
renderers, after output stop/join; the factory seam adds no callback allocation
or separate test scheduler.

## Reproduction and evidence

```sh
ctest --test-dir build --output-on-failure
python3 scripts/check_multipart_performance_cli.py /path/to/session.dawsession
python3 scripts/check_performance_session_gain.py --build build
# Opt-in, Mac + two installed licensed AUs, speakers ALWAYS silenced:
build/daw_performance_session_probe new-work new-evidence --hardware-silent
```

The portable tests cover part-scoped mappings/controllers, shared ends,
chronological undo, rollback, v4 byte-identical save/reopen/recovery and
whole-session publication with route balance/mute/solo. The CLI regression
discovers IDs from the source document and never sends `play`. The real-AU
probe discovers future IDs from the saved document, checks rejected edits,
edits both tracks, adopts expression, changes route gain, undoes/redoes all
four commands during one uninterrupted transport and verifies every attack.
It is a bounded two-route workload, not an arbitrary corpus tester.

[Raw commands, failures and local measurements](research/2026-10-02-multipart-performance-evidence.txt).
At the 2026-10-02 revision, local CTest passed **52/52**, affected ASan/UBSan **6/6**. The saved eight-bar
duet ran 72 piano / 8 cello attacks and releases, with live revision 12 after
four edits, four undos and four redos. Maximum complete-client callback was
4.1505 ms at 557 client frames: **35.7673%** of that callback's budget, with
zero deadline/headroom misses or callback errors. This is one device at
44.1 kHz / 512 frames with 48 kHz client conversion, not a heavy-polyphony or
native-48-kHz benchmark. The device-free bounce wrote 1,517,594 stereo frames
(31.61654 seconds), removed L=960, retained all 80 attacks/releases and had no
clipped samples. An independent PCM16 check confirmed frame count and RMS.
The first attempted old saved document failed its SWAM pitch/range admission;
the original -12-semitone state and failure remain recorded, not silently fixed.

The [2026-10-06 review evidence](research/2026-10-06-multipart-review-evidence.txt)
retains the two pre-fix failures. At that revision local CTest passed **53/53**, affected
ASan/UBSan **7/7**, data-only multi-part/recovery CLI checks passed, and the
device-free real Pianoteq/SWAM bounce again retained 72/8 attacks and releases,
removed L=960 and had no reported clipping. Independent PCM16 frame/RMS checks
passed. No new device callback budget measurement or listening approval was
made. The gain CLI test joins CTest when a Python 3 interpreter is available;
Python is not required by the C++ application.

Review also raised possible legacy single-part length changes. Inspection of
the pre-multi-part compiler and MIDI scheduler shows source note endings and
late controller events already extended the minimum measure end. A regression
now compares all legacy frames/MIDI/reset events against that scheduler path,
including a `duration=0` measure with a note beyond its nominal barline. This
is preserved behavior, not a newly fixed export-length regression. The earlier
instantaneous gain/pan step assertion has been replaced by an independent
480-frame coefficient oracle spanning two quanta, including immediate solo
exclusion and held-note continuity. The long multi-part compiler, route binding
and v4 reader sections were clang-formatted without changing the saved layout.

The [2026-10-07 native follow-up evidence](research/2026-10-07-native-review-followup.txt)
records local Debug CTest **54/54** and affected ASan/UBSan **4/4**. Adapter regressions
cover all-route preflight before construction, releasing the first renderer
when the second fails, the unarmed output gate, silent-mode DSP and clipping
meters, pollable per-track quarantine, changed-latency failure, real mailbox
rejection followed by a legal edit and undo, facade selection and range/capture
rejection. Factory fakes verify production host orchestration; they do not
certify AU internal allocation/locking, preset parsing, DSP, plugin crash/hang
containment or CoreAudio device behavior. This follow-up opens no real plugin
or output device and makes no new callback-budget or listening claim.

Application frame is recorded; audio-change mixer exit and device presentation
remain unmeasured. EOF/plan-application clocks are not substituted for those
two clocks. No GitHub workflow was dispatched or retried; no license change.
