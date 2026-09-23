# Native performance editing: the first complete internal slice

The acceptance target is one eight-bar piano document with a cross-bar tie,
CC64 pedal and editable CC11 expression curves. Written notation and performed
sound have separate identities and persistence. One editor history contains
notation, performed timing/velocity, curve point and gain edits in occurrence
order. Pianoteq generates audio from CoreAudio callbacks, not from a WAV cache.

This records the first gate at commit `49e5ac7`. The following
[live revision gate](live-performance.md) replaces restart-on-edit with an
uninterrupted AU run; its current semantics and evidence take precedence for
the interactive player.

## Use

Build normally on macOS with an installed/licensed Pianoteq 9 AU. A valid saved
Pianoteq state is required; plugin binaries and user states are not distributed.

```sh
build/daw_performance_fixture /path/to/piano.aupreset /path/to/new-eight-bars
build/daw_performance_play /path/to/new-eight-bars
```

Interactive commands (indices of performances start at zero):

```text
notes
play
edit 1 120 0.94 80
curve 2 2 105
pitch 2 F 0 4
undo
undo
undo
redo
gain -15
save "/path/to/new-saved-document"
stop
quit
```

`edit` takes a **performed-note ID**, onset offset in milliseconds, duration
scale and velocity (`-1` inherits score velocity). `pitch` takes a **notation
segment ID**, letter, alteration and octave. `curve` takes a curve ID, point ID
and MIDI value 0..127. `notes` shows the explicit correspondence. `take 0` and
`take 1` select the two stored performances; selection is not a history command.
`status` reports the editor revision and playback frame. Save requires a new
directory and preserves the original document.

The first gate restarted audition on valid edits. The current player publishes
ready immutable plans to the next engine quantum using the same AU and sample
clock. Invalid/busy submissions preserve the document, history and playing run.
Take/instrument-state changes require stopped playback. Stop/dispose and state
restoration still happen only on the control thread. Device faults stop playback; the
accepted document remains saveable. The CLI uses a bounded input buffer and
polls output health while awaiting input. EOF exits; newline-terminated commands
are the supported scripting interface.

## Data and command boundary

- `ScoreNote::id` identifies a notation segment. `Score::next_note_id` stores
  its allocator high water. Identified scores write project **v7**; v1..v6 stay
  readable and unidentified scores still write v6. `assignNoteIds` is an
  explicit migration, not a rewrite of old source files. Duplicate IDs, zero
  IDs in identified scores, allocator collisions and exhaustion are rejected.
- Each performance owns separate performed-note IDs and an explicit mapping to
  notation IDs. In the fixture, notation IDs 8 and 9 map to one performed ID 8;
  performed ID 9 then maps to notation ID 10. Tie onset/duration edits affect
  one attack while leaving the written segments unchanged.
- The mapping representation permits a relation, but this first compiler only
  accepts the validated ordinary-note/tie correspondences. Trill/tremolo and
  other one-to-many performance expansion are future work, not inferred by
  a parent pointer or claimed implemented.
- Performed onset offsets are seconds after tempo conversion; duration scale
  applies to audible duration, not notated ticks. The score's written position,
  duration, lyrics and identity do not change. Existing imported performance
  data in Score remains a baseline; migration of all legacy performance fields
  out of Score is not complete.
- Curves have persistent IDs and point IDs, seconds, values and a MIDI lane.
  The current slice supports CC64 and CC11 with linear interpolation sampled
  every 10 ms, rounded to MIDI values; duplicate repeated values are suppressed.
  Each lane initializes at zero and overrides baseline events on that lane.
  Final pedal/all-notes-off resets remain after the performance. Curves are
  stored and edited as curves, not as thousands of serialized MIDI messages.
- `WorkEditor` owns one bounded stack of fixed-size delta commands for pitch,
  performed note, curve point and gain. It does not instantiate `ScoreHistory`
  or `SessionMixState`. History storage is preallocated; successful application
  cannot fail while recording a command. Validation may compile the score or
  copy the affected small performance, but it never clones every score/history
  snapshot on each edit. Undo/redo restore the affected take, new edits discard
  redo, and invalid commands do not advance the revision.

Legacy `ScoreHistory` and session-mix editing still exist in their old tools.
This iteration does **not** silently claim their global unification. The new
workflow owns notation/performance/mix edits in one stack; migrating older UI
and multi-track session commands to it remains work. Undo stacks and editor
revision counters are in-memory; reopen starts a new history baseline.

The directory contains `score.dawproj`, `piano.aupreset` and
`performances.dawperformance`. The last entry contains versioned, bounded data
plus an exact score/state byte binding. It is written last; incomplete/corrupt
saves are rejected. Successful reopen/save reproduces all three files byte for
byte. This prototype duplicates the bound source bytes in its entry; it is not
block persistence, full crash recovery, power-loss durability or the future
production container format. Source dependencies must remain immutable while
loading. Existing destination directories are never overwritten.

## Actual realtime path

`TimedMidiEvent` carries sample position and complete channel-voice bytes plus
native identity metadata. It is now the `InstrumentRenderer` contract and
scheduler queue type; old `VoiceEvent` calls only remain as a note-only adapter.
Tests exercise CC64/CC11, bend and pressure transport. The diagnostic sine
implementation deliberately ignores controllers; the AU audition does not use
that sine path.

A compiled immutable performance is delivered in <=256-frame blocks to the AU
with intra-block sample offsets. `prepareRealtime` sets offline mode to zero,
prepares stereo 48 kHz and reads plugin latency before callbacks. The callback
uses fixed buffers and has no host allocation, file I/O, property queries or
locks. AU failures latch and produce silence. Third-party internal allocations
are not certified. Only Pianoteq realtime hosting has been validated here;
SWAM realtime support, multi-instrument delay compensation, CoreMIDI recording,
arbitrary plugin voice-parameter editing and graphical editing remain outside.
The measured Pianoteq latency was zero, so no alignment correction was required.

Limits: one piano part, 4096 attacks, 16 performances, 32 curves, 4096 points per
curve, 30 minutes including tail, -60..0 dB audition gain. Ambiguous overlapping
or coincident retriggers on the same MIDI channel/pitch are rejected. Cross-part
routing, curve-time editing and curve insertion/deletion commands are future
extensions. No MusicXML/MIDI round-trip guarantee is made for these identities;
SMF/XML exports remain musical interchange and lose native identity metadata.

## Gates and evidence, macOS 2026-09-23

The realtime spike was run **before proceeding with the revised model**:

```sh
build/daw_au_realtime_probe /path/to/piano.aupreset
```

One real Pianoteq instance received 690 messages (notes, releases, CC64 and CC11)
under actual CoreAudio callbacks for 30 seconds. Result: 1,443,909 frames,
callback errors **0**, measured callback deadline misses **0**, worst callback
1.20517 ms, plugin-reported latency **0 s**. Samples were measured before being
silenced at speaker output. Callback errors and measured processing deadlines
are observations from this run, not a claim to detect every possible OS glitch.
This was a **one-key-at-a-time** workload, not a polyphony benchmark; its margin
does not establish capacity for pedal-held chords or an orchestra. The later
live gate adds a ten-key pedal workload and a strict <50% per-quantum budget.

The vertical acceptance is opt-in, never part of CI/CTest:

```sh
build/daw_performance_tests
python3 scripts/check_performance_cli.py /path/to/eight-bars
build/daw_performance_probe /path/to/eight-bars /path/to/new-evidence
# Optional actual speaker output for the edited version:
build/daw_performance_probe /path/to/eight-bars /path/to/another-new-evidence --audible-edited
```

The probe plays baseline, edited and reopened data through the real callback,
and captures those callback samples only as evidence. The edited version was
sent to the current device during local acceptance. Three runs each produced
1,008,000 frames and zero output callback errors. The onset was moved +120 ms;
early-region RMS changed from 0.0154885 to 0.0000528864. Undo/redo interleaved
notation, note-performance and curve changes in a single stack.

Hard gate: saved/reopened score, performance/mapping/curves and state bytes are
identical. Performance-only edits keep score bytes identical. Soft audio gate,
declared before measurement: normalized RMS error <=2%, max sample difference
<=0.01 full scale. Measured edited-vs-reopened error **0.0700089%**, maximum
sample difference **0.000169208**. This measures the installed preset, not
universal plugin determinism or subjective musical quality.

The full normal and ASan/UBSan suites each passed **41/41**, including the final
history/error-path adjustments; the interactive CLI checker passed in both
builds. A subsequent indexed mapping lookup replaced a quadratic scan and was
checked with the performance suite and CLI checker in both builds. No GitHub
workflow was started or retried.

## Review decisions kept on record

The next acceptance priority remains this internal creative workflow, before
further peripheral feature expansion. Instrument manifests, key/transposing
instrument models, continuous tempo curves, block persistence, XML/MIDI identity
round-trips, licensing changes and CI changes are deferred.

The repository retains AGPL-3.0-or-later. Commit authorship alone is not proof of
exclusive copyright ownership; no claim that all rights are vested in one
person is made from git metadata. New contributions and borrowed assets need
recorded provenance/permissions before any future licensing decision. This
iteration adds original project code and local tests; proprietary plugins and
saved local presets remain outside the published repository.

[GitHub's current billing documentation](https://docs.github.com/en/billing/concepts/product-billing/github-actions)
says public repositories using standard hosted runners have free runtime;
larger runners and storage have separate billing rules. Thus macOS does not
inherently imply paid runtime. This read-only verification does not change the
user's no-retry policy; CI configuration and execution were left untouched.
