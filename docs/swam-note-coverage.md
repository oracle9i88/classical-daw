# SWAM missing low notes: incident and correction

The user reported hearing the cello only around 2, 12 and 21 seconds. A
second-by-second audio measurement confirmed that the other intended phrases
were absent. Raising playback volume could not fix this. The earlier claim that
nonzero whole-file audio meant the cello had rendered correctly was insufficient.

## Cause

The duet's written MIDI notes are `48,44,41,43,48,46,43,48`. The locally captured
SWAM Cello factory state contained `transpose=-12.0`, with instrument range
`36,89`. Only the three written notes 48 remained within range after the
instrument's transposition. Notes below 48 were silently ignored by the plugin.
The MIDI schedule and frozen remix preserved the input faithfully, including
this incorrect sound-source configuration.

The AU's Transpose parameter is normalized, not measured in semitones. A local
query of `ParameterStringFromValue`/`ParameterValueFromString` returned:

| AU value | Displayed semitones |
|---|---|
| 0 | -24 |
| 1/3 | -12 |
| 1/2 | -6 |
| 2/3 | 0 |
| 1 | +12 |

Direct parameter writes updated the exposed AU value immediately while the
serialized internal state initially retained -12. Servicing the setup instance's
message loop synchronized that state, but disposing this prematurely started
instance disturbed a later instance in a local multi-instrument probe, which
then returned silence. A fresh process using the correctly saved state rendered
all eight notes. We did not adopt that parameter-write/startup approach.

## Final implementation

For a **new factory selection**, the host reads a bounded state snapshot,
validates its AU identity and known SWAM schema, changes only its stored
`program/midimapping/params/PARAM[@id='transpose']` value to `0.0`, then restores
that copy through the AU document-state interface. It verifies that recapturing
the state reports zero. It does not pump a setup instance's startup loop or
write a global preset. Unrelated settings remain intact.

For a **saved user state**, the host preserves its transposition and validates
every MIDI note against the resulting playable range. The same state-only check
runs for frozen reuse, without instantiating a plugin. A stale historical bundle
with out-of-range notes now fails explicitly; it cannot quietly reuse the already
incomplete audio. Use the original score with a new concert-pitch factory route
to produce a corrected bundle. Old artifacts remain unchanged for comparison.

The source notes, velocities, CC curves, tempo and durations are unchanged.
This narrow profile does not implement general keyswitch/articulation mapping,
MPE, alternate tunings or automatic instrument substitution. The parser rejects
unsupported ranges/schema, malformed values, ambiguous transpose entries and
DTD-bearing XML. CRC/source binding alone is not a musical-completeness check.

## Measured acceptance

- The old duet's audio-energy/harmonic gate passes **3/8 notes**, reproducing the
  user's report. An isolated corrected render passes **8/8**.
- The corrected multi-instrument bundle passes **8/8**, and a fresh-process
  reopen of its saved project and states also passes **8/8**.
- That corrected cello stem also matched its reopened PCM byte for byte in this
  fixture; this is a local observation, not a cross-version determinism promise.
- All **29** normal and ASan/UBSan CTest cases passed. The corrected bundle passed
  the 21-operation frozen-mix check, eight CLI preflight checks and independent
  stem-sum/reload validation. The old bundle's preflight now rejects MIDI note 44
  at tick 4320, which its saved -12 setting would turn into out-of-range note 32.
- Existing instrument states are not silently rewritten; the original -12
  state is rejected when it would suppress the fixture's low notes.
- Unit tests cover range boundaries, transposition, malformed state, unrelated
  parameter preservation and source immutability, without loading an AU.

Read-only checks (Python environment with optional `mido` and `numpy`):

```sh
python3 scripts/check_cello_notes.py BUNDLE/track-2.mid BUNDLE/track-2.wav
python3 scripts/check_session_bundles.py FIXTURE_ROOT
python3 scripts/check_swam_bundles.py SINGLE_CELLO_FIXTURE_ROOT
```

The cello checker derives timing from the actual MIDI tempo map. For each
non-overlapping sustained note of at least one second it measures an interior
audio window, requires RMS above -55 dBFS, and checks that the strongest spectral
peak matches an integer harmonic of the written pitch within 35 cents. It is a
regression gate for these intentionally audible fixtures, not a general pitch
tracker, proof of musical quality, or acceptance rule for deliberately silent,
very soft, short, overlapping or expressive production passages. In particular,
harmonic matching alone cannot rule out every octave ambiguity. Stored instrument
transposition is checked separately.

The single-cello and session integration checkers now include this note-level
gate, so a repeatable but incomplete performance can no longer pass merely by
having nonzero aggregate RMS. Local tests and probes did not launch or retry any
GitHub workflow.
