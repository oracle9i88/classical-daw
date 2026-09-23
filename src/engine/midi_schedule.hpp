#pragma once

#include "daw/midi.hpp"
#include "daw/meter_map.hpp"
#include "midi_order.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace daw::detail {

struct ScheduledEvent {
  Tick tick;
  std::size_t track;
  int priority;  // 0 release, 1 channel event, 2 attack
  std::uint64_t order;
  std::size_t note_id;
  const MidiNote* note = nullptr;
  const MidiChannelEvent* channel_event = nullptr;
};

inline std::vector<ScheduledEvent> scheduleMidiEvents(const MidiFile& midi, Tick& end_tick) {
  validateMeterMap(midi.time_signature, midi.meter_changes);
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

}  // namespace daw::detail
