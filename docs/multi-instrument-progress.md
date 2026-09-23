# Multi-instrument implementation: first slice

2026-09-24; preceding design commit `8bd118d`.

**Priority update:** real-score [import and editing entry](performance-import.md)
now precedes further two-AU integration. Existing mailbox/PDC tests are retained;
their acceptance did not establish repertoire importability. The one-part
`PerformanceDocument` limitation is separate from the planned two-live-AU cap;
the existing offline session route model already supports multiple parts.

**Subsequent implementation:** [whole-session publication](live-session.md) is
now coded and tested; the old single-track API delegates to it. Also see the
[48-case SWAM attack-response table](swam-attack-response.md), including the
important negative differences from reported latency. The remaining complete
integration gates below are still open.

## SWAM latency experiment

Installed AU: **SWAM Cello 3.12.2**, `aumu/Sce3/AuMo`, encoded version 199682.
The isolated diagnostic initializes Cocoa/AU, services the main loop for five
seconds, subscribes to latency notifications and drives stereo 48 kHz / 256-frame
renders. It opens **no output device**. Production realtime remains Pianoteq-only.

After one discovery run, two explicit sweeps used `OfflineRender=0` and `1`.
Each recorded 12 factory-preset selections with matching read-back names and
27 parameter writes with matching numeric read-backs: 40 observations including
initialization. Every latency read before/after observations and during rendering
was **0.020 seconds = 960 frames**. One startup notification, no additional
notifications after baseline; every observation produced a nonzero sample peak.

The plugin supplied parameter names and enumeration labels. Controls tested:
Bow / Pizzicato / Col Legno, slow/fast tremolo, harmonics 2/3/4 Control, sordino,
advanced legato, bow lift, attack control; Source Delay Mode (No Delay, Real
Delay, Nearest), Source Distance and room-simulator enable. No guessed keyswitch
or CC mapping. Parameter sweeps follow the last enumerated preset, **Vivaldi**,
in the recorded sequential order; distance changes while Real Delay is selected.
This is not every preset×articulation combination, MIDI keyswitch path, user
state, sample rate, SWAM product or future version.

Decision: proceed with fixed-latency preparation **and runtime change detection**.
These observations do not make latency an immutable SWAM constant. The 20 ms is
a property report, not measured bow attack or speaker arrival. Nonzero peaks
do not certify articulation sound. Even `OfflineRender=0` uses a diagnostic loop,
not a CoreAudio callback; no SWAM realtime deadline/audition claim follows.

[Exact commands, raw outputs, read-backs, notifications and hashes](research/2026-09-24-swam-latency-evidence.txt).
`daw_swam_latency_probe` is opt-in, never CTest/CI. Discovery predates additional
read-back/value-label logging, as marked; both subsequent sweeps include it.

## Portable foundation

`PreparedTrackRenderer` separates prepared DSP, latency report/generation and
returned errors from plugin creation. `ParallelRenderGraph` owns fixed stereo
scratch/delay buffers and stable string track IDs; all renderers receive one
session frame. No instrument enum or AU code occurs in this kernel.

Construction refuses duplicate instances, checks latency generations before and
after reads/allocation, and accepts 48 kHz integral latency only (conversion
residual <=1e-6 frame, bounded to 96000 frames). Compensation is `max - own`.
Storage survives new quantum inputs. Muted renderers and delay lines keep
advancing; gain is post-PDC. L stays fixed on mute/failure. `latencyInfo()` and
control-only `statusText()` expose L and every determining lane.

Returned error or NaN/Inf quarantines only that lane. Its delayed samples become
inaccessible without a large callback clear, and it receives no further render
calls. `trackStatus()` supplies a pollable latched category/failure frame with
release/acquire publication. Healthy tracks keep their timeline and L. This
contains returned errors, **not plugin process crashes/hangs**.

Latency generations are checked before/after rendering. A change mutes the
invalid quantum and latches a stop requiring reconstruction. Portable injection
tests cover this, but the **production AU listener adapter is not wired yet**;
only the standalone probe subscribes to a real AU in this slice.

Session v3 stores signed `int64 track_delay_us`, defaults v1/v2 to zero, and
rejects overflow/fractional/malformed tokens. Existing mix edits/undo/redo retain
it. Nonzero execution fails before offline planning or either frozen player;
cache-performance comparison includes it. This is a schema reservation, with
no delay-edit command or negative-offset pre-roll yet.

## Validation boundary and remaining gates

The synthetic renderer emits an independently defined asymmetric stereo source
at `absolute_frame - declared_latency`; it does not reuse the host delay code.
Every isolated lane and sum are compared exactly, including zero samples.
Layouts: `[0,0]`, `[0,128]`, `[0,173]`, `[0,700]`, `[173,512]`, `[173,512,700]`,
and `[0,960]`. Each runs callback sizes 1/185/186/256/512/557/558 and a changing
pattern through production balanced subdivision, partial EOF and full drain.

Tests also cover nonempty buffers across a quantum-input change, muted clock
advancement, a failed maximum-latency lane, discarding a failed fast lane's
buffered audio, NaN, pre/post-render latency changes, second-lane validation
before any render, unsupported delay, concurrent polling and callback new/delete
counts. Standard new/new[] and delete/delete[] are intercepted; this does not
certify third-party allocator behavior.

Local validation: Debug **44/44**, ASan/UBSan **44/44** (leak detection off),
TSan PDC/status test passed, CLI preflight/export rejection **12/12** in Debug
and ASan/UBSan. Two affected player/mix tests were rerun after their final edits.
[Raw local test receipts and source hashes](research/2026-09-24-pdc-kernel-evidence.txt).

**No complete step is marked passed.** The kernel requires a coherent input
array from its caller; it does not publish or ACK document revisions. These
tests do not prove multitrack atomic adoption or held-note ownership. Never
construct its input from N independently adopting live-stream mailboxes.

Remaining, in the approved order:

1. Track-binding/document migration, unified history and prepared AU adapter.
2. Finish production binding/latency admission, coordinated seek/reset and
   three-clock receipts around the implemented whole-session mailbox; run the
   full synthetic matrix through that combined runtime. Basic nonempty PDC swap
   integration is tested, but is not that complete matrix.
3. Real Pianoteq+SWAM callback, separate captures, combined budget, device
   diagnostics and live/frozen integration. Measure the real 12-part edit-to-
   ensemble turnaround with cache-hit identities and phase timing. **No combined
   budget or turnaround result exists yet.**
4. Descriptor-driven factory/registry integration.

No GitHub workflow was run/retried. No plugin state/audio is published.
