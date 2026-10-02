# Contributing to Classical DAW

Start with the [Alpha guide](docs/trying-the-alpha.zh-CN.md) and
[architecture](docs/architecture.md). This is an evolving C++17 engine and CLI;
there is no complete public desktop product yet. The existing AGPL-3.0 license
is unchanged.

For a musical bug, include the commit, operating system, exact command, expected
result and actual result. Prefer a tiny original MusicXML/MIDI example; the
[public study](examples/classical-study/) is available to reproduce problems.
A successful import means that a file was accepted, not that its interpretation
is correct. Keep timing/pedal differences and repair counts in the report.

For a code change, use a separate branch and a focused regression where useful:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

Core tests do not require commercial plugins or an audio device. Native CLI
checks require macOS. Real AU probes are optional local checks; record the
plugin/version and distinguish manual rendering from hardware callbacks and
human listening. Do not include commercial presets, activation data or private
project recovery directories in a contribution.

Maintain notation/performance identity separation, whole-session publication,
callback allocation boundaries, ordered MIDI and explicit failure handling.
A repair must say what it changes. Preserve old document readers when changing
formats and add round-trip/rejection coverage. Do not rename a prototype or an
untested path “production ready.”

This work uses `[skip ci]` on commits to honor the owner's workflow-cost policy.
Do not repeatedly dispatch/rerun hosted workflows; submit local check evidence
and coordinate any hosted validation with the maintainer. No license changes,
source replacement or unrelated branch rewrites are part of routine fixes.
