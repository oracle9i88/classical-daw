#pragma once
#include "daw/frozen_track.hpp"
#include <memory>
#include <vector>

namespace daw {
// Four fixed pages for all tracks, one disk worker, one render consumer.
// Construct/destruct with rendering stopped. Construction validates readers
// and preloads page zero; destruction joins the worker outside the callback.
// File contents must remain immutable until destruction.
class StreamingAudio {
 public:
  static constexpr std::size_t kPageFrames = 4096, kPages = 4, kMaxTracks = 64;
  explicit StreamingAudio(std::vector<std::unique_ptr<FrozenTrackReader>> readers);
  ~StreamingAudio();
  StreamingAudio(const StreamingAudio&) = delete;
  StreamingAudio& operator=(const StreamingAudio&) = delete;
  std::size_t trackCount() const noexcept;
  std::size_t frameCount() const noexcept;
  std::size_t bufferBytes() const noexcept;
  // Render-consumer only. Bounded scan, no I/O/allocation/wait. Returned frame
  // contains interleaved L/R for every track. Valid until the next frame()
  // call requesting another page. Null = not ready or EOF; never partial tracks.
  // The same consumer may request paused/seeking positions for prefetch.
  const float* frame(std::size_t position) noexcept;
  bool failed() const noexcept;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
