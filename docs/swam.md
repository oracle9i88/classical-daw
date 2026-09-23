# SWAM Cello 3: first expressive instrument integration

The macOS offline AU host now supports **Pianoteq 9** and **SWAM Cello 3**.
`daw_instrument_render` selects the instrument explicitly; `daw_piano_render`
remains a piano-only compatibility command. Both use the same host, MIDI
timeline, state persistence and stereo WAV writer.

```sh
cmake -S . -B build
cmake --build build --parallel 4
build/daw_instrument_render --list-presets --instrument swam-cello
build/daw_instrument_render cello.mid out/new-cello --instrument swam-cello
build/daw_instrument_render out/new-cello/score.dawproj out/new-cello-reload \
  --instrument swam-cello --state out/new-cello/instrument.aupreset
```

The input/output rules in [the piano guide](pianoteq.md) apply. The SWAM bundle
uses `instrument.wav` and `instrument.aupreset`, with their names recorded in
`report.json`. The default factory preset is `Cello`. Plugin binaries, licenses,
generated audio and instrument states stay outside the source repository.

## Expression and lifecycle

SWAM requires expression control; the default mapping is MIDI CC11, confirmed
by [Audio Modeling's controller guide](https://kb.audiomodeling.com/support/solutions/articles/206000050919-can-i-use-swam-instruments-without-a-physical-midi-controller-).
The CLI requires an explicit CC11 message **before the first attack on every
used MIDI channel**. A zero initial value is allowed for a deliberate crescendo.
Missing expression fails before loading the plugin or creating output. The
host preserves the original curve; it does not insert arbitrary dynamics.
Custom mappings using other controllers are not supported by this preflight.

In local probes, a bare command-line AU host successfully accepted MIDI but
returned silence. Initializing Cocoa on the main thread and servicing its run
loop before rendering produced sound. The shared host now creates an
`NSApplication` without a window, initializes the AU, then services the main
loop for a bounded **five-second SWAM startup interval**. This happens before
sample zero and does not shift the score. It is offline setup work, not a
realtime callback. The interval is a tested local workaround, not a vendor
readiness API or a guarantee that slow activation will finish on every machine.
An AU returning exact silence for sounding notes fails without writing a bundle.

SWAM's state can refresh a `datetime` field inside `jucePluginState` on restore.
The CLI retains the exact source snapshot instead of replacing it with a newly
timestamped state. It validates component identity and preset name. Unlike
Pianoteq, raw AU state byte equality is not a failure gate for SWAM; the observed
comparison is reported. The read-only integration checker compares the complete
property list and JUCE XML with only that datetime value excluded, plus audio.

## Verified locally, 2026-09-23

SWAM Cello 3.12.2, AU identity `aumu / Sce3 / AuMo`, factory preset `Cello`:

- Original eight-note phrase, 65 CC11 messages, stereo 48 kHz PCM16 export:
  **816,429 frames**, peak **0.282759**, zero clipping.
- Fresh-process project/state reload: identical PCM16 audio for this fixture;
  project bytes unchanged; state equal except the plugin datetime metadata.
- The same pitch, duration and attack velocity with CC11=25 versus CC11=110:
  sustained RMS ratio **4.82**. No sound before the scheduled note; tail decays.
- Missing CC11 fails without a partial output directory.
- The existing Pianoteq phrase still renders through the refactored shared host
  at its expected level, with zero clipping.

Reproduce only when the local instrument is installed and ready:

```sh
python3 scripts/make_swam_fixture.py out/my-swam-check
build/daw_instrument_render out/my-swam-check/cello-phrase.mid out/my-swam-check/first --instrument swam-cello
build/daw_instrument_render out/my-swam-check/first/score.dawproj out/my-swam-check/reloaded \
  --instrument swam-cello --state out/my-swam-check/first/instrument.aupreset
build/daw_instrument_render out/my-swam-check/soft.mid out/my-swam-check/soft-render --instrument swam-cello
build/daw_instrument_render out/my-swam-check/loud.mid out/my-swam-check/loud-render --instrument swam-cello
python3 scripts/check_swam_bundles.py out/my-swam-check
```

The observed local installation also contains SWAM Violin 3 and Violin/Viola/
Cello/Double Bass Sections. Those are **not yet enabled or rendered by this
adapter**. Multi-instance orchestral routing, legato/articulation validation,
MPE, recording, realtime monitoring and plugin UI integration remain pending.
Every part currently routes to one instrument instance. The cello fixture is
monophonic; this is not a complete orchestral mockup or a general plugin host.

Research used the local AU's preset/parameter metadata, Apple SDK headers and
one free web search call (two queries); **Tavily calls: 0**. No third-party code
was copied, and no GitHub workflow was launched or retried.
