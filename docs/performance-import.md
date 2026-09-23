# Real-score performance entry — first repair, not import completion

2026-09-24. Work shifted from the second realtime instrument to the missing
file-to-performance path. The earlier eight-bar tests established their own
architecture properties, **not** repertoire coverage. The outside review's
1,105-file figures and Pianoteq run were useful leads, not locally measured
results. The independent census below now supplies local evidence.

## Commands

Build with CMake normally. The import/check tools link the portable engine and
never instantiate AU, initialize Cocoa, or open an audio device.

```sh
build/daw_performance_import --check /path/to/original.musicxml
build/daw_performance_import /path/to/original.musicxml /path/to/piano.aupreset /path/to/new-document
build/daw_performance_play /path/to/new-document
```

Input extensions: `.musicxml` / `.xml`, `.mid` / `.midi`, `.dawproj`.
Compressed `.mxl` is explicitly unsupported. Import assigns missing notation
IDs, builds stored performed-ID/tie mappings, compiles/validates, and saves to a
**new** directory. Source files remain untouched; an existing destination is
rejected. A saved local Pianoteq state is required to create the document, but
its plugin is not loaded/verified by this data operation. `--check` needs no
state. Failures identify `score_read`, `identity_mapping`, `performance_compile`,
`state_read` or `save`. The import does not create default pedal/expression
curves or pretend to interpret all MusicXML directions, repeats and ornaments.
In particular, notation pedal directions are not converted to CC64 here.

Editor commands (only `play` opens a device):

```text
notes 0 20
notes-at "0" 0 20
notes-at "X1" 0 20
curves
curve-put 91 0 64 10 0 0 20 1 127 30 2 0
curve 91 20 80
curve-remove 91
undo
redo
save "/path/to/new-saved-document"
quit
```

`notes [OFFSET COUNT]` defaults to 50 rows, max 200. `notes-at LABEL` filters by
any tied segment's measure label; rows show the anchor's measure/ordinal, part,
staff, voice, spelling/alter/octave, measure-relative ticks, absolute ticks,
notated duration and scheduled attack seconds. A tie's continuation label can
thus find its performed attack. Use printed IDs, not fixture IDs.

`curve-put ID CHANNEL CC` takes two or more `POINT_ID SECONDS VALUE` triples.
Channel is zero-based 0..15; CC is 64 or 11. It creates or replaces a complete
lane, including its point times. Times start at zero, increase strictly and
stay within the compiled musical end. Values are 0..127. Interpolation remains
linear at 10 ms steps; this is not a new pedal interpolation algorithm.
An authored curve **overrides imported MIDI messages on that controller lane**;
removal restores baseline playback. Duplicate lanes/IDs and invalid points
fail. Creation, replacement, point edits, removal, gain and notation/performance
edits share the existing undo stack and live admission rollback. Only the
changed lane's immutable snapshots are retained in its delta command; no new
history stack or full-score snapshots were added. Undo history is not saved.

## XML boundary repairs

- XML declaration/BOM, comments, and a single prolog `score-partwise` DOCTYPE
  with no external ID or a PUBLIC/SYSTEM ID are accepted. The external ID is
  ignored: **there is no DTD resolver, network fetch or file fetch**. Internal
  subsets/entities, unknown entities, malformed/misplaced/duplicate DOCTYPE,
  malformed comments and CDATA remain rejected. Comment contents cannot inject
  apparent notes, parts, entities or declarations into element searches.
- Measure `number` is preserved as a separate label when it differs from the
  internal positive integer. `0`, `X1`, `-1`, `01`, fractional-looking and long
  numeric tokens no longer have to become positive integers. Chronology remains
  vector order and tick positions. Pickup extent continues to use `implicit` and
  note/forward extent, **not** the label. XML export preserves the token.
- Native project v8 stores hex-encoded labels and the v7 identity fields, including
  zero allocator/IDs for an unidentified score. Files without explicit labels
  retain v6/v7 encoding; v1..v7 still load. Older applications cannot read v8.

This follows the primary [MusicXML measure definition](https://www.w3.org/2021/06/musicxml40/musicxml-reference/elements/measure-partwise/)
and [standard prolog example](https://www.w3.org/2021/06/musicxml40/tutorial/hello-world/).
It is not a replacement with a fully conforming XML parser.

## Document-derived verification

```sh
build/daw_performance_probe /path/to/document /path/to/new-evidence --data-only
python3 scripts/check_performance_import.py /path/to/original.musicxml /path/to/piano.aupreset
```

The generic probe derives a performed ID, notation anchor, MIDI channel and
existing velocity from the supplied document. It edits velocity/gain and an
existing curve point, or creates a curve if there are none. Undo restores all
three persisted files byte-for-byte; redo and reopen reproduce the edited
bytes, with unchanged notation bytes and a checked scheduled MIDI velocity.
This is not a claim to test every edit type or isolated onset audibility.

Without `--data-only`, the same probe additionally runs three actual CoreAudio
captures with speakers silenced by default; `--audible-edited` explicitly enables
the edited run. Timeout derives from document duration. Reopen audio retains
the existing 2% normalized RMS / 0.01 peak-delta soft gate, which may fail for
stateful/random plugins. This iteration did **not** rerun that hardware mode.
The previous hardcoded isolated-onset test is retained under the honest name
`daw_performance_fixture_probe`; it only accepts the designed fixture in practice.

The CLI check imports the supplied original, rejects overwrites, queries contextual
notes and measure labels, creates/edits/deletes a lane, interleaves gain edits,
checks invalid-command rollback, undo/redo and exact reopen, then runs the generic
probe. No plugin/device opens. The specialized fixture regression is retained too.

## Actual original-file census

Run, without stripping declarations or editing notes:

```sh
python3 scripts/check_performance_corpus.py /path/to/dcml-musicxml /path/to/new-report.json
```

Local 1,105-file result after the declaration/label repairs:

| First observed terminal stage | Files |
| --- | ---: |
| Score reader rejected | 653 |
| Identity/tie mapping rejected | 45 |
| Performance compiler rejected | 398 |
| Accepted through all three | **9** |

All 1,105 before/after SHA-256 pairs match. This is **acceptance**, not fidelity
or an accuracy score. First-failure counts are dependent on ordering: fixing
one blocker exposes others. They cannot be added to predict a future pass rate.

Largest first-failure reasons: missing duration 477 (all independently confirmed
to encounter a grace note first); one-part performance
restriction 208; same-key overlap/coincident retrigger 189; tempo conflict 121;
multiple lyrics 39. Other failures include tie topology, fractional timing,
metric modulation and synchronized measure boundaries. The full raw per-file
outputs and hashes are [retained here](research/2026-09-24-import-corpus.json).
No source score, plugin state or plugin audio is redistributed.

Original Bartók Op.6 No.3 passes: **391 elements = 387 sounding notation
segments + 4 rests; 383 performed notes** after four tie merges. The generic
probe selects performed ID192 / notation anchor198 without fixture assumptions.
Its original-file hash, exact commands and Debug/ASan/UBSan results are in the
[evidence log](research/2026-09-24-import-entry-evidence.txt). No hearing or new
hardware timing claim follows from these data-only runs.

## Open gates, in priority order

1. **Grace notes:** not implemented. They lack ordinary duration by design;
   [MusicXML grace attributes](https://www.w3.org/2021/06/musicxml40/musicxml-reference/elements/grace/)
   distinguish stealing preceding/following time from making time. Preserve their
   notation identities and metadata, then define/test an explicit realization
   policy. Do not delete them, fake ordinary durations or call a notation-only
   read a performance pass.
2. **Tempo conflicts:** still strict. Introduce source-aware, reported precedence
   before broadening admission. A same-position metronome and sound declaration
   is not an excuse to silently rewrite the piece's tempo.
3. **Same-key polyphony and adjacent retriggers:** still conservatively rejected.
   Distinguish an exact-boundary release/retrigger from a true overlap; test event
   order and ownership before changing admission. LIFO byte pairing alone cannot
   make MIDI 1.0 release two independently addressed voices on one channel/key.
4. Multiple verses and real-world tie topology; then multi-part performance
   documents. The one-part document restriction is **not** the same thing as the
   two-live-AU product cap: existing offline multi-part sessions already work.

The complete importer remains unfinished. These repairs make a real file usable
through the editor and make the remaining failures independently reproducible;
9/1,105 is not a production-ready result.
