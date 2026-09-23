#pragma once

#include "daw/wav.hpp"
#include <string>
#include <memory>
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
// Bounded-memory disk reader. Constructor validates the entire file and CRC
// before exposing samples. Reads/seeks belong on a worker/control thread only.
// Holds the validated file open; caller must keep its contents immutable.
class FrozenTrackReader {
 public:
  static constexpr std::size_t kReadFrames = 8192;
  FrozenTrackReader(const std::string& path, const std::string& identity, std::size_t frames);
  ~FrozenTrackReader();
  FrozenTrackReader(const FrozenTrackReader&) = delete;
  FrozenTrackReader& operator=(const FrozenTrackReader&) = delete;
  std::size_t frameCount() const noexcept;
  // Reads exactly count stereo frames. Bounds checked before writing. A disk
  // failure throws; caller must not publish any partially written block.
  void readFrames(std::size_t start, std::size_t count, float* stereo);
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace daw
