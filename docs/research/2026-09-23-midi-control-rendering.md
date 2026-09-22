# Offline MIDI performance rendering — 2026-09-23

Subsequent update: [project v4 tempo persistence](2026-09-23-score-tempo-persistence.md)
removes the single-BPM Score limitation recorded below. This document retains
the original controller-rendering acceptance baseline.

This slice makes retained performance events affect a diagnostic audio signal.
It is original C++ code, uses no external sound library, and does not implement
a production piano/orchestral instrument or connect this renderer to CoreAudio.

## Contract

`renderMidiFile` accepts synchronous Type 0/1 data normalized to 960 PPQ and
uses the complete `MidiFile::tempo` map. `renderNotes` and `renderScore` call
the same renderer; Score still retains only one BPM, so its later-tempo loss
is not fixed by this work.

All tracks share 16 channel states. Cross-track events sort by tick, then
track index; source ordinals only order messages within their own track.
The SMF writer and renderer share their within-track ordering helper. Default
authored events use note-off → source-ordered messages → new controls →
note-on. Different ticks keep their order when rounded to the same sample.
Stale explicit note ordinals that conflict with an adjacent same-pitch
retrigger fail before rendering, as they already do on MIDI export.

| Message | Implemented diagnostic behavior |
| --- | --- |
| CC64 | Binary sustain: 0–63 off, 64–127 on; lifted keys wait for pedal-up |
| CC7, CC11 | Independent linear volume/expression gain, both initially 127; affect active and releasing voices |
| Pitch Bend | 14-bit, center 8192; fixed ±2 semitones, continuous oscillator phase; endpoint divisors 8192 below center and 8191 above |
| CC120 = 0 | Immediately clear all voices on this channel |
| CC123 = 0 | Release all keys on this channel, honoring sustain |
| CC121 = 0 | Reset expression, bend and sustain; retain volume; held keys keep sounding |
| Other channel messages | Retain data and count unsupported events; no sound implementation |

These mappings use the [MIDI Association controller table](https://midi.org/midi-1-0-control-change-messages)
and [message summary](https://midi.org/summary-of-midi-1-0-messages). Retaining
CC7 on reset follows the RP-015 semantics cited by
[RFC 4695, A.3.1](https://www.rfc-editor.org/rfc/rfc4695.html#appendix-A.3.1).
The diagnostic gain curve, startup volume and fixed bend range are explicit
engine choices, not a claim of full General MIDI conformance. No third-party
implementation code was copied. Tavily metered calls: **0**.

Each note has its own voice identity, including same-pitch overlaps and pedal
retriggers. The envelope attacks over 8 ms and releases over 35 ms **after**
key/pedal release. A pedal pressed after a release does not recapture that
voice; there is no half-pedal/repedaling or acoustic piano decay model.

The render ends at the latest note edge or retained channel event plus the
requested tail. Trailing SMF End-of-Track silence is not retained by the MIDI
model. At the final event, still-held voices are released and counted. A
tail shorter than 35 ms can truncate their release. The CLI uses 100 ms.
Release velocity is counted when nonzero but does not alter the sine envelope.
Program changes, pressure, RPN bend sensitivity, pan, effects and percussion
sounds are not implemented.

The output is mono, mixed before one final clamp. `MidiRenderReport` separately
counts applied/unsupported channel messages, final forced releases, ignored
release velocities and clipped samples. Failure leaves the caller's report
unchanged. Sample rate is rounded once before all timing conversions. Output
length ignores a one-ULP floating-point overshoot at an integer frame boundary.
The default audio allocation limit is 134,217,728 float frames (512 MiB,
about 46.6 minutes at 48 kHz); the host can explicitly pass another frame
budget. This bounds audio allocation, not the imported event model. Streaming
rendering and a production mixer remain pending.

## Reproducible checks

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 4
ctest --test-dir build --output-on-failure
python3 scripts/check_midi_render_cli.py

cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug \
  '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
  '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined'
cmake --build build-sanitize -j 4
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-sanitize --output-on-failure
```

The 15-test suite includes black-box waveform checks for sustain thresholds,
release tails, cross-track channel sharing/isolation, exact gain ratios,
frequency measured from zero crossings, controller resets and all-notes-off,
event order and same-sample rounding, note identity, tempo changes, diagnostic
counters and failure preservation. Score rendering separately verifies that
a controller in another part addresses the shared MIDI channel. CLI checks
cover 14 cases including invalid rates/input, existing output preservation,
dangling symlink refusal, metadata agreement and unchanged source hashes.
ASan/UBSan are checked with leak detection disabled; this is not a leak audit.

## Local LilyPond corpus

Read-only sources were reused from neighboring projects; they are not added
to the repository. Full 48 kHz PCM16 mono WAV files and JSON reports are under
the ignored `out/midi-control-render-20260923/` directory.

| Source | Notes | Output frames | Applied controls | Unsupported | Clipped samples |
| --- | ---: | ---: | ---: | ---: | ---: |
| `../baroque-violin-sonata/sonata.midi` | 524 | 5,124,795 | 0 | 4 programs | 0 |
| `../chopin-op9-no1-light/score.midi` | 1,718 | 12,691,688 | 276 CC64 | 0 | 119,141 |
| `../liszt-ballade-romantic/score.midi` | 6,958 | 44,126,071 | 0 | 0 | 524 |

All outputs contain nonzero PCM audio, correct WAV/JSON frame counts and
unchanged source SHA-256 hashes. Independent mido 1.3.3 timing calculations
(absolute PPQ normalization, merged tempo changes, last channel-message tick
plus 100 ms) match all three frame counts exactly, including the Liszt tempo
changes. These are waveform/format checks, not a listening-quality judgment.
The two nonzero clipping counts are known limitations of summed diagnostic
oscillators; they are not concealed by normalization or called production mixes.

Example (choose a new output directory each time):

```sh
./build/daw_midi_render ../chopin-op9-no1-light/score.midi /tmp/new-chopin-diagnostic 48000
```

The tool creates `diagnostic.wav` and `report.json` only after parsing/rendering
succeed, refuses existing output paths, and only cleans up its own files on
failure. All validation is local. No workflow dispatch or retry is required.
