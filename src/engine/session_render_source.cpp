#include "daw/session_render_source.hpp"
#include <algorithm>
#include <stdexcept>

namespace daw {
namespace {
std::vector<LiveTrackPlan> validate(std::vector<LiveTrackPlan> plans,
                                  const std::vector<RenderTrackBinding>& bindings) {
  if (plans.empty() || plans.size() > 2 || plans.size() != bindings.size())
    throw std::invalid_argument("session runtime requires one or two live instruments; freeze other tracks explicitly");
  for (std::size_t i=0; i<plans.size(); ++i)
    if (plans[i].track_id != bindings[i].track_id || plans[i].track_delay_us != bindings[i].track_delay_us)
      throw std::invalid_argument("session plan and renderer route order/offset must match");
  return plans;
}
}
SessionRenderSource::SessionRenderSource(std::vector<LiveTrackPlan> plans,
    std::vector<RenderTrackBinding> bindings, std::uint64_t revision)
    : stream_(validate(std::move(plans), bindings), revision), graph_(std::move(bindings)) {
  static_assert(std::atomic<State>::is_always_lock_free);
}
bool SessionRenderSource::acceptsFormat(double rate, std::uint32_t channels) const noexcept {
  return rate == 48000 && channels == 2;
}
std::uint64_t SessionRenderSource::submit(std::vector<LiveTrackPlan> plans, std::uint64_t revision) {
  if (state() != State::Playing) throw std::logic_error("transport ended or is draining; stop and rebuild before editing playback");
  return stream_.submit(std::move(plans), revision);
}
LiveSessionStream::Receipt SessionRenderSource::waitForDecision(std::uint64_t ticket, std::chrono::milliseconds timeout) {
  // A publication may race the last quantum; no callback will adopt it after
  // draining starts. Cancel now where possible, or let the bounded wait cancel.
  if (state() != State::Playing) stream_.cancelPending(ticket);
  return stream_.waitForDecision(ticket, timeout);
}
void SessionRenderSource::render(float* out, std::uint32_t frames) noexcept {
  if (!out || !frames || frames > 256) { state_.store(State::Failed, std::memory_order_release); return; }
  std::fill_n(out, frames*2, 0.F);
  auto current = state();
  if (current == State::Finished || current == State::Failed) return;
  std::uint32_t used = 0;
  if (current == State::Playing) {
    const auto block = stream_.nextBlock(frames);
    if (stream_.failed() || block.count != stream_.trackCount() || block.frame != graph_.frame()) {
      state_.store(State::Failed, std::memory_order_release); return;
    }
    for (std::size_t i=0; i<block.count; ++i) {
      const auto& track=block.tracks[i];
      if (track.revision != block.revision || track.frame != block.frame || track.frames != block.frames) {
        state_.store(State::Failed, std::memory_order_release); return;
      }
      inputs_[i]={track.events,track.count,track.gain,track.audible,track.balance};
    }
    if (block.frames && !graph_.render(inputs_.data(), block.count, out, block.frames)) {
      state_.store(State::Failed, std::memory_order_release); return;
    }
    used=block.frames;
    if (stream_.frame() < stream_.endFrame()) return;
    // MIDI sequence already contains terminal resets and its explicit release
    // tail. Flush another L frames through BOTH plugins and host delay lines.
    drain_left_=graph_.latencyFrames();
    for (auto& input:inputs_) { input.events=nullptr; input.count=0; }
    state_.store(State::Draining, std::memory_order_release);
  }
  const auto n=std::min(frames-used,drain_left_);
  if (n && !graph_.render(inputs_.data(),stream_.trackCount(),out+used*2,n)) {
    std::fill_n(out, frames*2, 0.F);
    state_.store(State::Failed,std::memory_order_release); return;
  }
  drain_left_-=n;
  if (!drain_left_) state_.store(State::Finished,std::memory_order_release);
}
}
