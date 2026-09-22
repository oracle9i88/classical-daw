#include "daw/render.hpp"

#include "daw/score_midi.hpp"
#include "midi_order.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>

namespace daw {
namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

std::uint32_t validateConfiguration(double rate, double tail) {
  if (!std::isfinite(rate) || rate <= 0.0 ||
      rate > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
    throw std::invalid_argument("sample rate must be positive and within WAV range");
  }
  if (!std::isfinite(tail) || tail < 0.0) throw std::invalid_argument("tail must be non-negative");
  const auto rounded = std::llround(rate);
  if (rounded <= 0) throw std::invalid_argument("sample rate rounds to zero");
  return static_cast<std::uint32_t>(rounded);
}

struct ScheduledEvent {
  Tick tick;
  std::size_t track;
  int priority;  // 0 release, 1 channel event, 2 attack
  std::uint64_t order;
  std::size_t note_id;
  const MidiNote* note = nullptr;
  const MidiChannelEvent* channel_event = nullptr;
};

struct ChannelState {
  double volume = 1.0;
  double expression = 1.0;
  double frequency_ratio = 1.0;
  bool sustain = false;
};

struct Voice {
  const MidiNote* note;
  std::size_t start_frame;
  double frequency;
  double phase = 0.0;
  bool key_down = true;
  bool releasing = false;
  std::size_t release_frame = 0;
  double release_level = 1.0;
};

void releaseVoice(Voice& voice, std::size_t frame, double attack_frames) {
  if (voice.releasing) return;
  voice.releasing = true;
  voice.release_frame = frame;
  voice.release_level = std::min(1.0, static_cast<double>(frame - voice.start_frame) / attack_frames);
}

std::vector<ScheduledEvent> schedule(const MidiFile& midi, Tick& end_tick,
                                     MidiRenderReport& report) {
  if (midi.ticks_per_quarter != kTicksPerQuarter) {
    throw std::invalid_argument("render input must use normalized 960 PPQ");
  }
  if ((midi.format != 0 && midi.format != 1) || (midi.format == 0 && midi.tracks.size() > 1)) {
    throw std::invalid_argument("render input must be synchronous SMF Type 0 or 1");
  }
  std::vector<ScheduledEvent> events;
  std::size_t note_id = 0;
  for (std::size_t t = 0; t < midi.tracks.size(); ++t) {
    const auto& track = midi.tracks[t];
    using Boundary = std::tuple<Tick, std::uint8_t, std::uint8_t>;
    std::map<Boundary, std::uint64_t> attacks;
    for (const auto& note : track.notes) {
      if (note.start < 0 || note.duration <= 0 ||
          note.start > std::numeric_limits<Tick>::max() - note.duration ||
          note.pitch > 127 || note.velocity == 0 || note.velocity > 127 ||
          note.channel > 15 || note.release_velocity > 127) {
        throw std::invalid_argument("MIDI note has an out-of-range field");
      }
      end_tick = std::max(end_tick, note.end());
      events.push_back({note.start, t, 2, note.on_order, note_id, &note, nullptr});
      events.push_back({note.end(), t, 0, note.off_order, note_id, &note, nullptr});
      ++note_id;
      if (note.release_velocity != 0) ++report.ignored_release_velocities;
      if (note.on_order != 0) {
        auto [it, inserted] = attacks.emplace(Boundary{note.start, note.channel, note.pitch}, note.on_order);
        if (!inserted) it->second = std::min(it->second, note.on_order);
      }
    }
    for (const auto& note : track.notes) {
      const auto attack = attacks.find({note.end(), note.channel, note.pitch});
      if (note.off_order != 0 && attack != attacks.end() && note.off_order >= attack->second) {
        throw std::invalid_argument("MIDI source order conflicts with a same-pitch retrigger; clear orders on edited notes");
      }
    }
    for (const auto& event : track.channel_events) {
      if (!validMidiChannelEvent(event)) throw std::invalid_argument("invalid MIDI channel event");
      end_tick = std::max(end_tick, event.tick);
      events.push_back({event.tick, t, 1, event.order, 0, nullptr, &event});
    }
  }
  std::stable_sort(events.begin(), events.end(), [](const ScheduledEvent& a, const ScheduledEvent& b) {
    if (a.tick != b.tick) return a.tick < b.tick;
    // Source ordinals belong to one track, never to the entire SMF.
    if (a.track != b.track) return a.track < b.track;
    return detail::midiEventBefore(a.priority, a.order, b.priority, b.order);
  });
  return events;
}

}  // namespace

AudioBuffer renderMidiFile(const MidiFile& midi, double sample_rate, double tail_seconds,
                           MidiRenderReport* report, std::size_t max_output_frames) {
  const auto rate = validateConfiguration(sample_rate, tail_seconds);
  MidiRenderReport diagnostics;
  Tick end_tick = 0;
  const auto events = schedule(midi, end_tick, diagnostics);
  const double end_seconds = midi.tempo.tickToSeconds(end_tick);
  const double duration_frames = (end_seconds + tail_seconds) * static_cast<double>(rate);
  // Decimal tails such as 0.1 can put an exact frame boundary one ULP above
  // its integer. Do not append a spurious sample for that arithmetic noise.
  const double frames_real = std::ceil(std::nextafter(duration_frames, 0.0));
  AudioBuffer output;
  output.sample_rate = rate;
  output.channels = 1;
  // Bound before any floating-point-to-integer conversion or allocation.
  const auto max_frames = std::min<std::uintmax_t>(output.samples.max_size(),
      static_cast<std::uintmax_t>(std::numeric_limits<SampleIndex>::max()));
  if (!std::isfinite(end_seconds) || end_seconds < 0.0 || !std::isfinite(frames_real) ||
      frames_real < 0.0 || frames_real >= static_cast<double>(max_frames)) {
    throw std::length_error("render output is too large");
  }
  const auto frame_count = static_cast<std::size_t>(frames_real);
  if (frame_count > max_output_frames) {
    throw std::length_error("render exceeds the configured output frame budget; use a shorter input or an explicit larger budget");
  }
  auto frameAt = [&](Tick tick) {
    const double frame = midi.tempo.tickToSeconds(tick) * static_cast<double>(rate);
    if (!std::isfinite(frame) || frame < 0.0 || frame >= static_cast<double>(max_frames)) {
      throw std::length_error("render event time is out of range");
    }
    return std::min(frame_count, static_cast<std::size_t>(std::llround(frame)));
  };
  const auto end_frame = frameAt(end_tick);
  output.samples.assign(frame_count, 0.0F);
  const double attack_frames = std::max(1.0, static_cast<double>(rate) * 0.008);
  const double release_frames = std::max(1.0, static_cast<double>(rate) * 0.035);
  std::array<ChannelState, 16> channels{};
  // Stable note identities keep retriggers and cross-track unisons independent.
  // Allocation is permitted here: this renderer is explicitly offline only.
  std::map<std::size_t, Voice> voices;
  std::size_t cursor = 0;
  auto renderUntil = [&](std::size_t limit) {
    if (limit == cursor) return;  // Simultaneous chords must not scan all voices per event.
    for (auto it = voices.begin(); it != voices.end();) {
      auto& voice = it->second;
      const auto& state = channels[voice.note->channel];
      const double gain = 0.18 * static_cast<double>(voice.note->velocity) / 127.0 *
                          state.volume * state.expression;
      const double increment = kTwoPi * voice.frequency * state.frequency_ratio / static_cast<double>(rate);
      std::size_t frame = cursor;
      for (; frame < limit; ++frame) {
        double envelope = std::min(1.0, static_cast<double>(frame - voice.start_frame) / attack_frames);
        if (voice.releasing) {
          const double release_age = static_cast<double>(frame - voice.release_frame);
          if (release_age >= release_frames) break;
          envelope = voice.release_level * (1.0 - release_age / release_frames);
        }
        output.samples[frame] += static_cast<float>(gain * envelope * std::sin(voice.phase));
        // Preserve phase on controller changes and bound it for long notes.
        voice.phase = std::fmod(voice.phase + increment, kTwoPi);
      }
      if (frame < limit) it = voices.erase(it);
      else ++it;
    }
    cursor = limit;
  };
  auto releaseSustained = [&](std::uint8_t channel, std::size_t frame) {
    for (auto& entry : voices) {
      auto& voice = entry.second;
      if (voice.note->channel == channel && !voice.key_down) releaseVoice(voice, frame, attack_frames);
    }
  };
  for (const auto& event : events) {
    const auto frame = frameAt(event.tick);
    renderUntil(frame);
    if (event.priority == 2) {
      voices.emplace(event.note_id, Voice{event.note, frame,
          440.0 * std::pow(2.0, (static_cast<int>(event.note->pitch) - 69) / 12.0)});
    } else if (event.priority == 0) {
      const auto it = voices.find(event.note_id);
      if (it != voices.end()) {
        auto& voice = it->second;
        voice.key_down = false;
        if (!channels[voice.note->channel].sustain) releaseVoice(voice, frame, attack_frames);
      }
    } else {
      const auto& message = *event.channel_event;
      auto& state = channels[message.channel];
      bool interpreted = true;
      if (message.type == MidiChannelEventType::PitchBend) {
        const int value = static_cast<int>(message.data1) + 128 * static_cast<int>(message.data2);
        const double bend = static_cast<double>(value - 8192) / (value < 8192 ? 8192.0 : 8191.0);
        state.frequency_ratio = std::pow(2.0, (2.0 * bend) / 12.0);
      } else if (message.type == MidiChannelEventType::ControlChange) {
        switch (message.data1) {
          case 7: state.volume = static_cast<double>(message.data2) / 127.0; break;
          case 11: state.expression = static_cast<double>(message.data2) / 127.0; break;
          case 64:
            state.sustain = message.data2 >= 64;
            if (!state.sustain) releaseSustained(message.channel, frame);
            break;
          case 120:
            if (message.data2 != 0) { interpreted = false; break; }
            for (auto it = voices.begin(); it != voices.end();) {
              if (it->second.note->channel == message.channel) it = voices.erase(it);
              else ++it;
            }
            break;
          case 121:
            if (message.data2 != 0) { interpreted = false; break; }
            // Reset supported performance controllers; channel volume is kept.
            state.expression = 1.0;
            state.frequency_ratio = 1.0;
            state.sustain = false;
            releaseSustained(message.channel, frame);
            break;
          case 123:
            if (message.data2 != 0) { interpreted = false; break; }
            for (auto& entry : voices) {
              auto& voice = entry.second;
              if (voice.note->channel != message.channel) continue;
              voice.key_down = false;
              if (!state.sustain) releaseVoice(voice, frame, attack_frames);
            }
            break;
          default: interpreted = false; break;
        }
      } else {
        interpreted = false;
      }
      if (interpreted) ++diagnostics.interpreted_channel_events;
      else ++diagnostics.unsupported_channel_events;
    }
  }
  renderUntil(end_frame);
  for (auto& entry : voices) {
    if (!entry.second.releasing) {
      releaseVoice(entry.second, end_frame, attack_frames);
      ++diagnostics.voices_released_at_end;
    }
  }
  renderUntil(frame_count);
  for (float& sample : output.samples) {
    if (sample < -1.0F || sample > 1.0F) ++diagnostics.clipped_samples;
    sample = std::clamp(sample, -1.0F, 1.0F);
  }
  if (report != nullptr) *report = diagnostics;
  return output;
}

AudioBuffer renderNotes(const MidiTrack& track, const TempoMap& tempo, double sample_rate,
                        double tail_seconds, MidiRenderReport* report, std::size_t max_output_frames) {
  MidiFile midi;
  midi.tempo = tempo;
  midi.tracks.push_back(track);
  return renderMidiFile(midi, sample_rate, tail_seconds, report, max_output_frames);
}

AudioBuffer renderScore(const Score& score, double sample_rate, double tail_seconds,
                        MidiRenderReport* report, std::size_t max_output_frames) {
  (void)validateConfiguration(sample_rate, tail_seconds);
  MidiFile midi;
  std::string error;
  if (!scoreToMidiFile(score, &midi, &error)) {
    throw std::invalid_argument("score render conversion failed: " + error);
  }
  return renderMidiFile(midi, sample_rate, tail_seconds, report, max_output_frames);
}

}  // namespace daw
