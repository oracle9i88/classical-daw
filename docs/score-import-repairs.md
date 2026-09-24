# Importing real scores: what is repaired, and what is still refused

2026-09-24. The engine's readers were written to refuse anything they could not
represent exactly. That is the right rule for a round trip, where silent
corruption is worse than failure. It is the wrong rule for an importer: an
author cannot start work if the file will not open, and every notation program
emits constructs this engine has no model for.

Measured on the local corpora, the end-to-end import entry went from **9 of
1,105 MusicXML files to 589**, and imports **2,021 of 2,598 local MIDI files**.
Excluding scores with more than one part, which is a product decision rather
than a reader limit, 589 of 678 single-part MusicXML files import. Nothing
below relaxes an engine invariant: the strict path is unchanged and is still
what every round-trip test exercises.

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

Collision repair follows the chain that reaches furthest, not merely the one
before it in start order: a short or silenced chain otherwise hides a long one
still sounding underneath it, and the collision it conceals is exactly what the
audition refuses later. Chains are rebuilt after the tie pass, because a
released tie leaves its old grouping describing notes that no longer sound as
one, and reasoning from that stale grouping trims the wrong note.

## MIDI entry repairs

| Situation | Repair |
|---|---|
| Same channel and pitch sounding twice before the first release | One channel is one score voice, which cannot hold that. The sounding note is shortened; a second attack at the same instant is dropped. |
| A note released at or before its own attack | Dropped. It cannot sound, so there is no music to lose; exporters emit these routinely. |
| A release with nothing to close | Dropped. Nothing identifies what it referred to. |
| One instrument written as several tracks | Tracks whose channel sets intersect are merged into one part. Two tracks writing to one channel address one instrument; a second instrument there could not be controlled separately, so overlap is the grouping rule and it is transitive. Source ordinals are a per-track namespace and are cleared, which asks the writer for authored ordering rather than comparing two streams' ordinals. |

`readMidiFile` takes the repair report as a separate argument from its existing
`MidiImportReport`, because several callers already pass that report only to
observe and must keep refusing malformed input.

Counts are reported separately on purpose: on a Grieg lyric piece the first row
accounts for 156 of 164 changes, and reporting one total would badly overstate
what happened to the music.

## Still refused, and why

- **More than one part** (427 of the corpus). This is the two-live-instrument
  product decision, not a reader limit. It is the single largest remaining
  category and needs a product answer, not a parser fix.
- **Chord as the first note in a voice** (35), **durations not exactly
  representable at 960 PPQ** (25), **unsynchronized part measure starts** (14),
  **more than 4,096 attacks** (11), and a handful of metronome and polymeter
  cases. On the MIDI side the remaining failures are dominated by files whose
  tracks genuinely use separate channels, which are separate instruments.

Compressed `.mxl` remains unsupported.
