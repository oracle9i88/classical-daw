# Importing real scores: what is repaired, and what is still refused

2026-09-24. The engine's readers were written to refuse anything they could not
represent exactly. That is the right rule for a round trip, where silent
corruption is worse than failure. It is the wrong rule for an importer: an
author cannot start work if the file will not open, and every notation program
emits constructs this engine has no model for.

Measured on the local corpora, the end-to-end import entry went from **9 of
1,105 MusicXML files to 643**, and imports **2,188 of 2,598 local MIDI files**.
Scores with more than one part account for 457 of the remaining refusals, which
is a product decision rather than a reader limit. Of the 648 files that are
genuinely one part, **643 import**; the five that do not are metronome and time
signature notations nobody in this corpus writes twice. Nothing
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

## Pedal is read, not invented

A MIDI file carries its own CC64 and always has: import preserves it, and the
compiled audition sequence contains every one of those messages. MusicXML
pedalling was simply not read, which is why a score imported from notation
sounded dry while the same music imported from MIDI did not.

`<pedal type="start">` and `"stop"` now become CC64 127 and 0 at the mark's
playback position; `"change"` releases and presses again at one tick. This is
written playback data rather than an interpretation, so it is read whether or
not repairs were requested, and no pedalling is invented for a score that has
none. Sostenuto and una corda marks are not read. 149 of the 1,105 corpus
files carry pedal marks.

## One part at a time

An audition plays one instrument, so a quartet or a two-staff export could not
be opened at all. `--part N` keeps one part and discards the rest, and without
it the importer now names the parts a score has instead of only refusing it.
This is not multi-instrument support and does not pretend to be; it is the
difference between those scores being unopenable and being work you can do a
line at a time today, and it costs the eventual multi-instrument path nothing.

Parts that disagree about where a bar starts, or about what meter they are
in, used to refuse the whole file. That denies every part at once, including
the one the caller wanted, and a part's own notes keep their places regardless
of what another part does. Both are now counted instead. A Score keeps one
global meter map and it is taken from the longest part, so the bar numbers a
multi-part read reports may be another part's; the importer says so when it
sees the disagreement and no part was chosen. The strict path still refuses,
and now says which bar and by how many ticks.

Alone, a part's unrouted notes address channel zero while its own controller
messages still carry the number of the part it used to be. A pedal sent to a
channel nothing listens on fails silently, which is the worst way to fail, so
those messages follow the part. Messages carrying an explicit route came from
a MIDI file and already agree with the notes beside them, and are left alone.

## Positions are rounded; lengths never are

960 ticks per quarter is 2^6 x 3 x 5. A septuplet or an eleven-tuplet has no
exact tick in it, and the reader refused any file containing one. The MIDI
reader has always handled the same problem the other way, and its own rule is
the right one: convert an absolute position once, rounding halves up, and
derive every length from two rounded positions. Lengths rounded on their own
lose a tick each and the drift accumulates; positions rounded on their own
cannot drift, and a septuplet still sums exactly to the quarter it occupies.

The reader now keeps its place as an exact rational in source units alongside
the tick cursor, and derives ticks from it. A note written shorter than one
tick is given one rather than dropped, since it was written to sound; what
follows it in that bar moves one tick later, because a note that had no room
to exist has to take room from somewhere. A cursor move shorter than a tick
moves nothing. Both are counted. The strict path is
untouched and still refuses anything it cannot represent exactly.

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
| A grace chord | Its tones sound together and borrow time once, not once each. A buffered grace also occupies its voice, so its own chord tones are not read as that voice's first note. |
| One instrument written as several tracks | Tracks whose channel sets intersect are merged into one part. Two tracks writing to one channel address one instrument; a second instrument there could not be controlled separately, so overlap is the grouping rule and it is transitive. Source ordinals are a per-track namespace and are cleared, which asks the writer for authored ordering rather than comparing two streams' ordinals. |

`readMidiFile` takes the repair report as a separate argument from its existing
`MidiImportReport`, because several callers already pass that report only to
observe and must keep refusing malformed input.

Counts are reported separately on purpose: on a Grieg lyric piece the first row
accounts for 156 of 164 changes, and reporting one total would badly overstate
what happened to the music.

## Two limits that were the same number and should not have been

`kMaxAuditionAttacks` bounds how many attacks one audition holds. It sizes a
per-track voice ledger and nothing the callback does per block. `kMaxEvents
PerRealtimeSlice` bounds how many events may fall inside one 256-frame block,
which is the realtime scratch capacity. Both were 4,096, which read as one
rule and behaved as two.

The block rule is correct at 4,096 and is unchanged. The attack limit is now
16,384, which costs sixty kilobytes of ledger per track and opens the
repertoire this is for: Beethoven's Op. 106 finale is 6,638 sounding notes,
Op. 57's first movement 6,059, Liszt's Vallee d'Obermann 7,373. Every one of
those was refused at four thousand.

## Checking what a save would check

`--check` used to stop after compiling the performance, so it could report a
score that failed only when someone tried to keep it: a silenced chord tone
kept its chord marker, which the project format rejects but the compile never
looked at. `validateScore` is now exposed and the check calls it, so a passing
check is a promise the file can be saved. The corpus numbers here are measured
with that check.

## Still refused, and why

- **More than one part** (427 of the corpus). This is the two-live-instrument
  product decision, not a reader limit. It is the single largest remaining
  category and needs a product answer, not a parser fix.
- **Unsynchronized part measure starts** (19). Every one is a multi-part file,
  so repairing it would free no score that the single-part limit does not
  already hold.
- Four metronome or time-signature notations, each appearing once. On the MIDI side the remaining failures are dominated by files whose
  tracks genuinely use separate channels, which are separate instruments.

Compressed `.mxl` remains unsupported.
