# Persisted import repair receipts

2026-10-02. New `daw_performance_import` documents retain an import receipt.
It describes this import, not the current state after subsequent editing.
It is historical data, not an executable repair/revert command.

## Inspect a receipt

```sh
build/daw_performance_provenance /path/to/document > import-report.json
```

This portable tool loads no plugin and opens no output device. Missing receipts
in legacy documents return exit 2 and an explanation on stderr; the tool never
invents a report by comparing a later edited score. Other errors return 1.
In the macOS editor, `provenance` displays the same receipt. The JSON may be
large, so the standalone redirected command is preferable for long pieces.

The [original study's actual receipt](../examples/classical-study/import-receipt.json)
shows three changes. For example, notation ID 6 in measure 2 retains its tie-stop
and pitch C5, while duration changes from 960 to 959 ticks. ID 6 is a notation
segment ID, not a performed-note ID.

## What is covered

The `source` field records a basename (no absolute home directory), byte count
and FNV-1a 64-bit fingerprint. The importer fingerprints before and after reading/
processing and rejects a detected change. This detects accidental source changes;
it is not a cryptographic provenance or authenticity certificate. Keep the original
source file. Input fingerprinting is bounded at 256 MiB.

`selected_part_one_based` records the requested `--part` (zero means no explicit
selection). Reader counts can include parts parsed before selection, even if
only one part enters the final document.

| Stage | Coverage |
| --- | --- |
| MusicXML reader | Counts of grace realization/drop, tempo conflict, lyric drop, rounding, widening and part disagreement |
| MIDI reader/score conversion | Counts of overlap/drop, orphan release, silent notes and track merging |
| Audition repair on the selected score | Ordered per-note before/after records for repeat separation, overlap trimming, silencing and broken tie release |

Detailed records include part ID, one-based measure ordinal, printed measure
label, one-based note ordinal and `notation_id_at_import`. The snapshots include
start/duration ticks, pitch spelling, octave, staff, voice, MIDI channel, rest/
chord/tie flags and lyric. Channel -1 means implicit part routing. Flags are
serialized as 0/1. These are **post-reader normalized score positions**, not XML
line numbers, source byte offsets, or original engraving identities.

Records follow mutation order, not chronological score order. A broken tie
chain can generate multiple affected-note records but count as one chain repair.
A note can be changed more than once; each `before` is its immediate state at
that mutation, so replay of the records explains the repaired note states.
Silencing records the actual discarded owner (which can be the earlier shorter
chain), including removed lyric/chord/tie data.

The receipt explicitly labels reader coverage `aggregate_only` and source
positions unavailable. It does **not** pretend to describe individual grace,
lyric, tempo or MIDI parsing losses yet. Omitted unsupported notation that the
reader does not count is not made visible by this change.

## Persistence and bounds

The receipt is a bounded inert text field embedded in performance format v3,
inside `performances.dawperformance`. No extra sidecar can be silently left
behind by save/collect/recovery of that document. The usual three files remain.
New code reads v1/v2/v3. Documents without receipts retain their v1/v2 save layout;
older binaries cannot open v3. Receipt text is retained byte-for-byte through
history edits, take copies, saves, reopen and performance recovery. Its IDs and
snapshots stay historical after editing; it must not be regenerated from the
current score.

Up to 65,536 audition mutations and 16 MiB of receipt text are accepted. The
reader bounds decoded receipt growth and rejects a missing closing quote or
unsafe terminal control byte. Over-limit generation fails the import rather
than truncating evidence. The persisted field is treated as inert text, not
parsed/applied as JSON on load; the importer emits JSON. Checksums and structure
validation do not certify that externally edited receipt claims are true.

Detailed auditing is opt-in via the third argument to `repairScoreForAudition`.
Existing callers keep their counts/behavior without allocating a note-location
map or detailed snapshots. The CLI enables it; its source and repairs are never
applied to the input file.

## Verification

```sh
ctest --test-dir build --output-on-failure
python3 scripts/check_import_receipt_cli.py
python3 scripts/check_performance_recovery_cli.py
```

The engine regression replays every recorded mutation, verifies multi-segment
tie counts, captures silencing losses, exercises format v3 and legacy saves,
checks truncation/control-byte/size rejection, and retains receipts through
editing and recovery. Independent Python JSON parsing checks the public example,
Unicode filenames/measure labels, quoted multiline lyrics, reader grace counts,
and recovery/legacy behavior. These tests do not load commercial instruments.

Local results: **50/50 CTest**, **4/4 selected ASan/UBSan** and the CLI checks passed.
[Raw failures and final evidence](research/2026-10-02-import-receipt-evidence.txt)
are retained. This work does not rerun the repertoire census or claim new audio validation.
