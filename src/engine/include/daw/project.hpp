#pragma once

#include "daw/score.hpp"

#include <string>

namespace daw {

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

}  // namespace daw
