# Local Pianoteq piano integration

The macOS `daw_piano_render` command hosts the separately installed Pianoteq 9
AUv2 (`aumu / Pt9q / Mdrt`) directly. It does not launch the standalone piano to
render audio. The third-party plugin and its presets/license are not bundled
with this repository. No activation or network operation is requested.

This is an **offline integration**, not yet the interactive Mac track editor,
realtime keyboard monitoring, a general plugin scanner, or browser playback.
The existing CoreAudio diagnostic callback still uses its sine voice.

## Usage

```sh
cmake -S . -B build
cmake --build build --parallel 4
build/daw_piano_render --list-presets
build/daw_piano_render piano.mid out/new-piano-bounce
build/daw_piano_render piece.musicxml out/new-score-bounce --preset 'NY Steinway D Classical'
build/daw_piano_render out/new-piano-bounce/score.dawproj out/new-reload \
  --state out/new-piano-bounce/piano.aupreset
```

The output directory must be new and its parent must already exist. Inputs may
be `.mid`, `.midi`, `.dawproj`, `.musicxml`, or `.xml`, subject to the engine's
existing strict interchange limits. Compressed `.mxl` is not supported. The
bundle contains editable `score.dawproj`, canonical `performance.mid`, a local
`piano.aupreset`, stereo 48 kHz PCM16 `piano.wav`, and `report.json` linking them.
Project v6 itself does not embed the instrument state; pass the sidecar explicitly
when reopening. AU state files and generated audio are git-ignored.

All parts feed **one fixed piano**, retaining original MIDI channels. GM bank
select/program changes are suppressed only during playback, counted in the
report, and retained in MIDI/project data. Other channel voice messages pass
through; the plugin's mapping determines their interpretation. This is not GM
orchestration or drum routing. Native plugin MIDI mappings can affect the result.

Note attacks/releases, release velocity and controllers use the full step-tempo
map and sample offsets within 256-frame blocks. The scheduler shares source
ordering/validation with the existing sine renderer. At the last source event,
the host releases sustain, sostenuto, hold-2 and all notes on used channels, then
renders five seconds of tail. Tail level and clipping are reported; a five-second
tail is not a universal guarantee for all effect presets. Terminal notated rests
after the last MIDI event do not extend the bounce. There are no host tempo
callbacks, plugin latency compensation, automatic normalization, or mastering.
Output allocation is bounded to 512 MiB of stereo float audio.

Factory preset selection and document-state restoration produced different
audio levels on the tested Pianoteq 9.2.2 despite identical exposed AU parameters
and class-info state. Every CLI bounce now consumes its serialized state in a
**fresh AU instance**, using the document restore property with class-info
fallback. This gives the initial bounce and project reload the same setup path.
The host checks state round-trip bytes before rendering. Third-party DSP is not
promised to be bit-identical: the verified fixture has small sample differences.

## Local verification (2026-09-23, Apple Silicon, Pianoteq 9.2.2)

- Normal and ASan/UBSan engine suites: 26/26 each. They do not load the plugin.
- Shared timeline tests cover step tempos, same-sample/source ordering, release
  velocity, end-pedal release, invalid fields, overflow and output budgets.
- Original eight-bar phrase: 48 notes, two tempos, 16 pedal messages; MIDI hygiene
  scan found no hanging notes, same-pitch overlaps or sub-30ms notes.
- Direct AU bounce and fresh-process project/state reload: 48 kHz stereo,
  1,352,864 frames each, no clipped samples, silent final second.
- Saved project bytes and AU property lists match after reload. Independent mido
  comparison confirms channel-message order, tempo and meter parity.
- Reload RMS relative difference: `5.55e-7`; PCM16 difference RMS: `9.44e-6`.
- Isolated probes: loud/soft attack RMS ratio `9.69`; pedal/no-pedal release RMS
  ratio `18.86`. Audio is silent before the scheduled block-boundary attack.
- Invalid preset/state, malformed MIDI, and an existing destination fail without
  replacing output or creating a partial bundle.

Fixture generation and read-only verification are reproducible without packages:

```sh
python3 scripts/make_piano_fixture.py out/my-piano-check
# Scan the four generated MIDI files with your MIDI hygiene tool first.
build/daw_piano_render out/my-piano-check/piano-phrase.mid out/my-piano-check/verified
build/daw_piano_render out/my-piano-check/verified/score.dawproj out/my-piano-check/verified-reload \
  --state out/my-piano-check/verified/piano.aupreset
build/daw_piano_render out/my-piano-check/soft.mid out/my-piano-check/verified-soft
build/daw_piano_render out/my-piano-check/loud.mid out/my-piano-check/verified-loud
build/daw_piano_render out/my-piano-check/pedal.mid out/my-piano-check/verified-pedal
python3 scripts/check_piano_bundles.py out/my-piano-check
```

Plugin tests are opt-in local operations, not CTest entries or GitHub workflow
jobs. Installation and successful sound do not certify activation status; the
report deliberately records `license_status: not_verified_by_host`.

Implementation references: installed Apple SDK `AudioUnitProperties.h`
(`ClassInfo`, `ClassInfoFromDocument`, `PresentPreset`, `OfflineRender`) and the
installed Pianoteq manual. No third-party implementation source was copied.
