#pragma once
#include "daw/audio_limits.hpp"
#include "daw/wav.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>

namespace daw {
// Control-thread, device-free collector for a fresh, already-started session
// source (render/frame/done/failed). The same loop serves AU and fake sources.
// Remove common algorithmic latency from the file origin and retain its drain.
// It does not choose plugin mode, change gain or clip float samples.
template <class Source>
AudioBuffer collectSessionBounce(Source &source, std::size_t sequence_frames,
                                 std::uint32_t latency_frames) {
  if (!sequence_frames || sequence_frames > kMaxBufferedAudioFrames || latency_frames > 96000)
    throw std::invalid_argument("ensemble bounce exceeds frame/latency bounds");
  if (source.frame() != 0 || source.done() || source.failed())
    throw std::invalid_argument("ensemble bounce requires a fresh healthy source at frame zero");
  const auto total_frames = sequence_frames + latency_frames;
  AudioBuffer audio;
  audio.sample_rate = 48000;
  audio.channels = 2;
  audio.samples.resize(sequence_frames * 2);
  std::array<float, 512> block{};
  while (!source.done() && source.frame() < total_frames) {
    const auto from = source.frame();
    const auto request =
        static_cast<std::uint32_t>(std::min<std::size_t>(256, total_frames - from));
    source.render(block.data(), request);
    const auto until = source.frame();
    if (source.failed() || until <= from || until > from + request || until > total_frames)
      throw std::runtime_error("ensemble bounce source failed or violated frame progress");
    for (auto f = std::max<std::uint64_t>(from, latency_frames); f < until; ++f) {
      const auto output = static_cast<std::size_t>(f - latency_frames) * 2;
      const auto input = static_cast<std::size_t>(f - from) * 2;
      audio.samples[output] = block[input];
      audio.samples[output + 1] = block[input + 1];
    }
  }
  if (!source.done() || source.frame() != total_frames)
    throw std::runtime_error(
        "ensemble bounce ended before/after the complete source and PDC drain");
  return audio;
}
} // namespace daw
