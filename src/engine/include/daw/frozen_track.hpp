#pragma once

#include "daw/wav.hpp"
#include <string>
#include <vector>

namespace daw {
// Exact source binding, not a hash: conservative invalidation on any score-file
// byte change. Mixing settings and filenames are intentionally not included.
std::string frozenTrackIdentity(const std::string& score_bytes, const std::string& part_id,
                               const std::string& instrument, const std::vector<std::uint8_t>& state);
struct FrozenTrack {
  AudioBuffer audio;
  std::string preset;
  std::uint32_t component_version = 0;
};
// Internal DAWFRZ01 format: little-endian IEEE float32, fixed stereo 48 kHz,
// exact source identity and CRC32 for accidental corruption (not authentication).
// 32 Mi frames maximum. Reject non-finite samples, bad sizes, symlinks, stale
// identity, unexpected frame count, checksum mismatch, truncation and trailing data.
// Writers require a caller-owned new path; bundle cleanup belongs to the caller.
void writeFrozenTrack(const AudioBuffer& audio, const std::string& identity,
                      const std::string& preset, std::uint32_t component_version,
                      const std::string& path);
FrozenTrack readFrozenTrack(const std::string& path, const std::string& identity,
                           std::size_t expected_frames);
}  // namespace daw
