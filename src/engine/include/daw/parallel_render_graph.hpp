#pragma once

#include "daw/midi_sequence.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace daw {
struct RendererLatency {
  double seconds = 0;
  std::uint64_t generation = 0;
};
enum class TrackRenderError { None, ReturnedError, NonfiniteOutput };

// Minimal prepared audio-lane contract. Setup/state/preset work stays outside
// this interface. Events have offsets relative to this quantum. All renderers
// receive the SAME session frame, even if earlier lanes failed or were muted.
// Caller owns instances until graph/output teardown on the control thread.
class PreparedTrackRenderer {
 public:
  virtual ~PreparedTrackRenderer() = default;
  virtual RendererLatency latency() const = 0; // control only, may fail
  virtual std::uint64_t latencyGeneration() const noexcept = 0; // atomic read
  virtual TrackRenderError render(const TimedMidiEvent* events, std::size_t count,
      std::uint64_t session_frame, float* stereo, std::uint32_t frames) noexcept = 0;
};

struct RenderTrackBinding {
  std::string track_id; // stable document identity, not route index or MIDI channel
  PreparedTrackRenderer* renderer = nullptr;
  std::int64_t track_delay_us = 0; // musical offset, NOT algorithmic latency
};
struct TrackQuantum {
  const TimedMidiEvent* events = nullptr;
  std::size_t count = 0;
  double gain = 1;
  bool audible = true;
};
struct TrackPdcInfo {
  std::string track_id;
  double reported_seconds = 0;
  std::uint32_t algorithmic_frames = 0, compensation_frames = 0;
  std::uint64_t latency_generation = 0;
};
struct TrackRuntimeStatus {
  TrackRenderError error = TrackRenderError::None;
  std::uint64_t failure_frame = 0;
  bool quarantined() const noexcept { return error != TrackRenderError::None; }
};
enum class RenderGraphFault { None, InvalidQuantum, LatencyChanged, NonfiniteMix };

// Parallel direct-to-master PDC kernel, NOT a session scheduler or publication
// mailbox. The caller supplies one coherent whole-session quantum; it must not
// construct that quantum by independently adopting per-track stream revisions.
// Fixed topology/rate/latency for this lifetime; reconstruct after stopping.
class ParallelRenderGraph {
 public:
  static constexpr std::uint32_t quantum_limit = 256;
  static constexpr std::size_t track_limit = 64;
  static constexpr std::uint32_t latency_limit_frames = 96000;
  explicit ParallelRenderGraph(std::vector<RenderTrackBinding> bindings, std::uint32_t rate = 48000);
  ~ParallelRenderGraph();
  ParallelRenderGraph(const ParallelRenderGraph&) = delete;
  ParallelRenderGraph& operator=(const ParallelRenderGraph&) = delete;
  // Audio thread only. Caller supplies 2*frames writable floats. All input
  // validation precedes any renderer call. No allocation, destruction or locks.
  bool render(const TrackQuantum* tracks, std::size_t count, float* output,
              std::uint32_t frames) noexcept;
  std::uint32_t latencyFrames() const noexcept { return total_latency_; }
  std::uint32_t sampleRate() const noexcept { return rate_; }
  const std::vector<TrackPdcInfo>& latencyInfo() const noexcept { return info_; }
  TrackRuntimeStatus trackStatus(std::size_t index) const; // control, pollable
  RenderGraphFault fault() const noexcept { return fault_.load(std::memory_order_acquire); }
  std::uint64_t frame() const noexcept { return published_frame_.load(std::memory_order_acquire); }
  // Formatting is control-only; includes L, ALL determining lanes and faults.
  std::string statusText() const;
 private:
  struct Lane;
  bool generationsMatch() const noexcept;
  std::vector<std::unique_ptr<Lane>> lanes_;
  std::vector<TrackPdcInfo> info_;
  std::uint32_t rate_ = 48000, total_latency_ = 0;
  std::uint64_t position_ = 0;
  std::atomic<std::uint64_t> published_frame_{0};
  std::atomic<RenderGraphFault> fault_{RenderGraphFault::None};
};
}
