#pragma once
#include "daw/live_performance.hpp"
#include <algorithm>
#include <stdexcept>

namespace daw {
// Control thread only, BEFORE connecting this stream and renderer to output.
// Replay all preceding MIDI AND audio processing: chasing held keys alone loses
// notes already released under pedal, envelopes and instrument effect tails.
// The consumer must render/discard every block, and throw on renderer failure.
// Cost grows with the target time; this is not a constant-time realtime seek.
template<class Consume>
void prerollPerformance(LivePerformanceStream& stream, std::size_t target, Consume consume) {
  if(target<stream.frame() || target>=stream.endFrame() || stream.hasPendingUpdate())
    throw std::invalid_argument("preroll target outside the idle stream");
  while(stream.frame()<target) {
    const auto count=static_cast<std::uint32_t>(std::min<std::size_t>(256,target-stream.frame()));
    const auto block=stream.nextBlock(count);
    if(stream.failed() || block.frames!=count)throw std::runtime_error("preroll stream failed");
    consume(block);
  }
}
}
