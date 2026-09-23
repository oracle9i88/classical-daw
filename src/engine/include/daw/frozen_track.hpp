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
// Whole-buffer read/write: 32 Mi frames maximum. Reject non-finite samples,
// bad sizes, symlinks, stale identity, unexpected frame count, checksum mismatch, truncation and trailing data.
// Writers require a caller-owned new path; bundle cleanup belongs to the caller.
void writeFrozenTrack(const AudioBuffer& audio, const std::string& identity,
                      const std::string& preset, std::uint32_t component_version,
                      const std::string& path);
FrozenTrack readFrozenTrack(const std::string& path, const std::string& identity,
                           std::size_t expected_frames);
// Bounded-memory disk reader. Constructor validates the entire file and CRC
// before exposing samples. Reads/seeks belong on a worker/control thread only.
// Holds the validated file open; caller must keep its contents immutable.
// Supports up to two hours at 48 kHz, including tail, without loading the whole waveform.
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
// Incremental DAWFRZ01 writer for control/worker threads. Stereo 48 kHz,
// up to two hours including tail; chunks at most 8192 frames. Does not allocate
// an entire waveform. finish() requires the exact declared frame count, writes
// the checksum and publishes to a NEW path via an exclusive hard link.
// Destruction before finish removes only this writer's staging; source/output
// files are never overwritten. A process crash may leave .writing staging.
// Disk I/O failure poisons the writer; no partial file is published. No fsync or
// power-loss durability is claimed. Existing buffer APIs keep their old limits.
class FrozenTrackWriter {
 public:
  FrozenTrackWriter(const std::string& path, const std::string& identity,
                    const std::string& preset, std::uint32_t version, std::size_t frames);
  ~FrozenTrackWriter();
  FrozenTrackWriter(const FrozenTrackWriter&) = delete;
  FrozenTrackWriter& operator=(const FrozenTrackWriter&) = delete;
  void appendFrames(const float* stereo, std::size_t count);
  void finish();
  std::size_t writtenFrames() const noexcept;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace daw
