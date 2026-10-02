# Performance editing: recoverable local Alpha

2026-10-02. This change is on a feature branch. It does not certify a public
release, multi-AU editing, or general MusicXML fidelity.

## Ordered controller steps

`ControlCurve::stepped` distinguishes exact scheduled steps from the legacy
10 ms linear interpolation. `curve-adopt` retains every controller message at
its scheduled 48 kHz sample, including release/press pairs at the same frame.
`curve-step ID CHANNEL CC POINT_ID SECONDS VALUE [...]` authors this mode.
One point is sufficient; subsequent times may be equal, and vector order is
significant. A linear `curve-put` still needs two strictly increasing points.
Both modes require a point at zero. Missing initial values default to CC64=0,
CC11=127 on adoption. Terminal release resets remain authoritative.

Same-frame ordering **within the adopted lane** is retained. Explicit curves
still precede same-frame source notes and other controllers: adoption does not
promise preservation of arbitrary cross-lane/source-note interleaving.

Documents containing a step lane save as performance format v2; other documents
retain v1 byte layout. This version reads both. Old binaries reject v2; do not
expect an older editor to reopen newly authored step curves. Both modes use
the same command stack, undo and recovery mechanism.

## Output gain

`gain DB` is a saved document output gain (-60..0 dB), shared by audition and
`daw_performance_render`. The bounce preserves AU float headroom, applies gain,
then writes PCM16. Its report includes `output_gain_db`, post-gain/pre-PCM peak
and RMS, and the number of over-unity samples clipped during PCM conversion.
This fixes older bounces that ignored the saved audition gain. DSP randomness,
live gain ramps and range-preview fades still prevent an audio-byte parity
promise. No normalization or mastering is implied.

## Position and passage audition

`seek SECONDS`, `range FROM TO`, `range-clear`, `repeat on|off` are CLI transport
commands. Position/range changes stop playback; `play` prepares a fresh AU.
Pre-roll consumes the entire preceding MIDI sequence and renders discarded
audio, including released-but-pedal-sustained notes. Nothing is guessed from
the held-key ledger. The old output is stopped before destroying its AU and
ledger. Endpoints are sample-quantized; the callback emits silence after the
range end. Short 128-frame boundary fades avoid hard waveform cuts.

Range repetition is explicitly a stopped/rebuilt preview with a gap; it is not
seamless looping and does not preserve a live AU across passes. Pre-roll is
linear in elapsed score time and blocks the CLI during preparation. Transport
settings are not persisted. This conservative implementation does not replace
future graph-wide seek/reset and PDC drain work.

## Recovery

The CLI establishes a baseline before loading/editing the source. Each accepted
revision is saved to an exclusive sibling recovery directory. All three document
files are completed before publishing `latest` by rename. Two completed
snapshots are retained; cleanup never recursively deletes unexpected content.
The original document is never overwritten. Concurrent editors use distinct
recovery folders. A failed save leaves the edit accepted but prints an explicit
failure and keeps the previous complete pointer.

`daw_performance_recover list SOURCE` lists candidates; `restore SOURCE RECOVERY
NEW_DIRECTORY` validates source identity, lengths, file types and a
length-delimited FNV-1a checksum before loading and saving to a new directory.
The checksum detects accidental corruption; it is not authentication for hostile
files. Recovery requires the unchanged source directory. Undo history is not
serialized. Full snapshot/validation cost remains on the control thread and
can delay commands on large scores. No fsync/power-loss guarantee, cloud backup,
or automatic restore is claimed. Core rename behavior is tested on macOS.

## Reproduction

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
python3 scripts/check_performance_recovery_cli.py
build/daw_performance_import --check examples/classical-study/study.musicxml
```

The recovery script uses a fake state only for data operations, never `play`.
It kills the editor after an acknowledged autosave and restores exact bytes.
The engine regression verifies non-quantized preroll MIDI equivalence, repedal
order, format round trips, failed checkpoint retention, stale-source rejection,
and gain-before-clipping.

Optional local Pianoteq check (no output device):

```sh
build/daw_performance_range_probe /path/to/real-pianoteq-document
```

This compares a positioned AU render with an uninterrupted render over frames
60001..84001, including preview fades, with a 2% relative RMS tolerance. It is
manually driven AU verification, **not** CoreAudio callback timing or listening
approval. Short documents are rejected before rendering.

## Remaining public-use gates

Production multi-instrument live documents/PDC integration; granular import
provenance; large-score autosave/seek turnaround measurements; complete GUI,
installation packaging and testing on a clean independent machine. These remain
open. No workflow is dispatched or retried by this work.

## Measured in this iteration

- Ordinary local CTest: **49/49** (macOS). No CI run.
- ASan/UBSan: affected performance/readiness/import/live tests, results in the
  [evidence file](research/2026-10-02-alpha-readiness.txt).
- Process-kill CLI recovery: exact document bytes recovered; no AU or device.
- Original eight-bar public example: import and generic data-only editing probe
  pass, 40 notation elements / 39 performed notes, three reported one-tick
  repeat separations. This is not a repertoire census.
- Real Pianoteq AU, manual render of the positioning probe: relative RMS error
  **0.000146127** (about **0.0146%**); peak **0.0456715**. No output device.
- Real Pianoteq bounce at -12 / -18 dB: measured RMS ratio **0.501186635**;
  expected **0.501187234**. Both outputs non-silent and no clipped samples.
- The first sandboxed AU probe failed with “AU is not registered”; the component
  existed on the host, and the host-access run succeeded. That failure is retained
  in the evidence, not counted as an audio failure or silently omitted.

No new hardware callback benchmark, speaker listening, cold-machine install,
full-corpus scan, or deployed public release was performed.
