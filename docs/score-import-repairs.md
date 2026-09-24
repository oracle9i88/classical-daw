# Importing real scores: what is repaired, and what is still refused

2026-09-24. The engine's readers were written to refuse anything they could not
represent exactly. That is the right rule for a round trip, where silent
corruption is worse than failure. It is the wrong rule for an importer: an
author cannot start work if the file will not open, and every notation program
emits constructs this engine has no model for.

Measured on the local 1,105-file MusicXML corpus, the end-to-end import entry
went from **9 files to 465**. Nothing below relaxes an engine invariant: the
strict path is unchanged and is still what every round-trip test exercises.

## Asking for repairs

`readMusicXmlFile` takes an optional `MusicXmlImportReport*`, and
`repairScoreForAudition` takes an optional `ScoreRepairReport*`. **Passing a
report is the request to repair.** Passing `nullptr` keeps the old behavior
exactly, so no existing caller or test changes. A repair alters sounding music
and is always counted; it is an interpretation of an unrepresentable construct,
never a claim of a lossless read.

## Reader repairs

| Construct | Repair |
|---|---|
| Grace note (no `<duration>`) | Borrows its notated `<type>` value from the note it decorates, capped at half of that note. The written span of everything else is unchanged. A grace with nothing left to decorate is dropped and counted. |
| Two different tempos at one tick | The later declaration wins, as it would for a reader meeting them in order. |
| More than one lyric on a note | The first verse is kept; the rest are dropped. |

Grace notes are buffered across a barline, because they are commonly written at
the end of a bar to decorate the downbeat of the next one. Dots and
`steal-time-*` attributes are not interpreted; the corpus uses neither, and the
borrowed value is capped against the principal regardless.

## Audition repairs

MIDI 1.0 cannot address two same-pitch notes on one channel at once, and the
audition rejects a release that lands *on* the next attack, not only one that
passes it. The score-to-MIDI bridge follows a tie only when its segments join
exactly. Real engraving produces all three situations constantly.

| Situation | Repair |
|---|---|
| Release exactly on the next attack (a repeated note) | One tick of silence. At 960 PPQ that is well under a millisecond, and it is counted separately from an audible change. |
| A genuine overlap | The earlier note is shortened, which is audible, and counted as such. |
| Two attacks at one instant on one pitch | The later one is silenced. Nothing here can decide which voice the author meant. |
| A tie whose segments do not join | The whole chain is released into separate attacks. Releasing only one flag would strand the other end. |

Counts are reported separately on purpose: on a Grieg lyric piece the first row
accounts for 156 of 164 changes, and reporting one total would badly overstate
what happened to the music.

## Still refused, and why

- **More than one part** (427 of the corpus). This is the two-live-instrument
  product decision, not a reader limit. It is the single largest remaining
  category and needs a product answer, not a parser fix.
- **Same-pitch retrigger after repair** (124). The repair pass does not yet
  reproduce every grouping the audition performs. Not diagnosed here.
- **Chord as the first note in a voice** (35), **durations not exactly
  representable at 960 PPQ** (25), **unsynchronized part measure starts** (14),
  **more than 4,096 attacks** (11), and a handful of metronome and polymeter
  cases.

Compressed `.mxl` remains unsupported. The MIDI entry still refuses a
same-channel same-pitch overlap during conversion, before the repair pass can
run; that path is not covered by this change.
