#include "daw/parallel_render_graph.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>

namespace daw {
struct ParallelRenderGraph::Lane {
  PreparedTrackRenderer* renderer = nullptr;
  std::vector<float> delay;
  std::size_t head = 0;
  std::array<float, quantum_limit * 2> scratch{};
  std::atomic<TrackRenderError> error{TrackRenderError::None};
  // Written once before publishing the latched error; acquire that error first.
  std::uint64_t failure_frame = 0;
  float delayed(float sample) noexcept {
    if (delay.empty()) return sample;
    const float old = delay[head]; delay[head] = sample;
    if (++head == delay.size()) head = 0;
    return old;
  }
};
ParallelRenderGraph::ParallelRenderGraph(std::vector<RenderTrackBinding> bindings, std::uint32_t rate) : rate_(rate) {
  static_assert(std::atomic<TrackRenderError>::is_always_lock_free);
  static_assert(std::atomic<RenderGraphFault>::is_always_lock_free);
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
  if (rate != 48000 || bindings.empty() || bindings.size() > track_limit)
    throw std::invalid_argument("PDC requires 48000 Hz and 1..64 lanes");
  std::set<std::string> ids;
  std::set<PreparedTrackRenderer*> renderers;
  for (const auto& binding : bindings) {
    if (binding.track_id.empty() || binding.track_id.size() > 1024 || !ids.insert(binding.track_id).second ||
        !binding.renderer || !renderers.insert(binding.renderer).second)
      throw std::invalid_argument("PDC requires distinct stable track IDs and renderer instances");
    if (binding.track_delay_us != 0)
      throw std::invalid_argument("nonzero musical track_delay_us playback is not implemented");
    const auto before = binding.renderer->latencyGeneration();
    const auto report = binding.renderer->latency();
    if (before != report.generation || before != binding.renderer->latencyGeneration())
      throw std::invalid_argument("renderer latency changed during graph preparation");
    const double frames = report.seconds * rate;
    if (!std::isfinite(frames) || frames < 0 || frames > latency_limit_frames ||
        std::abs(frames - std::round(frames)) > 1e-6)
      throw std::invalid_argument("unsupported fractional or out-of-range renderer latency");
    const auto integral = static_cast<std::uint32_t>(std::round(frames));
    total_latency_ = std::max(total_latency_, integral);
    info_.push_back({binding.track_id, report.seconds, integral, 0, report.generation});
    auto lane = std::make_unique<Lane>(); lane->renderer = binding.renderer;
    lanes_.push_back(std::move(lane));
  }
  for (std::size_t i = 0; i < lanes_.size(); ++i) {
    info_[i].compensation_frames = total_latency_ - info_[i].algorithmic_frames;
    lanes_[i]->delay.resize(static_cast<std::size_t>(info_[i].compensation_frames) * 2, 0);
  }
  if (!generationsMatch()) throw std::invalid_argument("renderer latency changed while allocating PDC");
}
ParallelRenderGraph::~ParallelRenderGraph() = default;
bool ParallelRenderGraph::generationsMatch() const noexcept {
  for (std::size_t i = 0; i < lanes_.size(); ++i)
    if (lanes_[i]->error.load(std::memory_order_relaxed) == TrackRenderError::None &&
        lanes_[i]->renderer->latencyGeneration() != info_[i].latency_generation) return false;
  return true;
}
bool ParallelRenderGraph::render(const TrackQuantum* tracks, std::size_t count, float* output, std::uint32_t frames) noexcept {
  // Invalid oversized calls must not walk an untrusted output extent.
  if (!output || !frames || frames > quantum_limit) {
    fault_.store(RenderGraphFault::InvalidQuantum, std::memory_order_release); return false;
  }
  std::fill_n(output, frames * 2, 0.0F);
  if (fault() != RenderGraphFault::None) return false;
  auto invalid = [&] { fault_.store(RenderGraphFault::InvalidQuantum, std::memory_order_release); return false; };
  if (!tracks || count != lanes_.size()) return invalid();
  for (std::size_t i = 0; i < count; ++i) {
    const auto& t = tracks[i];
    if (!std::isfinite(t.gain) || t.gain < 0 || t.gain > 4 || t.count > 16384 || (t.count && !t.events)) return invalid();
    std::size_t prior = 0;
    for (std::size_t j = 0; j < t.count; ++j) {
      const auto& e = t.events[j]; const auto type = e.status & 0xf0;
      if (e.frame >= frames || e.frame < prior || type < 0x80 || type > 0xe0 ||
          e.data1 > 127 || e.data2 > 127 || ((type == 0xc0 || type == 0xd0) && e.data2)) return invalid();
      prior = e.frame;
    }
  }
  if (!generationsMatch()) {
    fault_.store(RenderGraphFault::LatencyChanged, std::memory_order_release); return false;
  }
  for (std::size_t i = 0; i < count; ++i) {
    auto& lane = *lanes_[i]; const auto& track = tracks[i];
    if (lane.error.load(std::memory_order_relaxed) != TrackRenderError::None) continue;
    std::fill_n(lane.scratch.data(), frames * 2, 0.0F);
    auto error = lane.renderer->render(track.events, track.count, position_, lane.scratch.data(), frames);
    if (error == TrackRenderError::None)
      for (std::size_t j = 0; j < frames * 2; ++j)
        if (!std::isfinite(lane.scratch[j])) { error = TrackRenderError::NonfiniteOutput; break; }
    if (error != TrackRenderError::None) {
      lane.failure_frame = position_;
      lane.error.store(error, std::memory_order_release);
      // Quarantine permanently bypasses this delay (including already buffered
      // samples). No large clear, plugin call or destructor on the audio thread.
      continue;
    }
    for (std::size_t j = 0; j < frames * 2; ++j) {
      const auto sample = lane.delayed(lane.scratch[j]); // advance even while muted
      if (track.audible) output[j] += static_cast<float>(sample * track.gain);
    }
  }
  if (!generationsMatch()) {
    fault_.store(RenderGraphFault::LatencyChanged, std::memory_order_release);
    std::fill_n(output, frames * 2, 0.0F); return false;
  }
  for (std::size_t i = 0; i < frames * 2; ++i) if (!std::isfinite(output[i])) {
    fault_.store(RenderGraphFault::NonfiniteMix, std::memory_order_release);
    std::fill_n(output, frames * 2, 0.0F); return false;
  }
  position_ += frames; published_frame_.store(position_, std::memory_order_release); return true;
}
TrackRuntimeStatus ParallelRenderGraph::trackStatus(std::size_t index) const {
  const auto& lane = *lanes_.at(index);
  const auto error = lane.error.load(std::memory_order_acquire);
  return {error, error == TrackRenderError::None ? 0 : lane.failure_frame};
}
std::string ParallelRenderGraph::statusText() const {
  std::ostringstream out;
  out << std::setprecision(17) << "graph_latency_frames=" << total_latency_
      << " graph_latency_seconds=" << static_cast<double>(total_latency_) / rate_
      << " graph_fault=" << static_cast<int>(fault()) << '\n';
  for (std::size_t i = 0; i < info_.size(); ++i) {
    const auto& p = info_[i]; const auto status = trackStatus(i);
    out << "track=" << std::quoted(p.track_id) << " reported_seconds=" << p.reported_seconds
        << " algorithmic_frames=" << p.algorithmic_frames << " pdc_frames=" << p.compensation_frames
        << " determines_graph_latency=" << (p.algorithmic_frames == total_latency_)
        << " quarantined=" << status.quarantined() << " render_error=" << static_cast<int>(status.error)
        << " failure_frame=" << status.failure_frame << '\n';
  }
  return out.str();
}
}
