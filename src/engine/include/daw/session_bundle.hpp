#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace daw {
struct SessionBundleReport {
  std::size_t tracks = 0, frames = 0;
  // Sum of referenced files, including the session and score. Shared source
  // references count per route; collected bundles have separate per-route files.
  std::uint64_t referenced_bytes = 0;
};
// Control/offline thread only. Validate saved routing, score, state bindings and
// complete frozen CRC/sample data using bounded audio buffers. Requires frozen
// media/state for every part. Does not load plugins or certify state compatibility.
SessionBundleReport verifySessionBundle(const std::string& session_path);

// Collect the SAVED on-disk session into a self-contained NEW directory.
// Exact score/state/frozen bytes are copied; filenames are normalized and mix
// targets retained. WAV/MIDI exports, undo history and recovery sidecars are not
// project dependencies and are not copied. Sources must stay immutable.
// Validate the staged copy, then publish the directory via an exclusive rename
// on macOS/Linux. Unsupported OS/filesystems fail; no racy overwrite fallback.
// Ordinary failures clean only owned staging files; crashes may leave
// NEW_DIRECTORY.collecting. Existing outputs/staging are preserved. No fsync or
// power-loss durability guarantee. No plugin licensing/asset redistribution grant.
SessionBundleReport collectSessionBundle(const std::string& source_session,
                                         const std::string& new_directory);
}
