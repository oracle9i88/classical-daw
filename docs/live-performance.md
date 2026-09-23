# Live performance revisions without restarting the instrument

This follows the first [internal performance slice](performance-vertical-slice.md).
The new gate is one AU instance and a continuous sample clock: publish an edit,
apply it at the next engine quantum, keep held-note ownership, and eventually
release every key. Re-exporting a WAV, recreating the AU, restarting playback or
sending a panic message to cover lost NoteOffs does not pass this gate.

## Preconditions and semantics

Plans are compiled and indexed on the control thread. The next-block guarantee
starts at **release publication of a ready plan**, not at keyboard input or the
start of compilation. CoreAudio may split one device callback into multiple
engine quanta of at most 256 frames. `status` distinguishes editor revision from
applied revision and reports the absolute application frame. During playback,
the CLI's existing edit/pitch/curve/gain/undo/redo commands publish revisions
without stopping output. `play` is still an explicit fresh audition.

- A note already sounding cannot have its past attack or NoteOn velocity
  rewritten. For the same performed ID and MIDI key, preserve that attack and
  take its future release time from the new plan. Even when its written new
  onset lies in the future, do not reattack it in the current pass.
- Shortening an active note into the past, or removing it, sends its own NoteOff
  at the exchange boundary. An end exactly on the boundary retains the source
  NoteOff and its release velocity. Repitching an active key releases the old
  key and starts the replacement if its new interval spans the playhead.
- A not-yet-played note moved across the playhead starts at the exchange boundary
  if its interval still spans that boundary. An already played/released attack
  is not resurrected by duration edits, undo or a later onset; explicit replay
  starts a new pass. All edits remain in the saved document for the next pass.
- CC64 and CC11 first chase values strictly before the exchange frame. Source
  events exactly on the boundary then run in their original order, followed by
  catch-up attacks using the resulting current controller state. Thus a source
  NoteOn before a same-frame CC is not reordered by chasing. Repeated identical
  controller values are suppressed.
- Sustain keeps MIDI semantics. Releasing a key under CC64 does not selectively
  silence that key's pedal tail. MIDI 1.0 cannot isolate one such tail without
  affecting other voices. Do not flush the pedal or send CC123 on every swap.
  The final plan releases pedals/keys at its declared terminal boundary.
- Two internally nonoverlapping plans can still collide with an old held key:
  moving that note's onset into the future can put another same-pitch note in
  front of it. A fixed actual-key owner table preserves the existing key and
  skips the conflicting new attack **for this pass only**. Its later NoteOff
  must not terminate the preserved voice. The skipped count is reported on
  stop and in probe results. This is an explicit audible limitation, not a
  claim that both notes were played. The acceptance fixture requires zero skips.
- A run's reserved render end never moves backwards. Shortening/deleting notes
  cannot truncate the tail already reserved for the old planned release. Live
  extensions can increase that end. Probe captures have fixed allocated capacity
  and reject extensions beyond it before publishing.
  Once a pass has fully ended, a same-length/shorter revision is acknowledged
  silently; it cannot manufacture one more render block past the capture end.

This iteration keeps one performance's initial ID universe (up to 4096 attacks).
IDs may disappear/reappear in an engine plan but cannot be added during a run.
The CLI currently provides edits, not insert/delete commands. Switching takes
or instrument state requires stopped playback: equal numeric performed IDs in
different takes are not interchangeable. Source CC120/121/123–127 channel-mode
changes and nonzero sostenuto/hold-2 are refused by this live path. Other source
channel messages remain fixed across live updates; only notes, CC64/CC11 and
gain may change. Offline interchange/rendering retains its wider input support.

## Ownership, transactions and bounds

`LivePerformanceStream` has two control-owned plan slots. A release/acquire
mailbox publishes one immutable plan; a separate retired pointer returns the
old plan to the control thread. There is no callback shared-pointer destruction,
allocation, full-event rescan or mutex. A busy mailbox rejects a new edit rather
than discarding an already accepted revision. The callback maintains actual
fired/down states and a 16×128 key-owner table from messages it dispatches.

The plan supplies note slots and indexed controller lanes. Boundary work is
bounded by 4096 old-note releases + 4096 catch-up attacks + 32 chased controllers
+ 4096 ordinary events per quantum. Initial controller setup adds at most 32
messages. The preallocated 16384-message buffer and AU dispatch limit include
this reconciliation headroom; it is not stolen from the source-event budget.
These structural limits are not a certification that every permitted workload
meets a DSP deadline on every machine.

`WorkEditor` uses a final control-thread admission hook for live publication.
Compilation, validation and mailbox availability all precede acceptance. A
rejected pitch/performance/curve/gain edit or undo/redo restores the document,
active take, history cursor and revision; failed new edits do not discard redo.
Once publication succeeds, recording the preallocated fixed-size command cannot
allocate. There remains one command stack. Gain changes ramp over the first
applied block; MIDI NoteOn velocity is not misrepresented as a live voice-volume
parameter.

## Reproducible checks

No plugin is loaded by these tests:

```sh
ctest --test-dir build -R 'daw_(live_performance|performance|history|project_history)_tests' --output-on-failure
python3 scripts/check_performance_cli.py /path/to/eight-bars
build/daw_au_realtime_probe --check-sequence --polyphonic
build/daw_performance_live_probe --check-fixture /path/to/eight-bars
```

The live model test counts C++ allocations and frees while inside `nextBlock`;
both must remain zero, including when retiring plans. It asserts held-note
identity across exchanges, moved release/end boundaries, pedal/expression chase,
future/past onset rules, exact-boundary release velocity, mailbox backpressure,
invalid-publication atomicity and both forms of actual-key collision. Editor
tests compare complete saved bytes after failed live admission, test undo/redo
rejection, and assert that a stored +100 ms onset remains +100 ms when tempo is
halved. These are architecture-property gates, not screenshot checks.
Two real threads also exchange 1000 revisions, checking every revision exactly
once, continuous frame order, mailbox retirement and zero callback new/delete.
The standalone test passed ThreadSanitizer without a data-race report.

The hardware probes are opt-in and are never registered with CTest/CI:

```sh
build/daw_au_realtime_probe /path/to/piano.aupreset --polyphonic
build/daw_performance_live_probe /path/to/eight-bars /path/to/new-evidence
```

The old 1.20517 ms spike used one key at a time. It is **not an orchestral or
polyphonic performance result**. The new load is ten-key chords, CC64 held for
three seconds then released, and 20 CC11 events per second. It schedules 1230
messages over 30 seconds, with up to ten keys down and twenty distinct
pedal-latched keys. These counts are MIDI key states, not the plugin's internal
DSP voice count. Every measured engine quantum must use **less than 50% of its
own frame-duration budget**, including short quanta; no fixed 256-frame divisor
is used for a shorter block.

The first heavy run failed this headroom gate: no callback errors or deadline
misses, but three blocks exceeded 50% and the worst ratio was 0.723957. Output
splitting produced 45-frame remainders. The host was corrected to balance each
hardware request: e.g. 557 frames become 186/186/185 instead of 256/256/45, with
the same call count and no extra buffering or device-setting change. A new run
of the same load after that code fix used 185–186-frame quanta and passed:
1,442,238 frames, 1230 messages, peak 0.78658, zero callback errors/deadline or
headroom misses, worst render time 1.89254 ms and budget ratio **0.488398**.
This is close to the 0.5 limit and does not establish capacity for more voices.
The failed run is retained as evidence; the gate was not relaxed to pass it.

The live probe keeps one AU/device run and exchanges three revisions while the
cross-bar tied note is still down: move a future onset +120 ms, change CC11,
then extend the tied duration. It audits actual AU-delivered messages and the
captured callback samples. The tie must retain exactly one attack/release, the
future onset must move, CC11 must arrive in the application block, and frames
must remain monotonic. Each exchange's ±20 ms neighborhood must contain no
≥1 ms silent run and every 10 ms RMS window must exceed 1e-6. Zero callback
errors/deadline misses and <50% worst budget use are also required. This detects
silence gaps, not every possible click, dropout or subjective timbral change.

Final local hardware acceptance (2026-09-23) passed with one AU, one output
start/stop, 1,008,000 callback frames and three applied revisions. Application
frames were 173314, 182787 and 192261, each inside its measured publication
window or the following quantum. The tied note retained one attack at 168000
and one release at 220500 (previously 210000); the future onset moved to 245760.
The smallest measured 10 ms window around the swaps had RMS 0.0055294065,
with zero silent frames. Callback errors, deadline misses and suppressed
same-key conflicts were all zero. Worst quantum budget use was **31.1882%**.
Raw failed/passing load measurements and final live receipts are preserved in
[the local evidence record](research/2026-09-23-live-performance-evidence.txt).

Normal and ASan/UBSan full CTest suites passed **42/42**; final boundary-only
test additions and the exact-byte interactive CLI checker passed in both
builds. The app remains an Alpha: this gate covers one piano/take, not live
multi-instrument routing, arbitrary insert/delete editing, or all old tools.
The previous save/reopen hardware gate was rerun after the playback changes:
all document files remained byte-identical, reopened audio relative RMS error
was **0.0536848%**, and maximum sample difference was **0.000169983** (limits:
2% and 0.01 respectively). All three runs had zero callback errors.

## Legacy history debt

The old `ScoreHistory` API now stores immutable shared snapshots. One commit
copies the new score once and copies retained handles, instead of deep-copying
all old scores. With 4096 notes containing 96-byte lyrics, measured allocation
was 885040 bytes with one retained state and 886032 bytes with 64: +992 bytes.
The new test was also run against the previous implementation and rejected it.
Allocation-failure injection exercises commit/reset/undo/redo/snapshot/clear
and recovery-facing behavior without changing callers on failure.

This is a compatibility improvement, not a completed migration to delta
commands. Resident snapshot memory remains O(history × score); clearing a redo
branch may still destroy many old scores. Current production references to
`ScoreHistory` are in project recovery, while session mix commands use their
separate `SessionMixState`; the repository does not support the claim that all
session tools currently commit through `ScoreHistory`. Global command/history
migration still precedes adding a second live instrument.
