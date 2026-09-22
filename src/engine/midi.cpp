#include "daw/midi.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
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

std::uint32_t getVlq(const Bytes& data, std::size_t& pos) {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    if (pos >= data.size()) throw std::runtime_error("truncated MIDI variable-length quantity");
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
};

struct TimedEvent {
  Tick tick = 0;
  int priority = 0;
  Bytes payload;
};

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
          timed_events.push_back({change.tick, 1, std::move(payload)});
        }
      }
      if (track != nullptr) {
        std::vector<MidiEvent> note_events;
        for (const MidiNote& note : track->notes) {
          if (note.start < 0 || note.duration <= 0 ||
              note.start > std::numeric_limits<Tick>::max() - note.duration ||
              note.pitch > 127 || note.velocity > 127 || note.channel > 15) {
            throw std::invalid_argument("MIDI note has an out-of-range field");
          }
          note_events.push_back({note.start, false, note.channel, note.pitch, note.velocity});
          note_events.push_back({note.end(), true, note.channel, note.pitch, 0});
        }
        std::stable_sort(note_events.begin(), note_events.end(), [](const MidiEvent& a, const MidiEvent& b) {
          if (a.tick != b.tick) return a.tick < b.tick;
          return a.note_off && !b.note_off;  // release before retrigger at the same tick
        });
        for (const MidiEvent& event : note_events) {
          Bytes payload{static_cast<std::uint8_t>((event.note_off ? 0x80 : 0x90) | event.channel),
                        event.pitch, event.velocity};
          timed_events.push_back({event.tick, event.note_off ? 0 : 2, std::move(payload)});
        }
      }
      std::stable_sort(timed_events.begin(), timed_events.end(), [](const TimedEvent& a, const TimedEvent& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        return a.priority < b.priority;
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

bool readMidiFile(const std::string& path, MidiFile* file, std::string* error) {
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
    if (format > 1 || (division & 0x8000) != 0 || division != kTicksPerQuarter) {
      throw std::runtime_error("unsupported MIDI format or time division (Alpha requires 960 PPQ)");
    }
    pos = 8 + header_length;

    MidiFile parsed;
    parsed.format = static_cast<std::int16_t>(format);
    parsed.ticks_per_quarter = static_cast<Tick>(division);
    parsed.tracks.reserve(track_count);
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
      std::map<std::pair<std::uint8_t, std::uint8_t>, std::vector<std::pair<Tick, std::uint8_t>>> active;
      while (pos < track_end) {
        const Tick delta = static_cast<Tick>(getVlq(data, pos));
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
          if (pos >= track_end) throw std::runtime_error("truncated MIDI meta event");
          const std::uint8_t type = data[pos++];
          const std::uint32_t length = getVlq(data, pos);
          if (length > track_end - pos) throw std::runtime_error("truncated MIDI meta payload");
          if (type == 0x2f) {
            pos += length;
            break;
          } else if (type == 0x03) {
            track.name.assign(reinterpret_cast<const char*>(data.data() + pos), length);
          } else if (type == 0x51 && length == 3) {
            const std::uint32_t micros = (static_cast<std::uint32_t>(data[pos]) << 16) |
                                         (static_cast<std::uint32_t>(data[pos + 1]) << 8) |
                                         data[pos + 2];
            if (micros != 0) parsed.tempo.addChange(tick, 60000000.0 / micros);
          }
          pos += length;
          continue;
        }
        if (status == 0xf0 || status == 0xf7) {
          const std::uint32_t length = getVlq(data, pos);
          if (length > track_end - pos) throw std::runtime_error("truncated MIDI sysex payload");
          pos += length;
          continue;
        }
        if (status < 0x80 || status >= 0xf0) throw std::runtime_error("unsupported MIDI event");
        const std::uint8_t command = status & 0xf0;
        const std::uint8_t channel = status & 0x0f;
        if (command == 0xc0 || command == 0xd0) {
          if (pos >= track_end || (data[pos] & 0x80) != 0) {
            throw std::runtime_error("truncated MIDI channel event");
          }
          ++pos;  // Program Change and Channel Pressure each carry one data byte.
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
            active[{channel, pitch}].push_back({tick, value});
          } else if (command == 0x80 || (command == 0x90 && value == 0)) {
            auto& stack = active[{channel, pitch}];
            if (!stack.empty()) {
              const auto [start, velocity] = stack.back();
              stack.pop_back();
              track.notes.push_back({start, std::max<Tick>(0, tick - start), pitch, velocity, channel});
            }
          }
        }
      }
      for (const auto& [key, stack] : active) {
        for (const auto& [start, velocity] : stack) {
          track.notes.push_back({start, std::max<Tick>(0, tick - start), key.second, velocity, key.first});
        }
      }
      std::sort(track.notes.begin(), track.notes.end(), [](const MidiNote& a, const MidiNote& b) {
        if (a.start != b.start) return a.start < b.start;
        if (a.pitch != b.pitch) return a.pitch < b.pitch;
        return a.channel < b.channel;
      });
      parsed.tracks.push_back(std::move(track));
      pos = track_end;
    }
    *file = std::move(parsed);
    return true;
  } catch (const std::exception& exception) {
    if (error) *error = exception.what();
    return false;
  }
}

}  // namespace daw
