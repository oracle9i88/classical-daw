#include "daw/midi.hpp"
#include "daw/meter_map.hpp"

#include "midi_order.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace daw {
namespace {

using Bytes = std::vector<std::uint8_t>;

void putU16(Bytes& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
  out.push_back(static_cast<std::uint8_t>(value & 0xff));
}

void putU32(Bytes& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
  out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
  out.push_back(static_cast<std::uint8_t>(value & 0xff));
}

std::uint16_t readU16(const Bytes& data, std::size_t& pos) {
  if (pos + 2 > data.size()) throw std::runtime_error("truncated MIDI uint16");
  const std::uint16_t high = data[pos++];
  const std::uint16_t low = data[pos++];
  return static_cast<std::uint16_t>((high << 8) | low);
}

std::uint32_t readU32(const Bytes& data, std::size_t& pos) {
  if (pos + 4 > data.size()) throw std::runtime_error("truncated MIDI uint32");
  const std::uint32_t result = (static_cast<std::uint32_t>(data[pos]) << 24) |
                               (static_cast<std::uint32_t>(data[pos + 1]) << 16) |
                               (static_cast<std::uint32_t>(data[pos + 2]) << 8) |
                               static_cast<std::uint32_t>(data[pos + 3]);
  pos += 4;
  return result;
}

void putVlq(Bytes& out, std::uint32_t value) {
  std::array<std::uint8_t, 4> bytes{};
  std::size_t count = 1;
  bytes[0] = static_cast<std::uint8_t>(value & 0x7f);
  while ((value >>= 7) != 0 && count < bytes.size()) {
    bytes[count++] = static_cast<std::uint8_t>(value & 0x7f);
  }
  while (count > 0) {
    --count;
    out.push_back(static_cast<std::uint8_t>(bytes[count] | (count == 0 ? 0 : 0x80)));
  }
}

std::uint32_t getVlq(const Bytes& data, std::size_t& pos, std::size_t end) {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    if (pos >= end) throw std::runtime_error("truncated MIDI variable-length quantity");
    const std::uint8_t byte = data[pos++];
    value = (value << 7) | (byte & 0x7f);
    if ((byte & 0x80) == 0) return value;
  }
  throw std::runtime_error("invalid MIDI variable-length quantity");
}

void putChunk(Bytes& out, const char (&id)[5], const Bytes& payload) {
  out.insert(out.end(), id, id + 4);
  putU32(out, static_cast<std::uint32_t>(payload.size()));
  out.insert(out.end(), payload.begin(), payload.end());
}

struct MidiEvent {
  Tick tick = 0;
  bool note_off = false;
  std::uint8_t channel = 0;
  std::uint8_t pitch = 60;
  std::uint8_t velocity = 0;
  std::uint64_t order = 0;
};

struct TimedEvent {
  Tick tick = 0;
  int priority = 0;
  Bytes payload;
  std::uint64_t order = 0;
};

std::uint8_t denominatorExponent(std::uint8_t denominator) {
  std::uint8_t exponent = 0;
  while (denominator > 1U) {
    denominator = static_cast<std::uint8_t>(denominator >> 1U);
    ++exponent;
  }
  return exponent;
}

Tick canonicalTick(Tick tick, Tick source_ppq, std::uint64_t* rounded) {
  // Division first bounds intermediate arithmetic even for long files. The
  // remainder is below 32768, so its product with 960 cannot overflow Tick.
  const Tick whole = tick / source_ppq;
  const Tick remainder = (tick % source_ppq) * kTicksPerQuarter;
  const Tick fraction = (remainder + source_ppq / 2) / source_ppq;
  if (whole > (std::numeric_limits<Tick>::max() - fraction) / kTicksPerQuarter) {
    throw std::runtime_error("MIDI time exceeds the engine tick range after PPQ conversion");
  }
  if (remainder % source_ppq != 0) ++*rounded;
  return whole * kTicksPerQuarter + fraction;
}

}  // namespace

bool writeMidiFile(const MidiFile& file, const std::string& path, std::string* error) {
  try {
    if (file.format != 0 && file.format != 1) throw std::invalid_argument("MIDI format must be 0 or 1");
    if (file.format == 0 && file.tracks.size() > 1) {
      throw std::invalid_argument("MIDI format 0 can contain only one track");
    }
    if (file.ticks_per_quarter != kTicksPerQuarter) {
      throw std::invalid_argument("Alpha MIDI support requires 960 ticks per quarter note");
    }
    validateMeterMap(file.time_signature, file.meter_changes);
    const std::size_t track_count = std::max<std::size_t>(1, file.tracks.size());
    if (track_count > 0xffff) throw std::invalid_argument("too many MIDI tracks");

    Bytes result;
    result.insert(result.end(), {'M', 'T', 'h', 'd'});
    putU32(result, 6);
    putU16(result, static_cast<std::uint16_t>(file.format));
    putU16(result, static_cast<std::uint16_t>(track_count));
    putU16(result, static_cast<std::uint16_t>(file.ticks_per_quarter));

    for (std::size_t track_index = 0; track_index < track_count; ++track_index) {
      const MidiTrack* track = track_index < file.tracks.size() ? &file.tracks[track_index] : nullptr;
      Bytes events;
      Tick previous_tick = 0;
      if (track != nullptr && !track->name.empty()) {
        putVlq(events, 0);
        events.insert(events.end(), {0xff, 0x03});
        putVlq(events, static_cast<std::uint32_t>(track->name.size()));
        events.insert(events.end(), track->name.begin(), track->name.end());
      }
      std::vector<TimedEvent> timed_events;
      // Tempo is a global event and is emitted in the first track. It is
      // merged with note events by tick so a tempo change after tick zero does
      // not make subsequent note deltas negative.
      if (track_index == 0) {
        for (const TempoChange& change : file.tempo.changes()) {
          Bytes payload{0xff, 0x51, 0x03};
          const double micros_value = 60000000.0 / change.bpm;
          if (!std::isfinite(micros_value) || micros_value < 1.0 || micros_value > 0xFFFFFFu) {
            throw std::invalid_argument("tempo cannot be represented by a 24-bit MIDI tempo event");
          }
          const auto micros = static_cast<std::uint32_t>(std::llround(micros_value));
          payload.push_back(static_cast<std::uint8_t>((micros >> 16) & 0xff));
          payload.push_back(static_cast<std::uint8_t>((micros >> 8) & 0xff));
          payload.push_back(static_cast<std::uint8_t>(micros & 0xff));
          timed_events.push_back({change.tick, -1, std::move(payload)});
        }
        const auto add_meter = [&](Tick tick, const TimeSignature& signature) {
          timed_events.push_back({tick, -1,
              Bytes{0xff, 0x58, 0x04, signature.numerator,
                    denominatorExponent(signature.denominator), signature.clocks_per_click,
                    signature.notated_32nds_per_quarter}});
        };
        add_meter(0, file.time_signature);
        for (const auto& change : file.meter_changes) add_meter(change.tick, change.signature);
      }
      if (track != nullptr) {
        std::vector<MidiEvent> note_events;
        using BoundaryKey = std::tuple<Tick, std::uint8_t, std::uint8_t>;
        std::map<BoundaryKey, std::uint64_t> first_ordered_attack;
        for (const MidiNote& note : track->notes) {
          if (note.start < 0 || note.duration <= 0 ||
              note.start > std::numeric_limits<Tick>::max() - note.duration ||
              note.pitch > 127 || note.velocity == 0 || note.velocity > 127 || note.channel > 15 ||
              note.release_velocity > 127) {
            throw std::invalid_argument("MIDI note has an out-of-range field");
          }
          note_events.push_back({note.start, false, note.channel, note.pitch, note.velocity, note.on_order});
          note_events.push_back({note.end(), true, note.channel, note.pitch, note.release_velocity, note.off_order});
          if (note.on_order != 0) {
            const BoundaryKey key{note.start, note.channel, note.pitch};
            const auto inserted = first_ordered_attack.emplace(key, note.on_order);
            if (!inserted.second) inserted.first->second = std::min(inserted.first->second, note.on_order);
          }
        }
        for (const MidiNote& note : track->notes) {
          const auto attack = first_ordered_attack.find({note.end(), note.channel, note.pitch});
          if (note.off_order != 0 && attack != first_ordered_attack.end() && note.off_order >= attack->second) {
            // Moving or repitching an imported note can make its old ordinal
            // inconsistent with an adjacent retrigger. Refuse the stale model
            // before writing rather than silently creating a zero-length note.
            throw std::invalid_argument("MIDI source order conflicts with a same-pitch retrigger; clear orders on edited notes");
          }
        }
        std::stable_sort(note_events.begin(), note_events.end(), [](const MidiEvent& a, const MidiEvent& b) {
          if (a.tick != b.tick) return a.tick < b.tick;
          return a.note_off && !b.note_off;  // release before retrigger at the same tick
        });
        for (const MidiEvent& event : note_events) {
          Bytes payload{static_cast<std::uint8_t>((event.note_off ? 0x80 : 0x90) | event.channel),
                        event.pitch, event.velocity};
          timed_events.push_back({event.tick, event.note_off ? 0 : 2, std::move(payload), event.order});
        }
        for (const MidiChannelEvent& event : track->channel_events) {
          if (!validMidiChannelEvent(event)) throw std::invalid_argument("invalid MIDI channel event");
          Bytes payload{static_cast<std::uint8_t>(static_cast<std::uint8_t>(event.type) | event.channel), event.data1};
          if (event.type != MidiChannelEventType::ProgramChange && event.type != MidiChannelEventType::ChannelPressure) {
            payload.push_back(event.data2);
          }
          timed_events.push_back({event.tick, 1, std::move(payload), event.order});
        }
      }
      std::stable_sort(timed_events.begin(), timed_events.end(), [](const TimedEvent& a, const TimedEvent& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        // Metadata does not change channel state. Imported channel events and
        // note edges retain source order, including after PPQ rounding merges
        // nearby ticks. Newly authored events use the documented fallback.
        // An authored note ending here must release before an imported
        // same-pitch retrigger. Otherwise note-on followed by that new off
        // would prematurely release the new note (or form a zero-length pair).
        return detail::midiEventBefore(a.priority, a.order, b.priority, b.order);
      });
      for (const TimedEvent& event : timed_events) {
        if (event.tick < previous_tick || event.tick - previous_tick > 0x0fffffff) {
          throw std::invalid_argument("MIDI event delta is outside VLQ range");
        }
        putVlq(events, static_cast<std::uint32_t>(event.tick - previous_tick));
        events.insert(events.end(), event.payload.begin(), event.payload.end());
        previous_tick = event.tick;
      }
      putVlq(events, 0);
      events.insert(events.end(), {0xff, 0x2f, 0x00});
      putChunk(result, "MTrk", events);
    }

    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("cannot open output MIDI file");
    output.write(reinterpret_cast<const char*>(result.data()), static_cast<std::streamsize>(result.size()));
    if (!output) throw std::runtime_error("failed while writing MIDI file");
    return true;
  } catch (const std::exception& exception) {
    if (error) *error = exception.what();
    return false;
  }
}

bool readMidiFile(const std::string& path, MidiFile* file, std::string* error, MidiImportReport* report) {
  if (file == nullptr) {
    if (error) *error = "file output pointer is null";
    return false;
  }
  try {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open MIDI file");
    const Bytes data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    std::size_t pos = 0;
    if (data.size() < 14 || std::string(data.begin(), data.begin() + 4) != "MThd") {
      throw std::runtime_error("invalid MIDI header");
    }
    pos = 4;
    const std::uint32_t header_length = readU32(data, pos);
    if (header_length < 6 || pos + header_length > data.size()) throw std::runtime_error("invalid MIDI header length");
    const auto format = readU16(data, pos);
    const auto track_count = readU16(data, pos);
    const auto division = readU16(data, pos);
    if (format > 1 || (division & 0x8000) != 0 || division == 0) {
      throw std::runtime_error("unsupported MIDI format or time division (requires Type 0/1 and positive PPQ)");
    }
    if (track_count == 0) throw std::runtime_error("MIDI file requires at least one track");
    if (format == 0 && track_count != 1) throw std::runtime_error("MIDI format 0 can contain only one track");
    pos = 8 + header_length;

    MidiFile parsed;
    MidiImportReport diagnostics;
    diagnostics.source_ticks_per_quarter = division;
    parsed.format = static_cast<std::int16_t>(format);
    parsed.ticks_per_quarter = kTicksPerQuarter;
    parsed.tracks.reserve(track_count);
    // Only explicit messages enter this map. The implicit 4/4 at tick zero
    // must neither count as a coalesced event nor backdate a later signature.
    std::map<Tick, TimeSignature> explicit_meters;
    // The writer emits one initial signature in addition to the bounded
    // later map, so its largest valid output must remain readable.
    constexpr std::uint64_t kMaxRawTimeSignatureEvents =
        static_cast<std::uint64_t>(kMaxMeterChanges) + 1;
    for (std::uint16_t track_index = 0; track_index < track_count; ++track_index) {
      if (pos + 8 > data.size() || std::string(reinterpret_cast<const char*>(data.data() + pos), 4) != "MTrk") {
        throw std::runtime_error("missing MIDI track chunk");
      }
      pos += 4;
      const std::uint32_t track_length = readU32(data, pos);
      if (track_length > data.size() - pos) throw std::runtime_error("truncated MIDI track");
      const std::size_t track_end = pos + track_length;
      MidiTrack track;
      Tick tick = 0;
      std::uint8_t running_status = 0;
      struct ActiveNote { Tick start; std::uint8_t velocity; std::uint64_t order; };
      std::map<std::pair<std::uint8_t, std::uint8_t>, std::vector<ActiveNote>> active;
      std::uint64_t event_order = 0;
      bool end_of_track_seen = false;
      while (pos < track_end) {
        ++event_order;
        const Tick delta = static_cast<Tick>(getVlq(data, pos, track_end));
        if (delta > std::numeric_limits<Tick>::max() - tick) {
          throw std::runtime_error("MIDI event tick overflow");
        }
        tick += delta;
        if (pos >= track_end) throw std::runtime_error("truncated MIDI event");
        std::uint8_t status = data[pos++];
        if ((status & 0x80) == 0) {
          if (running_status == 0) throw std::runtime_error("MIDI running status without prior event");
          --pos;
          status = running_status;
        } else if (status < 0xf0) {
          running_status = status;
        }
        if (status == 0xff) {
          running_status = 0;
          if (pos >= track_end) throw std::runtime_error("truncated MIDI meta event");
          const std::uint8_t type = data[pos++];
          const std::uint32_t length = getVlq(data, pos, track_end);
          if (length > track_end - pos) throw std::runtime_error("truncated MIDI meta payload");
          if (type == 0x2f) {
            if (length != 0) throw std::runtime_error("invalid MIDI end-of-track length");
            end_of_track_seen = true;
            pos += length;
            if (pos != track_end) throw std::runtime_error("extra data after MIDI end-of-track event");
            break;
          } else if (type == 0x03) {
            track.name.assign(reinterpret_cast<const char*>(data.data() + pos), length);
          } else if (type == 0x51) {
            if (length != 3) throw std::runtime_error("invalid MIDI tempo length");
            const std::uint32_t micros = (static_cast<std::uint32_t>(data[pos]) << 16) |
                                         (static_cast<std::uint32_t>(data[pos + 1]) << 8) |
                                         data[pos + 2];
            if (micros == 0) throw std::runtime_error("invalid MIDI tempo value");
            parsed.tempo.addChange(canonicalTick(tick, division, &diagnostics.rounded_tempo_events),
                                   60000000.0 / micros);
          } else if (type == 0x58) {
            if (length != 4) throw std::runtime_error("invalid MIDI time signature length");
            const std::uint8_t numerator = data[pos];
            const std::uint8_t exponent = data[pos + 1];
            if (numerator == 0 || exponent > 7 || data[pos + 3] == 0) {
              throw std::runtime_error("invalid MIDI time signature");
            }
            // Bound input work before growing the canonical map, including
            // duplicate messages that would otherwise bypass a map-size cap.
            if (diagnostics.preserved_time_signature_events >= kMaxRawTimeSignatureEvents) {
              throw std::runtime_error("MIDI file has too many time signature events");
            }
            const Tick normalized = canonicalTick(tick, division, &diagnostics.rounded_time_signature_events);
            const TimeSignature signature{
                numerator, static_cast<std::uint8_t>(static_cast<std::uint16_t>(1U) << exponent),
                data[pos + 2], data[pos + 3]};
            const auto inserted = explicit_meters.emplace(normalized, signature);
            if (!inserted.second) {
              inserted.first->second = signature;
              ++diagnostics.coalesced_time_signature_events;
            }
            ++diagnostics.preserved_time_signature_events;
          } else {
            ++diagnostics.ignored_meta_events;
          }
          pos += length;
          continue;
        }
        if (status == 0xf0 || status == 0xf7) {
          running_status = 0;
          const std::uint32_t length = getVlq(data, pos, track_end);
          if (length > track_end - pos) throw std::runtime_error("truncated MIDI sysex payload");
          pos += length;
          ++diagnostics.ignored_sysex_events;
          continue;
        }
        if (status < 0x80 || status >= 0xf0) throw std::runtime_error("unsupported MIDI event");
        const std::uint8_t command = status & 0xf0;
        const std::uint8_t channel = status & 0x0f;
        if (command == 0xc0 || command == 0xd0) {
          if (pos >= track_end || (data[pos] & 0x80) != 0) {
            throw std::runtime_error("truncated MIDI channel event");
          }
          const Tick normalized = canonicalTick(tick, division, &diagnostics.rounded_channel_events);
          track.channel_events.push_back({normalized, static_cast<MidiChannelEventType>(command), channel,
                                          data[pos++], 0, event_order});
          ++diagnostics.preserved_channel_events;
        } else {
          if (pos >= track_end || (data[pos] & 0x80) != 0) {
            throw std::runtime_error("truncated MIDI channel event");
          }
          const std::uint8_t pitch = data[pos++];
          if (pos >= track_end || (data[pos] & 0x80) != 0) {
            throw std::runtime_error("truncated MIDI channel event");
          }
          const std::uint8_t value = data[pos++];
          if (command == 0x90 && value != 0) {
            if (!active[{channel, pitch}].empty()) ++diagnostics.overlapping_same_pitch_notes;
            active[{channel, pitch}].push_back({tick, value, event_order});
          } else if (command == 0x80 || (command == 0x90 && value == 0)) {
            auto& stack = active[{channel, pitch}];
            if (stack.empty()) throw std::runtime_error("MIDI note-off has no matching note-on");
            const auto [start, velocity, on_order] = stack.back();
            stack.pop_back();
            if (tick <= start) throw std::runtime_error("MIDI note duration must be positive");
            const Tick normalized_start = canonicalTick(start, division, &diagnostics.rounded_note_boundaries);
            const Tick normalized_end = canonicalTick(tick, division, &diagnostics.rounded_note_boundaries);
            if (normalized_end <= normalized_start) {
              throw std::runtime_error("MIDI note collapses to zero duration at 960 PPQ");
            }
            track.notes.push_back({normalized_start, normalized_end - normalized_start, pitch, velocity, channel,
                                   value, on_order, event_order});
          } else {
            const Tick normalized = canonicalTick(tick, division, &diagnostics.rounded_channel_events);
            track.channel_events.push_back({normalized, static_cast<MidiChannelEventType>(command), channel,
                                            pitch, value, event_order});
            ++diagnostics.preserved_channel_events;
          }
        }
      }
      if (!end_of_track_seen) throw std::runtime_error("MIDI track is missing end-of-track event");
      for (const auto& entry : active) {
        if (!entry.second.empty()) throw std::runtime_error("MIDI track has unclosed notes at end-of-track");
      }
      std::sort(track.notes.begin(), track.notes.end(), [](const MidiNote& a, const MidiNote& b) {
        if (a.start != b.start) return a.start < b.start;
        if (a.pitch != b.pitch) return a.pitch < b.pitch;
        return a.channel < b.channel;
      });
      parsed.tracks.push_back(std::move(track));
      pos = track_end;
    }
    if (pos != data.size()) throw std::runtime_error("trailing data after MIDI tracks");
    parsed.meter_changes.reserve(explicit_meters.size());
    for (const auto& entry : explicit_meters) {
      if (entry.first == 0) parsed.time_signature = entry.second;
      else parsed.meter_changes.push_back({entry.first, entry.second});
    }
    validateMeterMap(parsed.time_signature, parsed.meter_changes);
    *file = std::move(parsed);
    if (report != nullptr) *report = diagnostics;
    if (error != nullptr) error->clear();
    return true;
  } catch (const std::exception& exception) {
    if (error) *error = exception.what();
    return false;
  }
}

}  // namespace daw
