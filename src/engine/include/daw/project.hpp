#pragma once

#include "daw/score.hpp"

#include <string>

namespace daw {

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
// CLASSICAL_DAW_PROJECT format. Version 2 stores one hex-encoded lyric per
// note; the reader remains compatible with version 1 files. The format
// deliberately has no third-party
// parser dependency: every field and section is ordered and validated.
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

// Load the primary project first, then a valid recovery sidecar, then a valid
// interrupted-write temporary file (`.tmp`).  The output score and source
// indicator remain unchanged when every candidate is missing or invalid.
bool readProjectFileWithRecovery(const std::string& path, Score* score,
                                 ProjectLoadSource* source = nullptr, std::string* error = nullptr);

}  // namespace daw
