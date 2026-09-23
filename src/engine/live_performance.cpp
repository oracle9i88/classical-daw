#include "daw/live_performance.hpp"
namespace daw {
namespace {
std::vector<LiveTrackPlan> single(MidiSampleSequence sequence,double gain,
    const std::vector<std::uint64_t>& guards={}) {
  std::vector<LiveTrackPlan> tracks;
  tracks.push_back({"single",std::move(sequence),gain,guards}); return tracks;
}
}
LivePerformanceStream::LivePerformanceStream(MidiSampleSequence sequence,double gain,std::uint64_t revision)
    : session_(single(std::move(sequence),gain),revision) {}
LivePerformanceStream::~LivePerformanceStream()=default;
std::uint64_t LivePerformanceStream::submit(MidiSampleSequence sequence,double gain,std::uint64_t revision,
    const std::vector<std::uint64_t>& guards) { return session_.submit(single(std::move(sequence),gain,guards),revision); }
LivePerformanceStream::Receipt LivePerformanceStream::receipt(std::uint64_t ticket) const { return session_.receipt(ticket); }
bool LivePerformanceStream::cancelPending(std::uint64_t ticket) { return session_.cancelPending(ticket); }
LivePerformanceStream::Receipt LivePerformanceStream::waitForDecision(std::uint64_t ticket,std::chrono::milliseconds timeout) {
  return session_.waitForDecision(ticket,timeout);
}
void LivePerformanceStream::collectRetired() noexcept { session_.collectRetired(); }
LivePerformanceStream::Block LivePerformanceStream::nextBlock(std::uint32_t frames) noexcept {
  const auto block=session_.nextBlock(frames);
  return block.count ? block.tracks[0] : Block{};
}
}
