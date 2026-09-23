#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace daw {
// AudioUnit capacity is distinct from the engine's processing quantum. Reserve
// rate-conversion headroom without changing the hardware buffer or sample rate.
inline std::uint32_t outputSliceCapacity(std::uint32_t device_frames, double device_rate,
                                         double client_rate, std::uint32_t quantum) {
  constexpr std::uint32_t limit = 65536;
  if (!device_frames || !quantum || quantum > limit || !std::isfinite(device_rate) ||
      !std::isfinite(client_rate) || device_rate < 8000 || device_rate > 384000 ||
      client_rate < 8000 || client_rate > 384000)
    throw std::invalid_argument("unsupported output buffer/rate configuration");
  const double required = std::ceil(device_frames * std::max(1., client_rate / device_rate)) + quantum;
  if (required > limit) throw std::invalid_argument("output device requires more than 65536 frames per callback");
  return std::max({4096U, quantum, static_cast<std::uint32_t>(required)});
}

// Validated interleaved buffer, split without copying or allocating. Hardware
// callbacks may have any size up to the prepared capacity, not just quantum.
// Balance subdivisions: e.g. 557 frames becomes 186/186/185, not 256/256/45.
// This avoids giving AU fixed call overhead an artificially tiny last quantum,
// without buffering extra samples, adding latency or changing the device size.
template <typename Render>
inline void renderOutputBlocks(float* output, std::uint32_t frames, std::uint32_t channels,
                               std::uint32_t quantum, Render&& render) noexcept {
  if (!output || !channels || !quantum || !frames) return;
  const auto blocks = 1U + (frames - 1U) / quantum;
  const auto base = frames / blocks, extra = frames % blocks;
  for (std::uint32_t block = 0, offset = 0; block < blocks; ++block) {
    const auto count = base + (block < extra ? 1U : 0U);
    render(output + static_cast<std::size_t>(offset) * channels, count);
    offset += count;
  }
}
}
