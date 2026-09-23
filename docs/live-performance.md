# Live performance revisions without restarting the instrument

This follows the first [internal performance slice](performance-vertical-slice.md).
The new gate is one AU instance and a continuous sample clock: publish an edit,
apply it at the next engine quantum, keep held-note ownership, and eventually
release every key. Re-exporting a WAV, recreating the AU, restarting playback or
sending a panic message to cover lost NoteOffs does not pass this gate.

## Preconditions and semantics

Plans are compiled and indexed on the control thread. The next-block guarantee
starts at **release publication of a ready plan**, not at keyboard input or the
start of compilation. CoreAudio may split one client render callback into multiple
engine quanta of at most 256 frames. `status` distinguishes editor revision from
applied revision and reports the absolute application frame. During playback,
the CLI's existing edit/pitch/curve/gain/undo/redo commands publish revisions
without stopping output. `play` is still an explicit fresh audition.

- Product policy: changing the onset of a performed note whose attack has
  already been processed in this pass rejects the **whole edit**, including any
  duration/velocity changes bundled with it. This covers attacks actually sent,
  including notes since released, and attacks consumed by same-key conflict
  suppression. A suppressed attack did not sound; it is conservatively locked
  because the stream will not retry it during this pass. The user receives an
  explicit reason;
  no part of the rejected edit is saved. Stop playback before changing that
  onset, or edit a note that has not yet attacked. Undo/redo follow the same
  admission rule and keep their cursor on rejection.
- A note already sounding cannot have its past attack or NoteOn velocity
  rewritten. For the same performed ID and MIDI key, preserve that attack and
  take its future release time from an accepted plan. The lower-level stream
  also preserves an attack when a diagnostic plan moves it into the future;
  this defensive behavior is not permission for the product to accept that edit.
- Shortening an active note into the past, or removing it, sends its own NoteOff
  at the exchange boundary. An end exactly on the boundary retains the source
  NoteOff and its release velocity. Repitching an active key releases the old
  key and starts the replacement if its new interval spans the playhead.
- A not-yet-played note moved across the playhead starts at the exchange boundary
  if its interval still spans that boundary. An already played/released attack
  is not resurrected by duration edits or undo; explicit replay starts a new
  pass. Accepted edits remain in the saved document for the next pass.
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
  claim that both notes were played. The ordinary live-edit fixture requires
  zero skips. A separate forced-plan diagnostic exercises this fallback through
  the lower-level stream; it does not bypass the public editor's onset policy.
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
Compilation, validation and mailbox availability precede publication; publication
alone is not transaction acceptance. The candidate identifies changed onsets.
At the exchange boundary the audio thread checks a sticky processed-onset
ledger before changing the active plan. A sent or conflict-suppressed NoteOn
locks that performed ID until a fresh playback run; repitching and release
cannot clear the lock. This is separate from transient voice/retrigger state.
If a target is locked, it rejects the complete candidate without changing
voices or controller state. Checking
only on the control thread would race an attack during compilation/publication.

The control thread waits for the applied/rejected acknowledgement before
accepting the command; the audio thread never waits. Rejection restores the
document, active take, history cursor and revision; failed new edits do not
discard redo. A timeout cannot simply roll back a potentially applied plan:
it must first cancel a still-unclaimed request, or wait for the audio thread's
decision if that thread has already claimed it. Request identity must distinguish
attempts even when a rejected edit leaves the editor revision unchanged.
Once acknowledgement succeeds, recording the preallocated fixed-size command
cannot allocate. There remains one command stack. Gain changes ramp over the
first applied block; MIDI NoteOn velocity is not misrepresented as a live
voice-volume parameter.

## Reproducible checks

No plugin is loaded by these tests:

```sh
ctest --test-dir build -R 'daw_(live_performance|performance|history|project_history)_tests' --output-on-failure
python3 scripts/check_performance_cli.py /path/to/eight-bars
build/daw_au_realtime_probe --check-sequence --polyphonic
build/daw_performance_live_probe --check-fixture /path/to/eight-bars
build/daw_performance_conflict_probe --check-sequence
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
build/daw_performance_conflict_probe /path/to/eight-bars /path/to/new-conflict-evidence --audible
```

The old 1.20517 ms spike used one key at a time. It is **not an orchestral or
polyphonic performance result**. The new load is ten-key chords, CC64 held for
three seconds then released, and 20 CC11 events per second. It schedules 1230
messages over 30 seconds, with up to ten keys down and twenty distinct
pedal-latched keys. These counts are MIDI key states, not the plugin's internal
DSP voice count.

### Corrected load gate: the complete client callback

Measure wall time from entry to exit of `CoreAudioOutput::renderCallback`,
including all engine subblocks and the host work between them. Require
**elapsed seconds / (client callback frames / client sample rate) < 0.5**
for every measured callback. Subblock timings remain diagnostics, not the
acceptance denominator. Record the client and device sample rates, device
buffer size, and the frame count and elapsed time associated with each maximum;
the largest duration and largest ratio need not describe the same callback.

This callback supplies the input of Apple's DefaultOutput AU, currently at
48 kHz. Its frame count is in that client format. The measured interval does
not include sample-rate conversion or driver work performed outside our
callback, other clients, or the entire HAL IO cycle. It is a **client callback
headroom gate**, not proof that the hardware never underruns. `callback_errors`
counts the host's invalid buffer/capacity cases; it is not an OS xrun counter.
Likewise, a duration exceeding this budget is a measured budget overrun, not
an independent device-underrun notification.

### Historical subblock measurements, not the corrected gate

The first heavy run failed the former 50% **subblock** proxy: its reported
`callback_errors` and subblock `deadline_misses` were zero, but three subblocks
exceeded 50%, with a maximum ratio of 0.723957. Splitting produced 45-frame
remainders. The host now balances each client request: e.g. 557 frames become
186/186/185 instead of 256/256/45, keeping the same number of AU calls without
extra buffering or a device-setting change. Retain this balancing change.

The subsequent run used 185–186-frame subblocks and passed that old proxy:
1,442,238 frames, 1230 messages, peak 0.78658, maximum subblock duration
1.89254 ms and maximum subblock ratio **0.488398**. The earlier duration maximum
was 2.07933 ms: these two observed maxima differ by about 9%. Separate runs
without paired timing samples do not establish a causal 9% speedup, unchanged
total DSP work, or a 33% reduction in workload. Their complete client callback
durations were not measured and cannot be reconstructed from these maxima.

The short remainder makes a fixed per-call cost more prominent in the old
ratio, but the receipts do not pair each maximum with its block size. They
also do not record device rate/buffer size. A 44.1 kHz / 512-frame device is
consistent with roughly 557 client frames at 48 kHz, not established by those
receipts. Neither old run proves the absence of OS xruns. The original failed
and passing outputs remain unmodified evidence, including their historical
field names; neither is a result for the corrected gate above.

The live probe keeps one AU/device run and exchanges three revisions while the
cross-bar tied note is still down: move a future onset +120 ms, change CC11,
then extend the tied duration. It audits actual AU-delivered messages and the
captured callback samples. The tie must retain exactly one attack/release, the
future onset must move, CC11 must arrive in the application block, and frames
must remain monotonic. Each exchange's ±20 ms neighborhood must contain no
≥1 ms silent run and every 10 ms RMS window must exceed 1e-6. Zero callback
errors, no measured complete-client-callback budget overruns and <50% worst
complete-client-callback budget use are required by the corrected gate. This detects
silence gaps, not every possible click, dropout or subjective timbral change.

The earlier local live-edit acceptance (2026-09-23, at `956dd13`) passed with one AU, one output
start/stop, 1,008,000 callback frames and three applied revisions. Application
frames were 173314, 182787 and 192261, each inside its measured publication
window or the following quantum. The tied note retained one attack at 168000
and one release at 220500 (previously 210000); the future onset moved to 245760.
The smallest measured 10 ms window around the swaps had RMS 0.0055294065,
with zero silent frames. Callback errors, deadline misses and suppressed
same-key conflicts were all zero. Worst quantum budget use was **31.1882%**;
this is the old subblock metric, not a complete-client-callback measurement.
Its zero suppressed-conflict count means this run did not exercise the
same-key fallback on hardware. A separate forced-conflict recording and an
auditory check are needed for that behavior; these old results cannot stand
in for either.
Raw failed/passing load measurements and final live receipts are preserved in
[the local evidence record](research/2026-09-23-live-performance-evidence.txt).

### Corrected callback and forced-collision acceptance, 2026-09-23

The new [unaltered run outputs and artifact hashes](research/2026-09-23-client-callback-evidence.txt)
record one polyphonic run, one forced-conflict A/B pair, and one live-edit run.
All use the local Debug build and licensed Pianoteq, without sanitizers during
measurement. The actual device was read as **44.1 kHz / 512 frames**, with a
48 kHz client. No device settings or system volume were changed.

| Workload | Worst complete client callback | Corresponding frames | Budget use |
| --- | ---: | ---: | ---: |
| 30 s / ten-note chords / CC64 / CC11 | 4.001958 ms | 557 | 34.4873% |
| Conflict reference, 7 s | 2.080209 ms | 557 | 17.9264% |
| Forced conflict, 7 s | 2.057250 ms | 557 | 17.7285% |
| 21 s / three live edits plus onset rejection | 2.519792 ms | 557 | 21.7145% |

Every run had zero invalid budget samples, zero measured ratios >=50% or >=100%,
and zero host callback errors. These observations pass the corrected client
callback gate on this machine; they do not certify arbitrary polyphony or the
unmeasured remainder of the HAL cycle.

The collision probe deliberately bypasses product admission and directly
publishes a conflicting engine plan. It retains the old C4 until 3 s, suppresses
the new ID 2 attack at 0.8 s, ignores that ID's release at 1.2 s, and plays an
independent C4 at 4 s. The injected run reports **one suppressed conflict**;
actual ID 1 and ID 3 each have one NoteOn/NoteOff, ID 2 has neither. Its delivered
MIDI and captured waveform equal the reference byte for byte in this run
(the automated audio gate allows 2% relative RMS error / 0.01 FS maximum delta).
Both conflict boundaries have zero silent frames. The WAV pair was actually
sent to speaker output. **Human auditory confirmation remains pending**;
automated equality is not a substituted human listening sign-off.

The live probe also attempts a sub-sample onset edit on the sounding tied note.
The actual player returns a visible rejection without accepting a document or
history revision. It then accepts the original three edits without restarting
the AU. Publication frames and application frames respectively are equal at
173871, 182787 and 192261; the tie still has exactly one attack/release. The
probe now records the narrow window immediately around the atomic publication,
separately from the larger submit call interval: waiting for an ACK must not
make the next-block gate vacuously pass.

The guarded protocol has separate request tickets and editor revisions, safe
unclaimed cancellation, and a sticky processed-onset ledger. Tests cover
repitch/release and suppressed attacks not clearing that lock, sub-sample
stored offsets, byte-exact rejection rollback, retained redo, and 300 concurrent
reject/retry transactions in addition to the 1000 accepted-plan exchanges.
Normal and ASan/UBSan full suites passed **43/43** each. After adding the
narrow publication-window receipts, the three affected tests passed again in
both builds; the two-thread stream test also passed ThreadSanitizer. The final
normal build and normal/sanitized exact-byte CLI checks passed. No GitHub
workflow was run or retried.

The earlier normal and ASan/UBSan full CTest suites passed **42/42**; final boundary-only
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
