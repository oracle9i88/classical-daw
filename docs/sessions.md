# Independent instrument sessions and stems

The macOS offline session renderer gives each score part its own Pianoteq 9 or
SWAM Cello 3 AU instance. A part's notes and controllers stay together even if
another part uses the same MIDI channel. Routes use stable part IDs, not their
position in a UI or MIDI channel number. This is the first offline multi-instrument
slice, not a realtime mixer or complete orchestral host.

Session v2 now adds saved mute/solo and explicit pre-fader frozen-audio reuse.
See [frozen mixing](frozen-mixing.md) for the edit/reuse commands; the v1 example
below remains readable, while current output uses v2.

## Reproduce locally

Both licensed plugins must already be installed and ready on this Mac. The
repository contains no plugin binaries, activation data, presets or audio.

```sh
cmake -S . -B build
cmake --build build --parallel 4
build/daw_session_fixture out/my-duet
build/daw_session_render --check out/my-duet/duet.dawsession
build/daw_session_render out/my-duet/duet.dawsession out/my-duet/first
build/daw_session_render out/my-duet/first/session.dawsession out/my-duet/reloaded
python3 scripts/check_session_bundles.py out/my-duet
python3 scripts/check_session_cli.py
```

The fixture is an original engineering duet: 72 piano notes with pedal, eight
cello notes with 65 CC11 messages, a tempo change from 84 to 76 BPM, and a final
explicitly silent bar. Both instruments deliberately use MIDI channel 0.
The checker reads files only; it never launches plugins or retries rendering.
`--check` validates score, routes, expression prerequisites, frame budget and
state file accessibility. It does **not** load or validate AU state contents,
plugin installation, licensing, or factory preset availability.

## Session format v1

The native v6 score stays unchanged. A separate UTF-8 `.dawsession` file stores
instrument routes, static faders and stereo balance:

```text
CLASSICAL_DAW_SESSION 1
score "score.dawproj"
master_gain_db -3
routes 2
route "piano" "pianoteq" -4.5 -0.2 "NY Steinway D Classical" ""
route "cello" "swam-cello" -6 0.2 "Cello" ""
end
```

Each v1 route is `part_id instrument gain_db balance preset state_file`. There
must be exactly one route per score part. Preset and saved-state filename are
exclusive; both empty selects the instrument's default preset. Reopened bundles
use saved state files rather than reselecting factory presets.

Gain is bounded to -60..+12 dB. Balance ranges from -1 (left) to +1 (right).
Center preserves the original stereo signal; movement attenuates the opposite
channel with a cosine law. This is stereo balance, not a mono constant-power
panner. CC11 expression remains an instrument control, independent of the fader.

The score and state references must be plain sibling filenames. The renderer
rejects symlinks for these files and refuses existing output directories or
output symlinks. The format is bounded to 1 MiB, 64 routes, 16 MiB per state and
64 MiB of state data in total. These limits are not an assertion that a 64-part
orchestra has been performance-tested.

## Audio and saved bundle

Every stem uses the complete shared tempo map, the same sample-zero origin and
the same final score extent, including explicitly stored trailing rests. Final
pedal release/all-notes-off happens at that common end, followed by five seconds
of tail. Playback retains original note channels and message order. Controllers
are not borrowed from other parts; a SWAM route needs its own initial CC11.
Bank/program changes are retained in MIDI but suppressed during the fixed-preset
AU bounce, matching the single-instrument host.

Output is stereo 48 kHz PCM16:

- `track-N.wav`: after track gain/balance and mute/solo, before master gain.
- `track-N.dawfreeze`: pre-fader float audio with its exact source binding (v2).
- `track-N.mid` and `track-N.aupreset`: that part's isolated MIDI and state.
- `mix.wav`: sum of the float stems, then master gain.
- `score.dawproj`, `session.dawsession`, `report.json`: score, reload entry point,
  part-to-filename mapping, plugin versions, levels and render diagnostics.

To remix the exported stems, sum them and apply only the master gain; do not
apply the track faders twice. Separately quantized PCM16 stems reconstruct the
float-derived master within their quantization error, not necessarily bit for bit.
No normalization, limiter, bus processing or dither is hidden in this path.
The float intermediate mix can exceed unity. A stem or final master exceeding
PCM16 headroom makes the export fail with a gain-adjustment error instead of
silently baking clipping into the output.

Rendering is sequential, retaining one stem and the mix in memory. Each stereo
buffer is bounded to 256 MiB (roughly 11.65 minutes at 48 kHz including the tail);
the two audio buffers use up to roughly 512 MiB, plus plugin/state/score memory.
Streaming long works remains future work.

On ordinary exceptions the renderer removes only its own output files. The
session entry point is written last. A process or plugin crash can still leave
an incomplete directory; this is not an atomic directory transaction or plugin
crash sandbox. Completed bounced stems are preserved for direct audio reuse.

## Local evidence, 2026-09-23

Pianoteq 9.2.2 and SWAM Cello 3.12.2 produced two aligned stems and a master:
**1,517,594 frames each (31.6165 seconds)**, common end tick 34,560, zero clipping.
The first master peak was 0.131348 and RMS 0.019743. Independent checks compare
source part MIDI, project bytes, route settings, stored state and mix reconstruction.
Core routing/mixing tests pass in both the normal and ASan/UBSan 27-test suites.
Eight plugin-free CLI checks also pass, including malformed input, unknown parts,
missing/symlinked state and preservation of existing outputs. A deliberate
master-overload bounce (track gains +1.5/0 dB, master +12 dB) failed after both
stems had rendered and removed its partial bundle, as intended. Reconstructing
each successful master from its PCM16 stems differed by at most one PCM step.

**AU rerendering is not proven sample-deterministic.** The piano reload difference
RMS was 0.00000830. SWAM's first factory-selected session versus reopened session
had a difference RMS of 0.016745, despite identical stored state/MIDI. Its 100 ms
energy-envelope normalized error was 1.71%, total RMS changed by 0.066%, and the
first nonzero PCM sample differed by 33 frames (0.688 ms). A second fresh-process
reopen of the exact same saved session matched the first reopen's cello PCM
exactly. The cause of the initial-versus-reopen difference remains unresolved;
these results do not establish stable behavior across versions or machines.

The integration checker strictly compares saved routing/state/MIDI and piano
waveform tolerance. For all audio it checks total level, 100 ms energy envelope,
onset, length, headroom and stem-sum reconstruction, while explicitly reporting
sample differences. Its passing result is **not** a deterministic-waveform claim.
Use bounced WAVs or the new explicit [frozen reuse](frozen-mixing.md) when exact
audio repeatability matters. Automatic per-part rerendering/invalidation has not
yet been implemented.

Still pending: additional SWAM instruments, articulation/legato validation,
real-time plugin playback and mixer controls, effects buses, automation, latency compensation,
plugin UI and a graphical mixer. These commands are not integrated into the web
prototype or a finished macOS application shell. No GitHub workflow is required.
