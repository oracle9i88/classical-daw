# Multi-instrument realtime: implementation decisions and gates

Date: 2026-09-23. Baseline inspected: `03676e2`.
**Status: implementation started 2026-09-24.** The standalone SWAM latency
experiment and portable PDC kernel are implemented; see
[implementation evidence and remaining gates](multi-instrument-progress.md).
The complete session stream, AU integration and four end-to-end gates below
are **not passed**. A tested kernel is not a delivered multi-AU player.
The preceding [single-instrument gate](live-performance.md) remains the evidence
for the running implementation. Its human listening sign-off remains separate.

## 1. Order and the atomic unit

Latency compensation changes the render graph, buffer ownership and clocks.
A registry adds discoverability to that structure later. Therefore implement
the graph/PDC contract before a general instrument manifest. Extract only the
small renderer interface needed to test that contract first.

The current code is narrower: `PerformanceAudition` owns one AU, one stream,
gain, capture and audit; `prepareRealtime()` only permits Pianoteq; the session
route format is now v3 with a reserved user track delay (nonzero execution is
rejected). An `InstrumentDescriptor` already
exists. `InstrumentKind` is not yet confined to a factory: AU state/range/preset
handling and session-render selection still branch on it. Confining it to the
platform adapter/creation boundary is a target, not an accomplished migration.

**One session stream, one pending whole-plan pointer, one applied revision.**
There must not be N independently published `LivePerformanceStream` objects.
Per-track event cursors and voice ledgers are subordinate runtime state, not
independent mailboxes or transport clocks.

```text
One document / command history
  -> prepare complete SessionRenderPlan on control thread
  -> one two-slot pending/retired mailbox
  -> validate every track's admission at one engine-quantum boundary
  -> accept all tracks, or reject the whole candidate before emitting MIDI
  -> per-track renderer -> compensation delay -> track mix -> master
```

- The control thread prepares all tracks, bindings and bounded buffers before
  the single release publication. Preparation may fail without publication.
- At adoption, first inspect all onset locks, binding/latency generations and
  capacities without changing a track. Only then reconcile all tracks. A late
  rejection must not follow NoteOff/CC emission to an earlier track.
- One ACK and request ticket govern the document transaction. Reject/cancel
  preserves every track, document revision, undo/redo cursor and pending voices.
  Reuse the existing safe cancellation rule: a claimed request needs its ACK.
- All tracks in one engine quantum see one revision. A 557-frame client callback
  currently has several <=256-frame quanta; adoption may occur between these
  quanta, but cannot divide tracks within a quantum.
- Address voices by `(stable TrackId, PerformedNoteId)`; do not key by MIDI
  channel, route vector index or performed ID alone. Preserve notation identity
  and independent per-instance controller/key ownership.
- Prepared AU instances and delay-line memory survive ordinary plan exchanges.
  Instrument/topology/PDC-layout changes require stopped reconstruction in v1.
  The callback never allocates, destroys an AU/plan, performs I/O, or waits.
- Extend the single document/history to track bindings and per-track performance
  selection. The old `piano_state` document needs an explicit version migration
  into a one-track binding; do not graft separate `SessionMixState` histories
  onto each track. Legacy `ScoreHistory` migration remains visible debt.

Acceptance: force the second track's validation/allocation/onset guard to fail;
the first track must retain its old revision, MIDI and controls. Stress one
publication while the audio consumer advances; audit every track's revision
per quantum, including edits/undo/redo and same numeric note IDs on both tracks.

## 2. Three distinct timing inputs

| Value | Owner and storage | Meaning |
| --- | --- | --- |
| `onset_seconds` | Existing saved per-note performance data | Authored expressive displacement from the score |
| `track_delay_us` | Proposed signed `int64` route/session field, default 0 | User musical advance/delay of the whole track |
| `algorithmic_latency_frames` / `pdc_delay_frames` | Prepared runtime graph, measured/recomputed | Plugin processing delay and host alignment delay |

Reserve `track_delay_us` in the next route/session schema, with an explicit
version increment and old v1/v2 default of zero. Store it independently of AU
state and expose it separately from the read-only PDC display. Include it in
command history, recovery, collection and frozen-input identity. This document
reserves the execution contract; the v3 struct/serializer now retain the field.
It has no editing command yet. Legacy mix commands preserve it as data.
Until all executing paths implement it, a nonzero value must produce an explicit
unsupported-playback/export error, never be accepted and silently ignored.

Positive user delay means later; negative means earlier. It remains an absolute
elapsed-time value across tempo edits. Shift the track's notes and associated
controllers consistently, preserving within-track order; do not change only
NoteOn and leave its pedal/expression behind. Negative displacement requires
pre-roll/lookahead at song start and seek; never clamp early events to zero.
The first graph gate may require zero user delay while retaining this schema.
Changing track delay during playback is stopped-only initially.

For parallel routes directly feeding the master, at one prepared sample rate:

```text
L = max(algorithmic_latency_frames[i])
pdc_delay_frames[i] = L - algorithmic_latency_frames[i]
```

Delay each faster route's audio, without rewriting its MIDI to impersonate PDC.
Keep L fixed through mute/solo or a failed lane so healthy tracks do not jump.
General sends/buses and feedback graphs are outside this first parallel-graph
gate; future path latency is accumulated along the path, not just one AU.

The AU reports latency in seconds; retain that raw report and the prepared
sample rate beside the resolved sample count. V1 must declare its integer-frame
precision/bounds and refuse unsupported fractional/out-of-range values rather
than advertise untested fractional-sample alignment. Delays and scratch buffers
are sized before playback. Startup warmup (including SWAM readiness) and a
bowed instrument's expressive attack envelope are not algorithmic latency.
Apple defines the AU latency property as processing time from input to output.
[Apple latency definition](https://developer.apple.com/documentation/audiotoolbox/kaudiounitproperty_latency)

Expose `L` in frames/seconds and the stable IDs of all routes attaining the
maximum. This is a user-actionable algorithmic-delay report, not total acoustic
latency. The portable kernel provides it through `statusText()`/`latencyInfo()`.

Acceptance: changing a user/per-note offset must not rewrite reported latency
or compensation; changing AU latency must not rewrite musical offsets. Roundtrip
signed route fields exactly, default old sessions to zero, reject malformed
values and unsupported nonzero execution, and preserve offsets across tempo
changes. Do not claim a universal measured SWAM attack-time constant.

## 3. Nonzero latency must be tested without plugins

Use an instrument test double that actually delays a known impulse/sample
sequence by its declared latency. A reported number with undelayed output is
not an adequate fixture. Check each compensated lane before summing, plus the
mix, against an independently built expected timeline.

| Route latencies, frames | Required compensation | Failure exposed |
| --- | --- | --- |
| 0, 0 | 0, 0 | Transparent baseline |
| 0, 128 | 128, 0 | Basic delay |
| 0, 173 | 173, 0 | Non-divisible circular-buffer indexing |
| 0, 700 | 700, 0 | Delay spanning multiple client callbacks |
| 173, 512 | 339, 0 | Both routes nonzero; general max-minus-own rule |
| 173, 512, 700 | 527, 188, 0 | More than two compensation paths |

Exercise callback requests 1, 185, 186, 256, 512, 557 and 558, with changing
sequences of request sizes and the actual <=256-frame subdivision path.
Put impulses/events immediately before, on and after boundaries; wrap delay
buffers repeatedly; end on partial blocks and drain the complete compensated
tail. Require exact sample positions and no dropped/duplicated samples.
Swap a musical plan with nonempty delay lines: the old buffered audio must
remain in order, without resetting or shortening the lines. Audit zero host
callback allocations/frees and bounded work; run the concurrent mailbox tests.

These are required even if both installed AUs happen to report zero latency.
Synthetic variable-slice tests do not substitute for a future native-48k device
measurement; current hardware evidence covers 44.1k/512 -> 48k conversion only.

## 4. Application, mixer output and device presentation

Record three clocks with explicit units and one transport/device epoch:

1. `plan_applied_frame`: exact engine frame at whole-plan adoption. Also keep
   the narrow before/after publication window, excluding compilation/ACK wait.
2. `affected_mix_frame`: where the affected sample reaches the mixed output.
   Label whether it was observed by a synthetic marker/audit or predicted from
   a scheduled event. A future note, a controller change on silence and a
   post-PDC gain change do not all equal `plan_applied_frame + L`.
3. `device_presentation_frame_estimate` and corresponding host time: map the
   affected output through the validated output timestamp/device-clock model.
   Include rate, validity flags, route and latency-generation provenance.

The current output callback discards `AudioTimeStamp`; it must retain bounded
timing observations to implement this contract. Check timestamp flags and the
time domain before using host/sample fields. A 48k engine frame and a 44.1k
device frame are not interchangeable integers. Preserve the rational conversion
or sufficient precision; do not accumulate independently rounded deltas.
[Apple callback timestamp contract](https://developer.apple.com/documentation/audiotoolbox/aurendercallback)

**Do not claim device arrival was measured by adding PDC and one device buffer.**
PDC is already included at the mixer-output point. Device/stream latency,
safety offset, buffering and conversion must be accounted for relative to the
chosen timestamp anchor, without counting latency already represented there
twice. Query properties outside the audio callback. Apple's SDK describes
device and stream latency as potentially additive, and safety offset as the
safe lead/lag from the current hardware position; these are different quantities.
[Apple presentation latency](https://developer.apple.com/documentation/avfaudio/avaudioionode/presentationlatency)

If the DefaultOutput timestamp-to-presentation mapping or a property is not
established, report `unknown` with a reason, not zero or an exact arrival frame.
A host/device timestamp estimate is not physical speaker-to-ear measurement.
Hardware loopback requires its own input-latency calibration before certifying
physical end-to-end timing. This uncertainty cannot be concealed in clock 2.

Acceptance: known-delay fake output clock at 48k and rational 44.1k conversion,
missing flags/properties, route changes and seeks; verify all three records,
no double-added PDC, and invalidation on epoch changes. Print estimated/observed/
unknown status in evidence instead of fabricating the third measurement.

## 5. Latency changes and seek lifecycle

Register an AU property listener for `kAudioUnitProperty_Latency`; the listener
only updates preallocated atomic generation/dirty state. It never logs,
allocates, queries properties or rebuilds the graph. Register before taking the
initial latency snapshot and verify the generation did not change during
preparation. The control thread reads/validates latency and constructs buffers.
[Apple property listener API](https://developer.apple.com/documentation/audiotoolbox/audiounitaddpropertylistener(_:_:_:_:))

A changed generation invalidates the prepared PDC graph. V1 requests controlled
transport stop and stopped reconstruction, with a visible reason; it does not
resize delays in the callback. Check before rendering and after all lanes: if
a change occurs during that render, do not present the block as correctly
aligned. Discard/mute the invalid block, record the interruption and stop.
Detach listeners and establish callback quiescence before freeing their context.
If monitoring cannot be installed, refuse the monitored realtime mode. A plugin
that changes latency without reporting it is outside the verified contract.

V1 seek explicitly permits a transport interruption; it is not seamless seek:

1. Resolve/cancel any pending document transaction; ramp/stop new note delivery.
2. For healthy AUs, deliver owned NoteOffs and pedal releases (CC64/66/69), plus
   the existing terminal CC123 reset. CC123 alone does not prove release tails
   or pedal-held sound have disappeared.
3. Stop/join output. Reset or recreate AU state on the control thread so old
   internal release/reverb buffers cannot leak into the new position. Clear
   PDC buffers, voice/key ledgers and old queued/prefetched data together.
4. Advance the transport epoch, rebuild/chase the target controller/note state,
   warm up/pre-roll as required, then resume all routes from one origin.

The product choice is a deliberate cut of old tails under the stop transition,
not indefinite tail preservation or an unannounced clear while keys remain
owned. Natural EOF instead drains releases and compensated tails. Tests must
hold notes under pedal across seek, then replay the same key: no old NoteOff
may kill the new voice, no old delay-page audio may emerge, and locks must reset
only with the transport epoch. Document/history revisions are not reset by seek.

## 6. Capacity and failure policy, decided before hardware integration

First implementation: **one serial audio rendering thread; at most two live
instrument instances admitted for the initial real-AU gate.** The graph remains
track-count general. Additional parts use explicitly prepared frozen tracks;
the existing 64-route model is not a claim that 64 live AUs meet a deadline.
Do not introduce callback worker barriers/thread pools in this iteration.

Measure the complete combined client callback, including renderers, delay lines,
mixing and streaming access. Separate runs' worst percentages cannot establish
the combined worst case. A local opt-in representative load check must pass the
<50% gate before certifying that configuration. This remains an observation,
not a guarantee against all later plugin/OS load spikes.

| Condition | Action |
| --- | --- |
| More than two requested live instruments | Refuse the realtime configuration; offer explicit freeze of selected tracks |
| Combined test reaches >=50% | Preserve FAIL, refuse certification; use fewer live instruments/frozen alternatives and remeasure only after an actual change |
| Running graph reaches >=50% | Latch a budget fault, request controlled stop, preserve accepted edits and report the offending run |
| >=100% or device overload notification | Record separately as budget overrun/device notification; stop safely when control returns; do not claim to undo an underrun that already occurred |
| One AU returns an error/invalid/nonfinite output | Latch that lane failed, discard its output and pending delayed samples, supply silence there, keep healthy lanes and the fixed graph latency running |
| AU blocks indefinitely or crashes the process | In-process hosting cannot preserve other tracks; process isolation remains required for that stronger guarantee |

No callback freeze/render-to-disk, hidden track dropping, automatic device buffer
changes or unbounded retry. Freezing is an explicit offline operation after
stop; use a saved valid cache or create a new one and validate it before graph
admission. The failed track is visibly quarantined, no more MIDI/render calls
are made to it, and its ownership is discarded for this runtime epoch; recovery
requires stopped reconstruction. A per-lane returned error is not conflated
with a whole-output/device fault. Already accepted edits remain saved after a
later DSP failure; there is no retroactive history rollback of played audio.

Quarantine must also be pollable throughout a long take: expose stable track ID,
latched error category and engine failure frame. Log output alone is insufficient.
Readers must acquire the latched status before its associated payload; audio
thread publication must not allocate or print. The kernel's `trackStatus()`
implements this portion; CLI/whole-session wiring is still required.

Mixed live/frozen PDC needs an explicit cache time-origin policy. Existing
frozen files do not certify algorithmic-latency removal. Version/bind the new
cache policy, source/performance/track-delay inputs and removed-latency metadata;
normalize new frozen samples to their documented musical origin before treating
their runtime algorithmic latency as zero. Reject an unknown-origin legacy
cache from the new compensated graph instead of silently double-compensating.
The legacy all-frozen player remains a distinct validated path until migrated.

Subscribe to `kAudioDeviceProcessorOverload` and retain its per-device/epoch
notification count separately from host buffer errors and callback budget
overruns. It supplies the device's reported overload signal, not an acoustic
dropout detector; unavailable registration is `unavailable`, not zero observed
overloads. Keep listener work bounded and teardown safe.
[Apple overload notification](https://developer.apple.com/documentation/coreaudio/kaudiodeviceprocessoroverload)

Acceptance: deterministic returned-error/NaN injection on one renderer leaves
the other lane's timing/samples unchanged; keep max latency fixed. Inject budget
and property-change notifications without a CPU-burning test, verify stop and
document preservation, then test the real combined load. Do not require an
in-process crash/hang test to pretend isolation has been implemented.

## 7. Four implementation gates

| Step | Deliverable | Required evidence before proceeding |
| --- | --- | --- |
| 1 | Minimal renderer contract, stable track bindings, explicit route-delay/version migration contract | Existing single-piano gates remain green; errors and latency values represented independently; old document becomes one track without changing its musical data |
| 2 | One whole-session stream/mailbox/history, parallel-route PDC and the three-clock receipt model | Atomic all-track rejection/acceptance, the full nonzero-delay matrix, pre-roll/unsupported-offset policy, buffer drain/seek tests, notification and lane-failure injections, allocation/TSan checks |
| 3 | Actual Pianoteq + SWAM in the same output run | SWAM realtime startup/CC11 readiness, independent MIDI/controllers on equal channel numbers, per-track captures, stable latency reports, total callback <50%, actual device diagnostics, documented freeze refusal/fallback; clock 3 remains labelled estimate/unknown until calibrated |
| 4 | Descriptor/registry-driven instrument creation | Another registered descriptor requires no scheduler/mixer switch; validation/preset quirks stay in adapters; all previous gates preserved |

Before graph integration, probe the installed SWAM's actual latency reports
across exposed presets/articulation settings. Preserve requested/read-back
values, plugin version, render mode and latency notifications. Stable results
only cover that tested configuration; they do not remove the runtime listener.

Step 3 also requires a real many-part editing turnaround measurement. For a
12-part fixture, record “select/edit part 7 -> changed ensemble available at
the output” using one monotonic wall clock, with stop, cache validation,
offline rendering/freezing, graph construction, seek/pre-roll and first changed
mixed-frame phases. Record reused/rebuilt track IDs and cache hit counts;
repeat while moving edit focus to a different part to expose required refreezes.
Report the device presentation estimate/unknown separately. Do not relabel
render completion or clock 2 as the instant a human heard the ensemble. This
metric has not been measured; cached renders make no automatic speed guarantee.

The single-piano `PerformanceAudition` becomes a one-track facade over the shared
runtime. Its proven identity, held-note and transaction invariants remain tests
of that runtime; its old ownership of one AU/capture/mix is not the new graph.
No step is marked passed by this design document. CI/workflow policy, licensing,
GUI integration, general bus graphs, fractional-sample PDC, seamless seek,
render-graph parallelism and plugin process isolation are not delivered here.

Source checks used local code/installed Apple SDK plus the linked Apple primary
documentation. Tavily metered calls: 0.
