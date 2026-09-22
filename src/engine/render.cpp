#include "daw/render.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace daw {

AudioBuffer renderNotes(const MidiTrack& track, const TempoMap& tempo, double sample_rate, double tail_seconds) {
  if (!std::isfinite(sample_rate) || sample_rate <= 0.0) throw std::invalid_argument("sample rate must be positive");
  if (!std::isfinite(tail_seconds) || tail_seconds < 0.0) throw std::invalid_argument("tail must be non-negative");

  Tick end_tick = 0;
  for (const MidiNote& note : track.notes) end_tick = std::max(end_tick, note.end());
  const double duration = std::max(0.0, tempo.tickToSeconds(end_tick)) + tail_seconds;
  const auto frame_count = static_cast<std::size_t>(std::ceil(duration * sample_rate));
  AudioBuffer output;
  output.sample_rate = static_cast<std::uint32_t>(std::llround(sample_rate));
  output.channels = 1;
  output.samples.assign(frame_count, 0.0f);

  constexpr double two_pi = 6.283185307179586476925286766559;
  for (const MidiNote& note : track.notes) {
    if (note.pitch > 127 || note.velocity == 0 || note.duration <= 0) continue;
    const auto start_frame = static_cast<std::size_t>(std::max<SampleIndex>(0, std::llround(tempo.tickToSeconds(note.start) * sample_rate)));
    const auto end_frame = static_cast<std::size_t>(std::min<SampleIndex>(static_cast<SampleIndex>(frame_count),
                                                                           std::llround(tempo.tickToSeconds(note.end()) * sample_rate)));
    if (start_frame >= end_frame || start_frame >= frame_count) continue;
    const double frequency = 440.0 * std::pow(2.0, (static_cast<int>(note.pitch) - 69) / 12.0);
    const double amplitude = 0.18 * static_cast<double>(note.velocity) / 127.0;
    const double note_frames = static_cast<double>(end_frame - start_frame);
    const double attack_frames = std::max(1.0, sample_rate * 0.008);
    const double release_frames = std::max(1.0, sample_rate * 0.035);
    for (std::size_t frame = start_frame; frame < end_frame; ++frame) {
      const double age = static_cast<double>(frame - start_frame);
      const double remaining = note_frames - age;
      const double attack = std::min(1.0, age / attack_frames);
      const double release = std::min(1.0, remaining / release_frames);
      const double envelope = std::max(0.0, std::min(attack, release));
      const double phase = two_pi * frequency * age / sample_rate;
      output.samples[frame] += static_cast<float>(amplitude * envelope * std::sin(phase));
    }
  }
  for (float& sample : output.samples) sample = std::clamp(sample, -1.0f, 1.0f);
  return output;
}

}  // namespace daw
