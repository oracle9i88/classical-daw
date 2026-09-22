#include "daw/render.hpp"

#include "daw/score_midi.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <stdexcept>

namespace daw {

AudioBuffer renderNotes(const MidiTrack& track, const TempoMap& tempo, double sample_rate, double tail_seconds) {
  if (!std::isfinite(sample_rate) || sample_rate <= 0.0) throw std::invalid_argument("sample rate must be positive");
  if (!std::isfinite(tail_seconds) || tail_seconds < 0.0) throw std::invalid_argument("tail must be non-negative");
  if (sample_rate > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
    throw std::invalid_argument("sample rate exceeds WAV format range");
  }
  const auto rounded_sample_rate = std::llround(sample_rate);
  if (rounded_sample_rate <= 0) throw std::invalid_argument("sample rate rounds to zero");

  Tick end_tick = 0;
  for (const MidiNote& note : track.notes) {
    if (note.start < 0 || note.duration <= 0 ||
        note.start > std::numeric_limits<Tick>::max() - note.duration) {
      throw std::invalid_argument("MIDI note timing is negative, empty, or overflowing");
    }
    end_tick = std::max(end_tick, note.end());
  }
  const double note_seconds = std::max(0.0, tempo.tickToSeconds(end_tick));
  const double duration = note_seconds + tail_seconds;
  if (!std::isfinite(duration)) throw std::length_error("render duration is not finite");
  const double frame_count_real = std::ceil(duration * sample_rate);
  if (!std::isfinite(frame_count_real) || frame_count_real < 0.0 ||
      frame_count_real >= static_cast<double>(std::numeric_limits<std::size_t>::max())) {
    throw std::length_error("render output is too large");
  }
  const auto frame_count = static_cast<std::size_t>(frame_count_real);
  AudioBuffer output;
  output.sample_rate = static_cast<std::uint32_t>(rounded_sample_rate);
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

AudioBuffer renderScore(const Score& score, double sample_rate, double tail_seconds) {
  // Validate the render configuration before converting the score so all
  // failures are reported consistently and no caller-owned state is touched.
  if (!std::isfinite(sample_rate) || sample_rate <= 0.0) {
    throw std::invalid_argument("sample rate must be positive");
  }
  if (!std::isfinite(tail_seconds) || tail_seconds < 0.0) {
    throw std::invalid_argument("tail must be non-negative");
  }
  if (sample_rate > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
    throw std::invalid_argument("sample rate exceeds WAV format range");
  }
  const auto rounded_sample_rate = std::llround(sample_rate);
  if (rounded_sample_rate <= 0) throw std::invalid_argument("sample rate rounds to zero");

  MidiFile midi;
  std::string conversion_error;
  if (!scoreToMidiFile(score, &midi, &conversion_error)) {
    throw std::invalid_argument("score render conversion failed: " + conversion_error);
  }

  // Render each part independently. This keeps renderNotes' deterministic
  // envelope behavior while preserving overlapping parts, voices, and chords
  // in the final diagnostic mix.
  std::vector<AudioBuffer> parts;
  parts.reserve(midi.tracks.size());
  const auto output_sample_rate = static_cast<std::uint32_t>(rounded_sample_rate);
  std::size_t frame_count = 0;
  for (const MidiTrack& track : midi.tracks) {
    AudioBuffer rendered = renderNotes(track, midi.tempo, sample_rate, tail_seconds);
    if (rendered.channels != 1 || rendered.sample_rate != output_sample_rate) {
      throw std::logic_error("score render produced an incompatible part buffer");
    }
    frame_count = std::max(frame_count, rendered.frameCount());
    parts.push_back(std::move(rendered));
  }

  AudioBuffer mixed;
  mixed.sample_rate = output_sample_rate;
  mixed.channels = 1;
  mixed.samples.assign(frame_count, 0.0F);
  for (const AudioBuffer& part : parts) {
    const std::size_t frames = std::min(frame_count, part.frameCount());
    for (std::size_t frame = 0; frame < frames; ++frame) {
      mixed.samples[frame] += part.samples[frame];
    }
  }
  for (float& sample : mixed.samples) sample = std::clamp(sample, -1.0F, 1.0F);
  return mixed;
}

}  // namespace daw
