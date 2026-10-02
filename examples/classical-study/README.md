# Courtyard Study

An original eight-bar engineering study authored for this repository. Two piano
staves, one cross-bar tie, and per-bar pedal changes exercise the public import
path. 40 notation elements map to 39 performed notes. Three adjacent same-key
releases are separated by one tick by the import repair policy.

This source is included under the repository's AGPL-3.0 license. No commercial
score, audio, plugin state or third-party preset is bundled. It is a small
example, not a repertoire fidelity benchmark.

Run from the repository root:

```sh
build/daw_performance_import --check examples/classical-study/study.musicxml
```

See [中文试用说明](../../docs/trying-the-alpha.zh-CN.md) for building, listening,
editing, bouncing and recovery.

[The import receipt](import-receipt.json) records the three actual one-tick changes.
It contains no instrument state or audio, and is reproducible from this XML.
