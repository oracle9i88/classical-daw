#pragma once

#include "daw/score.hpp"

#include <string>

namespace daw {

class ScoreHistory;

// Identifies which on-disk candidate supplied a successfully loaded score.
// A recovery load never replaces the primary file; the caller can inspect the
// source and explicitly save the recovered score when it is ready.
enum class ProjectLoadSource {
  None,
  Primary,
  Recovery,
  Temporary,
};

// Persist the editable score state in the versioned line-oriented
// CLASSICAL_DAW_PROJECT format. Scores with explicit measure labels use v8:
// a hex-encoded label follows each number, with the v7 identity fields (zero
// allocator/IDs allowed for an unidentified v8 score). Labels do not set timing.
// Identified scores without labels use version 7 with per-segment
// note IDs and a persisted allocator high water. Legacy/unidentified scores
// still write version 6; assignNoteIds is the explicit upgrade operation.
// Version 6 adds an explicit duration after each
// measure start, including terminal silence. Zero keeps the legacy unspecified
// extent; versions 1..5 load with zero measure durations. A positive duration
// must contain the measure's notes and meet the next measure start, if any.
// Version 5 stores all four MIDI meter fields
// and up to one million later meter changes. Versions 1..4 default the extra
// meter fields to 24 clocks per click and 8 notated 32nds per quarter, with
// no later meter changes. Version 4 added up to one million later tempo
// changes after the authoritative tick-zero BPM. Versions 1/2/3 remain
// readable with no later tempos. Version 3 added note MIDI channel, source
// on/off order, release velocity, and per-part channel events; version 2 added
// hex-encoded lyrics. Earlier versions supply defaults for missing metadata.
// Every field and section is ordered and validated, without
// a third-party parser dependency. MIDI event counts are bounded at one
// million per part and one million across the complete project.
//
// Both functions leave the caller's Score unchanged on failure.  Writing is
// crash-safe at the file replacement boundary: data is flushed to a sibling
// temporary file and then atomically renamed over the destination.
// The check a save performs before it writes anything. Exposed so a read-only
// import check can answer the same question a save would, instead of passing a
// score that only fails once someone tries to keep it.
bool validateScore(const Score& score, std::string* error = nullptr);
bool writeProjectFile(const Score& score, const std::string& path, std::string* error = nullptr);
bool readProjectFile(const std::string& path, Score* score, std::string* error = nullptr);

// Write an autosave/recovery sidecar at `path + ".recovery"`.  The primary
// project is never touched by this operation; the sidecar itself uses the
// same atomic temporary-file replacement as writeProjectFile.
bool writeProjectRecoveryFile(const Score& score, const std::string& path, std::string* error = nullptr);

// Persist the current state of an editable history to the recovery sidecar.
// Only the current Score is written; the in-memory undo/redo stack remains a
// control-thread concern and is rebuilt by constructing or resetting a
// ScoreHistory after a successful recovery load.
bool writeProjectRecoveryFile(const ScoreHistory& history, const std::string& path, std::string* error = nullptr);

// Load the primary project first, then a valid recovery sidecar, then a valid
// interrupted-write temporary file (`.tmp`).  The output score and source
// indicator remain unchanged when every candidate is missing or invalid.
bool readProjectFileWithRecovery(const std::string& path, Score* score,
                                 ProjectLoadSource* source = nullptr, std::string* error = nullptr);

}  // namespace daw
