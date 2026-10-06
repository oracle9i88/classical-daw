# Web Alpha public release — 2026-10-07

Public URL: **https://classical-daw.oracle9i88.chatgpt.site**

The hosting service returned `status=succeeded` for version 1. Access is
`public`, so visitors do not need a project invitation. This is an Alpha for
music sketches and score interchange, not a commercial-ready orchestral DAW.

## What was released

- Up to 16 independent parts, 20,000 notes, 4,096 quarter-note beats and MIDI
  pitches 0–127, with tempo/meter maps, per-channel CC and editable track mix.
- MusicXML/MIDI import and export with visible unsupported-feature reports;
  no old 16-bar or C4–B5 import clipping. JSON v2 preserves the full web state.
- One bounded undo/redo history, explicit per-tab local recovery, validation
  before replacement, and a shared 8 MiB import/save limit.
- Original additive piano/string/sine synthesis in a module Worker, stereo
  audition and PCM16 WAV from the same buffer. Each audio range is limited to
  120 seconds and a stated polyphony/work budget, without truncating the score.
- The web application processes scores in the visitor's browser. It has no
  upload API, account, analytics, external samples or model dependency.

The web runtime is JavaScript, separate from the C++/AU engine. It cannot load
Pianoteq/SWAM, perform low-latency MIDI recording or engrave a full score. Grace
timing, repeat/jump unfolding and complete interchange fidelity remain open;
see the [format and usage boundaries](../web/README.md).

## Exact sources and package

Public repository branch: `feature/web-public-alpha-20261007`.

Web runtime and packager commit:
`2453d9cdcb07d36c0b4974b2c27ae6ede68f0157`.

Native mix-ramp/adapter follow-up commit:
`58d5402d260b3f76ec4f5a3c1f6def00d20bedc2`.

The standalone package was generated with:

```sh
python3 scripts/package_web_release.py --output out/web-release-20261007
```

- ZIP size: 57,445 bytes; 12 allowlisted files.
- ZIP SHA-256: `4b72dee785c92427340548aab6abca222923efc254921b9dfd79f30891a1cb7a`.
- `release.json` records each source/published file hash and the exact source
  commit. Source and license links in the HTML are pinned to that commit.
- Independent package checks matched every directory/ZIP byte, verified the
  exact allowlist and rejected an overwrite without modifying the old archive.
- No local scores, recordings, AU state, test fixtures, Git history or credentials
  are included. The source code and AGPL license remain openly linked.

The hosting checkout is a separate repository, not a replacement branch of the
DAW. Its `dist/` exactly matches the validated standalone package.

## Hosting receipt and future updates

Keep this site's existing identity; do not register a replacement for an update.
The local hosting checkout is `../classical-daw-site` relative to the DAW checkout,
with `.openai/hosting.json` containing `static.directory=dist` and the ID below.
There are no deployment credentials in this document or source control.

```text
project_id=appgprj_6ac5268dabb88191853849c50fa99af1
access_mode=public
access_revision=2
hosting_source_commit=36c256d318882030d813aacd086fc915ef8adeca
saved_version_id=appgprj_6ac5268dabb88191853849c50fa99af1~appgver_088866e37d748191a80a44955b8d4dae
version_number=1
deployment_id=appgdep_6ac527ea9e708191a385449203e38440
status=succeeded
url=https://classical-daw.oracle9i88.chatgpt.site
updated_at_utc=2026-10-06T16:55:16.391408+00:00
hosting_archive_sha256=72f08fc17a9adaf607b379eaf8c2f279bd6cd53eb42ca68c9d590cd1dfb262ee
hosting_archive_bytes=174080
hosting_archive_files=13
```

The hosting archive includes its manifest in addition to the 12 public files;
its hash and source commit therefore differ from the portable ZIP. It was saved
and deployed once, using the native Sites publishing tools. The success receipt
is the publication confirmation; it is not an additional production load test.

Future updates should validate locally, commit with `[skip ci]`, build a new
allowlisted package and publish that exact source to this same site. Do not
trigger or retry GitHub Actions for this path. Pages is now manual-only in this
branch; no workflow was dispatched or retried during this release, and no other
GitHub branch was overwritten or merged.

## Verification and limits of the evidence

- Native Debug: **54/54**; affected ASan/UBSan: **4/4**.
- Separate real installed Pianoteq + SWAM offline bounce: both parts nonzero,
  1,517,594 stereo frames at 48 kHz, 960 common latency frames removed,
  no clipping and no output device opened. PCM16 frame/peak/RMS independently
  matched the report within one LSB. This is not a fresh realtime benchmark.
- Web Node tests: **38/38**.
- Browser flows: **14/14**, including both root and subdirectory hosting;
  MusicXML DOM cases: **11/11** in both locations.
- Chrome 154.0.8037.98: real module Worker/PCM rendering. Most cases use a
  scheduling mock for exact buffer/cancellation assertions; one uses native
  Web Audio and confirms running state, advancing time and stop behavior.
  Chrome remains muted. No human listening or physical speaker sign-off is
  claimed. Desktop and mobile screenshots were inspected.

Original failures and repair results are preserved in the
[web evidence](research/2026-10-07-web-alpha-evidence.txt) and
[native evidence](research/2026-10-07-native-review-followup.txt).
The checked browser is desktop Chrome with a mobile viewport, not an iOS Safari
device. More browsers, large real-score corpora and musical listening remain
part of Alpha feedback; no claim of complete DAW readiness follows from these
test counts.
