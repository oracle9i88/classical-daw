# Whole-session publication and ACK

2026-09-24. `LiveSessionStream` replaces independent live mailboxes as the
publication layer. The existing `LivePerformanceStream` is now a **one-track
facade over this same core**, so its earlier identity, held-key, controller,
guarded transaction and retirement tests exercise the new implementation too.

There is one owned pair of whole `Plan` objects, one `pending_`, one `retired_`,
one applied revision and one requested/acknowledged ticket sequence. Each plan
contains every immutable track plan. Lane runtime holds voice/key/controller
ledgers, event cursor and preallocated MIDI scratch only. Its position field is
assigned from the common session frame on every quantum, never independently
advanced. It owns no plan, publication pointer, revision or ACK. Per-track block
revision stamps are derived from the one active whole-plan revision.

Control preparation resolves stable string track IDs into fixed runtime slots;
candidate route order can change without moving instruments or MIDI channels.
The topology and each performed-ID universe remain fixed for this transport.
The same performed ID and channel may occur on different tracks. Invalid or
duplicate/missing routes, nonzero track offsets, invalid MIDI or allocation
failure leave the published plan untouched. Every track is prepared before the
only release-store publication. One pending request means backpressure, never
overwriting an unheard revision.

At the engine-quantum boundary, the audio thread claims that single pointer.
It inspects **all tracks' sticky onset guards before any lane reconciliation**.
A guard on the last track rejects the whole candidate, including earlier tracks'
gain, pitch and CC changes. The receipt supplies performed ID plus an immutable
slot that the control thread resolves to the stable track ID without allocating
a string in the callback. If admitted, one active pointer selects all tracks.
Only after all host lane processing has stopped referencing the old plan are
retirement and one ACK published. External AU rendering occurs afterward.

Cancellation wins ownership only before the audio claim. Once claimed, the
control thread must await that request's decision rather than roll back a
possibly accepted edit on timeout. Editor revision reuse after rejection gets
a new request ticket. All allocation/destruction remains on the control thread.
No legacy per-track stream object is wrapped inside the new session stream.

## Evidence

New tests cover reordered stable bindings; duplicate numeric note IDs and MIDI
channels; second-track preparation failure and guard rejection before first-
track repitch/CC; exhaustive fail-at-allocation injection through whole-plan
preparation (20 allocation sites in this fixture); backpressure/cancellation;
exactly one held release per track across swap; future attacks/releases moved
together to samples 800/900; a swap while 960-frame PDC contains earlier audio;
and 1000 accepted plus 100 rejected concurrent requests. Every observed quantum
checks both tracks' revision, frame count, start and expected gain. Callback
new/delete counters remain zero. Original single-track concurrent rejection/
retry and ownership tests also run unchanged through the facade.

Debug and ASan/UBSan suites: **45/45** (leak detection disabled for the latter).
TSan passes both the new session and original single-track concurrency suites.
The added future-note timing case was subsequently rerun in Debug, ASan/UBSan
and TSan. The first allocation test used an unjustified “more than 20” count
assertion; this fixture has exactly 20 sites. The corrected gate exhausts every
failure position until the first successful preparation and checks unchanged
state at each failure, rather than imposing an arbitrary allocation minimum.

[Raw validation receipts and source hashes](research/2026-09-24-live-session-evidence.txt).
These are local CPU tests, not new real-AU callback-budget or listening results.

## Still separate work

This implements the **MIDI-plan atomic-publication subgate**, not all of step 2.
The multitrack document/history migration, production latency-generation/binding
admission, graph/stream stop and quarantine coordination, seek/reset, mixed
frozen/live time origins, full PDC matrix through the combined runtime and the
three-clock receipt adapter remain unimplemented. The real multitrack AU host
must not commit document transactions merely on submitting a candidate; it
must use the common decision as the existing single-track editor does.

The shared stream pads lanes to a common reserved end and does not shorten it
on edits. The eventual graph adapter also needs an explicit end/drain admission
policy: the MIDI stream's stopped EOF and the PDC audio tail are different
positions. This test does not permit silently restarting MIDI behind an already
advanced PDC clock. Two-AU hardware admission and 12-part editing turnaround are
still pending; no concurrency or sample test substitutes for those gates.
