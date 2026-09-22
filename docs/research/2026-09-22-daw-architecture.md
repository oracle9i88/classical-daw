# 2026-09-22 architecture study

This note records what was learned before the next implementation slice. The
repository is an AGPL project; these references are design and interoperability
inputs only. No third-party source was copied into the tree.

## Realtime engine boundaries

- [Ardour's audio engine callback](https://github.com/Ardour/ardour/blob/master/libs/ardour/audioengine.cc)
  keeps the audio callback small and treats device lists, watchdog work, and
  session changes as lifecycle work outside the callback. Its callback can
  report an xrun and return a safe buffer when a lock cannot be acquired.
- [Tracktion Engine's feature list](https://github.com/Tracktion/tracktion_engine/blob/develop/FEATURES.md)
  separates the editable `Edit` model and undo state from the prepared runtime
  graph. That is the pattern we will use: edit commands compile a new immutable
  graph off the realtime thread, then the audio block swaps it at a boundary.
- [Zrythm](https://github.com/zrythm/zrythm) reinforces asynchronous project
  saving and explicit callback/device lifecycle handling. A save operation must
  never make the audio callback or UI wait on disk I/O.

The immediate M0 gate is therefore a CoreAudio device adapter, a fixed-size
block scheduler, an xrun counter, and a bounded command queue. The callback
must not allocate, lock on contended state, parse files, scan plugins, or call
network code.

## Score and notation boundary

- [MuseScore](https://github.com/musescore/MuseScore) is a useful reference for
  MusicXML, notation, and playback regression coverage. It is GPLv3, so its
  implementation is not embedded in this AGPL codebase.
- [music21](https://github.com/cuthbertLab/music21) is BSD-3-Clause and can be
  used later as an offline analysis worker for chord symbols and Roman-numeral
  analysis. It does not belong in the realtime audio callback.
- [Verovio](https://github.com/rism-digital/verovio) is an LGPL C++ toolkit that
  can turn MusicXML/MEI into SVG and timemaps. It is a candidate for a read-only
  notation preview after the internal score model exists; it is not an editor or
  audio engine.
- [OSMD](https://github.com/opensheetmusicdisplay/opensheetmusicdisplay) and
  [VexFlow](https://github.com/0xfe/vexflow) are renderer options for a WebView
  preview. They do not replace the score model or transport.

MusicXML remains an interchange format, not the realtime edit state. The next
score model must retain pitch step/alter/octave, integer duration, voice/staff,
ties, tuplets, key/time signatures, tempo, and structured harmony fields. A
string such as `Cm7(b5)` must not be the only representation of harmony.

## License and adoption rule

Every future dependency gets a notice and license review before it is linked or
distributed. The first implementation slices stay self-contained so that the
engine's timing and realtime guarantees can be tested independently of a UI,
notation renderer, sampler, or AI service.

