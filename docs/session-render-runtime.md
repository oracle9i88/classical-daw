# Whole-session render runtime and first two-AU integration

2026-10-02. Feature branch; this is an integration layer and an opt-in diagnostic,
**not yet the multi-instrument document editor or a public release**.

## Ownership and ending

`SessionRenderSource` owns exactly one `LiveSessionStream` and one
`ParallelRenderGraph`. Initial stable route order must match renderer bindings;
subsequent plans can reorder routes without remapping instruments. The callback
adopts one complete revision, then renders all lanes in that engine quantum.
Renderer instances are borrowed, prepared on the main/control thread and kept
alive until output has stopped/joined. There are no per-lane mailboxes.

V1 admits one or two live renderers. A third is explicitly rejected; the runtime
does not silently freeze tracks or launch parallel rendering. The portable PDC
kernel still tests up to three lanes, but that is not live-instrument admission.
Musical `track_delay_us` remains separate from algorithmic latency and nonzero
execution remains rejected.

The runtime latches `Playing -> Draining -> Finished`, or `Failed`. Sequence
length already includes terminal MIDI resets and the requested release tail.
After that length, both plugins and host compensation lines run for another
`L=max(plugin latency)` frames, with no new MIDI. Partial final quanta are
zero-filled; the graph clock ends exactly at sequence length + L. This keeps
the last delayed samples, but does not promise an unlimited plugin reverb tail.

Live musical swaps preserve nonempty delay lines. Quarantine is pollable per
track and does not change L. A latency notification on a healthy lane latches a
global fault: the caller must stop/join and rebuild. A DSP failure and latency
change in the same render cannot conceal that notification behind quarantine.
No automatic recovery, seek/reset or seamless loop is added here.

Submission is control-thread-only. Host acceptance is not DSP success or proof
that a human heard audio. Publication can race the final render after MIDI has
already been consumed: the unadopted ticket is cancelled by `waitForDecision`,
not reported as applied. Submit after draining begins is rejected. A dedicated
threaded regression forces exactly this race. The embedding caller must inspect
receipts and stop on global failure; destruction never occurs in the callback.

## AU adapter

`AudioUnitTrackRenderer` bridges the same graph contract to the existing local
AU host. The sequence-taking `prepareRealtime` entry permits Pianoteq and SWAM
Cello after initial 48 kHz / SWAM CC11 and saved-transposition range checks.
The legacy no-argument entry remains piano-only. SWAM gets its bounded Cocoa
startup service before sample zero, never in the audio callback. Runtime note
checks quarantine a SWAM lane if a caller bypasses initial pitch/expression
admission; future document editing must reject such edits before committing.

AU latency listener registration precedes initialization. Notifications only
increment a lock-free generation; no property query occurs in the callback.
The prepared seconds/generation pair is immutable. Startup notifications are
absorbed before reading the baseline; subsequent changes invalidate the graph.
The listener is removed during stopped destruction and its context remains
alive through AU uninitialization/disposal. The single-piano audition also now
checks its baseline before/after each render.

Plugin internals are still in-process third-party code: allocation, locking,
hangs or crashes are not isolated by host quarantine. Quarantine covers returned
errors and nonfinite output. These measurements do not certify plugin behavior.

## Verification

- Normal local CTest: **51/51**.
- ASan/UBSan: session runtime, whole-session mailbox and PDC tests **3/3**.
- ThreadSanitizer: combined runtime (including forced EOF publication race) **1/1**.
- Existing real Pianoteq range comparison: relative RMS error 0.00012919,
  below its 2% tolerance; no output device.
- Combined runtime: 36 exact-sample cases covering latency pairs 0/0, 0/173,
  173/512, 173/700, 0/960 and 700/960, with request sizes 1/185/256/512/557/558.
  Independent sample oracle includes the very last source impulse, asymmetric
  stereo, complete drain and silence after finish.
- Whole-revision gain edit with reordered routes; late-track guard rejection;
  held-note on/off/reset counts; preserved nonempty compensation buffers;
  independent quarantine, fixed L, latency-stop, two-live cap, EOF race.
- Test callbacks record zero host `new`/`delete`; this excludes third-party DSP.

Opt-in local commands (not CTest/CI):

```sh
build/daw_session_au_probe PIANO.aupreset CELLO.aupreset
build/daw_session_au_probe PIANO.aupreset CELLO.aupreset --hardware-silent
```

Both modes always suppress speaker output and never save plugin state. The
first drives the real AUs manually without a device; the second uses CoreAudio
and measures the **complete client callback**, not its individual subdivisions.
Licensed plugin states stay local, outside the repository.

Observed versions: Pianoteq 590338; SWAM Cello 199682. Both reported the expected
0 / 0.02 seconds, so the graph used L=960, piano compensation=960, cello=0.
The six-second workload has two monophonic lanes, two attacks per lane, CC11,
terminal resets and one atomic edit to both future onsets while old notes sound.
Both lanes emitted exactly two NoteOn and two NoteOff messages and nonzero audio.
This checks adapter integration; exact PDC proof is from synthetic known-delay
sources, not alignment of piano transients with bowed-string attacks.

One hardware run, no retry: device 44.1 kHz / 512 frames; client 48 kHz.
520 callbacks; maximum full-callback time 0.004100792 seconds at 557 frames;
budget ratio **0.3533896158**, zero measured deadline/headroom misses, zero
callback errors. Lane peaks 0.2716913 / 0.3241267, mix peak 0.1164981.
The graph ended at 288961 = 288001 + 960 frames. Applied revision 1 at frame
24335. These are workload-specific observations, not orchestral load capacity.
No listener change was induced during this take: SWAM generation 1 was already
its preparation baseline. Synthetic generation changes exercise the stop path.

[Raw evidence, including sandbox failure](research/2026-10-02-session-render-runtime-evidence.txt).
The initial sandbox run could not see the installed Pianoteq registry. Host
access resolved discovery; no plugin installation or activation was performed.
First compilation found two signedness warnings; explicit index conversion
removed them; final normal/ASan builds had no warnings.

## Gates still open

- Multi-part performance documents, unified editor transactions and public CLI
  commands using this runtime; the existing `PerformanceAudition` remains a
  separate piano implementation. This probe deliberately has a fixed workload.
- A real score routed to two AUs with editing, undo, save/reopen and rendered
  reference comparisons; heavy piano polyphony plus expressive cello stress.
- Three-clock receipts: application frame is observed; changed audio mixer
  exit is not detected by this probe; device presentation time is **unknown**.
  `graph_output_end_frame` is an EOF clock, not an edit's audibility timestamp.
- Twelve-part edit-to-ensemble turnaround, frozen/live common origins, seek
  reset/held-note coordination, stopped focus switching and cache accounting.
- HAL/driver work beyond the client callback, native 48 kHz device coverage,
  runtime budget admission/fallback policy, human listening and process isolation.

No GitHub workflow was dispatched or retried. Local tests do not substitute for
an independent-platform build. No license changes.
