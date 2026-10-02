#pragma once
#include "daw/audio_output_source.hpp"
#include "daw/live_session.hpp"
#include "daw/parallel_render_graph.hpp"
#include <array>

namespace daw {
// One mailbox, one graph, one monotonic clock. Renderers are borrowed and must
// outlive this object; stop/join output before destruction or reconstruction.
// Serial graph V1 admits at most two live renderers. No hidden freeze/threads.
class SessionRenderSource final : public AudioOutputSource {
 public:
  enum class State { Playing, Draining, Finished, Failed };
  SessionRenderSource(std::vector<LiveTrackPlan> plans,
      std::vector<RenderTrackBinding> bindings, std::uint64_t revision = 0);
  bool acceptsFormat(double rate, std::uint32_t channels) const noexcept override;
  // One engine quantum (1..256), as dispatched by renderOutputBlocks.
  void render(float* out, std::uint32_t frames) noexcept override;
  // Control thread only. Acceptance is host scheduling, NOT successful DSP or
  // audible output. An EOF race can leave a pending ticket: wait cancels it.
  std::uint64_t submit(std::vector<LiveTrackPlan> plans, std::uint64_t revision);
  LiveSessionStream::Receipt waitForDecision(std::uint64_t ticket,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(1000));
  LiveSessionStream::Receipt receipt(std::uint64_t ticket) const { return stream_.receipt(ticket); }
  std::uint64_t appliedRevision() const noexcept { return stream_.appliedRevision(); }
  std::size_t appliedFrame() const noexcept { return stream_.appliedFrame(); }
  std::uint64_t suppressedConflictsAfterStop() const noexcept { return stream_.suppressedConflictsAfterStop(); }
  State state() const noexcept { return state_.load(std::memory_order_acquire); }
  bool done() const noexcept { return state() == State::Finished; }
  bool failed() const noexcept { return state() == State::Failed; }
  std::uint64_t frame() const noexcept { return graph_.frame(); }
  std::uint32_t latencyFrames() const noexcept { return graph_.latencyFrames(); }
  const ParallelRenderGraph& graph() const noexcept { return graph_; }
 private:
  LiveSessionStream stream_;
  ParallelRenderGraph graph_;
  std::array<TrackQuantum, 2> inputs_{};
  std::uint32_t drain_left_ = 0; // audio thread only
  std::atomic<State> state_{State::Playing};
};
}
