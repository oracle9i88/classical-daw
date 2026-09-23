#include "daw/midi_sequence.hpp"
#include "midi_schedule.hpp"

#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace daw {

void requireInitialExpression(const MidiSampleSequence& sequence) {
  std::array<bool, 16> initialized{};
  for (const auto& event : sequence.events) {
    const auto channel = static_cast<std::size_t>(event.status & 0x0f);
    if ((event.status & 0xf0) == 0xb0 && event.data1 == 11) initialized[channel] = true;
    if ((event.status & 0xf0) == 0x90 && event.data2 != 0 && !initialized[channel]) {
      throw std::invalid_argument("instrument requires CC11 expression before the first note on MIDI channel " +
                                  std::to_string(channel + 1));
    }
  }
}

bool isInstrumentSelection(const MidiSampleEvent& event) noexcept {
  return (event.status & 0xf0) == 0xc0 ||
      ((event.status & 0xf0) == 0xb0 && (event.data1 == 0 || event.data1 == 32));
}

MidiSampleSequence makeMidiSampleSequence(const MidiFile& midi, std::uint32_t rate,
                                          double tail, std::size_t max_frames) {
  if (rate < 8000 || rate > 192000 || !std::isfinite(tail) || tail <= 0.0 || tail > 60.0) {
    throw std::invalid_argument("instrument rendering requires 8000..192000 Hz and a tail in (0,60] seconds");
  }
  // Bound events before allocating the sorted schedule (two per note).
  std::size_t count = 0;
  for (const auto& track : midi.tracks) {
    if (track.notes.size() > 1000000 || track.channel_events.size() > 1000000) {
      throw std::length_error("instrument input event budget exceeded");
    }
    count += 2 * track.notes.size() + track.channel_events.size();
    if (count > 3000000) throw std::length_error("instrument input event budget exceeded");
  }
  Tick end_tick = 0;
  const auto source = detail::scheduleMidiEvents(midi, end_tick);
  MidiSampleSequence sequence;
  sequence.sample_rate = rate;
  const double end = midi.tempo.tickToSeconds(end_tick) * static_cast<double>(rate);
  const double frames = std::ceil(std::nextafter(end + tail * static_cast<double>(rate), 0.0));
  // Keep sample positions exactly representable in AU's Float64 timestamps.
  constexpr double exact_limit = 9007199254740991.0;
  if (!std::isfinite(end) || end < 0 || !std::isfinite(frames) || frames >= exact_limit ||
      frames >= static_cast<double>(std::numeric_limits<std::size_t>::max() / 2) ||
      frames > static_cast<double>(max_frames)) {
    throw std::length_error("instrument render exceeds output frame budget");
  }
  sequence.frames = static_cast<std::size_t>(frames);
  sequence.end_frame = static_cast<std::size_t>(std::llround(end));
  // Even a sub-sample tail must leave room to dispatch the final release.
  if (sequence.frames <= sequence.end_frame) {
    if (sequence.end_frame >= max_frames) throw std::length_error("no room for instrument release");
    sequence.frames = sequence.end_frame + 1;
  }
  sequence.events.reserve(source.size() + 64);
  std::array<bool, 16> channels{};
  for (const auto& event : source) {
    const auto frame = static_cast<std::size_t>(std::llround(
        midi.tempo.tickToSeconds(event.tick) * static_cast<double>(rate)));
    MidiSampleEvent message;
    message.frame = frame;
    if (event.note) {
      message.status = static_cast<std::uint8_t>((event.priority == 2 ? 0x90 : 0x80) | event.note->channel);
      message.data1 = event.note->pitch;
      message.data2 = event.priority == 2 ? event.note->velocity : event.note->release_velocity;
    } else {
      const auto& channel = *event.channel_event;
      message.status = static_cast<std::uint8_t>(static_cast<std::uint8_t>(channel.type) | channel.channel);
      message.data1 = channel.data1;
      message.data2 = channel.data2;
    }
    channels[message.status & 0x0f] = true;
    sequence.events.push_back(message);
  }
  for (std::size_t channel = 0; channel < channels.size(); ++channel) {
    if (!channels[channel]) continue;
    for (auto controller : {64, 66, 69, 123}) {
      sequence.events.push_back({sequence.end_frame, static_cast<std::uint8_t>(0xb0 | channel),
                                 static_cast<std::uint8_t>(controller), 0});
    }
  }
  return sequence;
}

}  // namespace daw
