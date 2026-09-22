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
// CLASSICAL_DAW_PROJECT format. Version 4 stores up to one million later
// tempo changes after the authoritative tick-zero BPM. Versions 1/2/3 remain
// readable with no later changes. Version 3 added note MIDI channel, source
// on/off order, release velocity, and per-part channel events; version 2 added
// hex-encoded lyrics. Earlier versions supply defaults for missing metadata.
// Every field and section is ordered and validated, without
// a third-party parser dependency. MIDI event counts are bounded at one
// million per part and one million across the complete project.
//
// Both functions leave the caller's Score unchanged on failure.  Writing is
// crash-safe at the file replacement boundary: data is flushed to a sibling
// temporary file and then atomically renamed over the destination.
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
