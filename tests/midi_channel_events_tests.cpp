#include "daw/midi.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Bytes = std::vector<std::uint8_t>;
using EventType = daw::MidiChannelEventType;

void require(bool value, const std::string& message) {
  if (!value) throw std::runtime_error(message);
}
void putU32(Bytes& out, std::size_t value) {
  for (int shift : {24, 16, 8, 0}) out.push_back(static_cast<std::uint8_t>(value >> shift));
}
void putVlq(Bytes& out, unsigned value) {
  unsigned shift = 0;
  for (unsigned rest = value; rest > 127; rest >>= 7) shift += 7;
  for (;;) {
    out.push_back(static_cast<std::uint8_t>(((value >> shift) & 127) | (shift ? 128 : 0)));
    if (!shift) break;
    shift -= 7;
  }
}
void event(Bytes& track, unsigned delta, std::initializer_list<std::uint8_t> payload) {
  putVlq(track, delta);
  track.insert(track.end(), payload);
}
Bytes smf(unsigned ppq, const std::vector<Bytes>& tracks, unsigned format = 0) {
  Bytes bytes{'M','T','h','d',0,0,0,6,0,static_cast<std::uint8_t>(format),
              static_cast<std::uint8_t>(tracks.size() >> 8),static_cast<std::uint8_t>(tracks.size()),
              static_cast<std::uint8_t>(ppq >> 8),static_cast<std::uint8_t>(ppq)};
  for (const auto& track : tracks) {
    bytes.insert(bytes.end(), {'M','T','r','k'});
    putU32(bytes, track.size());
    bytes.insert(bytes.end(), track.begin(), track.end());
  }
  return bytes;
}
void save(const std::filesystem::path& path, const Bytes& bytes) {
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  require(static_cast<bool>(out), "could not write test fixture");
}
Bytes load(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  require(static_cast<bool>(in), "could not read test output");
  return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct WireEvent {
  daw::Tick tick;
  Bytes bytes;
};
using WireTrack = std::vector<WireEvent>;

// An independent, small SMF decoder is the export oracle. It does not call the
// production reader or use the engine's channel-event serialization helpers.
// Metadata is skipped; every channel message, including note edges, is kept.
class WireReader {
 public:
  explicit WireReader(const Bytes& bytes) : bytes_(bytes) {}
  std::vector<WireTrack> read() {
    expect("MThd");
    const auto header_length = integer(4);
    require(header_length >= 6, "wire decoder: short header");
    const auto format = integer(2);
    require(format <= 1, "wire decoder: unexpected format");
    const auto count = integer(2);
    require(integer(2) == 960, "writer did not emit canonical 960 PPQ");
    skip(header_length - 6);
    std::vector<WireTrack> tracks;
    for (std::uint32_t i = 0; i < count; ++i) {
      expect("MTrk");
      const auto length = integer(4);
      require(length <= bytes_.size() - pos_, "wire decoder: truncated track");
      const auto end = pos_ + length;
      daw::Tick tick = 0;
      std::uint8_t running = 0;
      WireTrack track;
      while (pos_ < end) {
        tick += variable(end);
        auto status = byte(end);
        if (status < 128) {
          require(running != 0, "wire decoder: missing running status");
          --pos_;
          status = running;
        }
        if (status == 0xff) {
          byte(end);
          const auto length = variable(end);
          require(length <= end - pos_, "wire decoder: meta payload crosses track");
          skip(length);
          running = 0;
          continue;
        }
        if (status == 0xf0 || status == 0xf7) {
          const auto length = variable(end);
          require(length <= end - pos_, "wire decoder: SysEx payload crosses track");
          skip(length);
          running = 0;
          continue;
        }
        require(status >= 0x80 && status < 0xf0, "wire decoder: invalid channel status");
        running = status;
        Bytes payload{status};
        const auto family = status & 0xf0;
        const int size = family == 0xc0 || family == 0xd0 ? 1 : 2;
        for (int j = 0; j < size; ++j) {
          const auto value = byte(end);
          require(value < 128, "writer emitted a status byte in channel data");
          payload.push_back(value);
        }
        track.push_back({tick, std::move(payload)});
      }
      require(pos_ == end, "wire decoder: track overflow");
      tracks.push_back(std::move(track));
    }
    require(pos_ == bytes_.size(), "wire decoder: trailing bytes");
    return tracks;
  }
 private:
  std::uint8_t byte(std::size_t end) {
    require(pos_ < end && pos_ < bytes_.size(), "wire decoder: unexpected end");
    return bytes_[pos_++];
  }
  std::uint32_t integer(unsigned count) {
    std::uint32_t value = 0;
    while (count--) value = (value << 8) | byte(bytes_.size());
    return value;
  }
  std::uint32_t variable(std::size_t end) {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const auto next = byte(end);
      value = (value << 7) | (next & 127);
      if (next < 128) return value;
    }
    throw std::runtime_error("wire decoder: invalid VLQ");
  }
  void skip(std::size_t count) {
    require(count <= bytes_.size() - pos_, "wire decoder: truncated payload");
    pos_ += count;
  }
  void expect(const char* signature) {
    for (int i = 0; i < 4; ++i) {
      require(byte(bytes_.size()) == static_cast<std::uint8_t>(signature[i]), "wire decoder: wrong chunk");
    }
  }
  const Bytes& bytes_;
  std::size_t pos_ = 0;
};
void sameEvents(const WireTrack& actual, const WireTrack& expected, const std::string& label) {
  require(actual.size() == expected.size(), label + ": channel message count differs");
  for (std::size_t i = 0; i < expected.size(); ++i) {
    require(actual[i].tick == expected[i].tick && actual[i].bytes == expected[i].bytes,
            label + ": message bytes/order/tick differ at index " + std::to_string(i));
  }
}
void checkExport(const daw::MidiFile& midi, const std::filesystem::path& output,
                 const std::vector<WireTrack>& expected, const std::string& label) {
  std::string error;
  require(daw::writeMidiFile(midi, output.string(), &error), label + ": " + error);
  const auto bytes = load(output);
  const auto tracks = WireReader(bytes).read();
  require(tracks.size() == expected.size(), label + ": track count differs");
  for (std::size_t i = 0; i < expected.size(); ++i) sameEvents(tracks[i], expected[i], label);
}

void allTypesAndRounding(const std::filesystem::path& input, const std::filesystem::path& output) {
  Bytes track;
  event(track, 0, {0xb2,0,1});
  event(track, 0, {32,2});             // two-byte CC running status
  event(track, 0, {0xc2,40});
  event(track, 0, {41});               // one-byte program running status
  event(track, 0, {0x92,60,100});
  event(track, 1, {0xb2,11,77});
  event(track, 1, {11,66});
  event(track, 1, {0xa2,60,70});
  event(track, 1, {61,0});             // poly pressure running status
  event(track, 1, {0xd2,90});
  event(track, 1, {0});                // one-byte pressure running status
  event(track, 1, {0xe2,0,0});         // bend minimum
  event(track, 1, {0,64});             // bend center
  event(track, 1, {127,127});          // bend maximum
  event(track, 1, {1,2});              // asymmetric bytes catch LSB/MSB swaps
  event(track, 1, {0xb2,64,127});
  event(track, 1, {0x82,60,37});
  event(track, 0, {0xb2,64,0});
  event(track, 1, {123,0});
  event(track, 0, {0xff,0x2f,0});
  save(input, smf(384, {track}));
  daw::MidiFile midi;
  daw::MidiImportReport report;
  std::string error;
  require(daw::readMidiFile(input.string(), &midi, &error, &report), error);
  require(midi.tracks.size() == 1 && midi.ticks_per_quarter == 960, "384 PPQ import metadata");
  require(report.source_ticks_per_quarter == 384 && report.preserved_channel_events == 17 &&
          report.rounded_channel_events == 7 && report.ignored_channel_events == 0 &&
          report.rounded_note_boundaries == 0, "channel preservation/rounding diagnostics");
  const auto& notes = midi.tracks[0].notes;
  require(notes.size() == 1 && notes[0].start == 0 && notes[0].duration == 30 &&
          notes[0].pitch == 60 && notes[0].velocity == 100 && notes[0].channel == 2 &&
          notes[0].release_velocity == 37 && notes[0].on_order == 5 && notes[0].off_order == 17,
          "note edge fields or release velocity were lost");
  const WireTrack expected{
      {0,{0xb2,0,1}}, {0,{0xb2,32,2}}, {0,{0xc2,40}}, {0,{0xc2,41}}, {0,{0x92,60,100}},
      {3,{0xb2,11,77}}, {5,{0xb2,11,66}}, {8,{0xa2,60,70}}, {10,{0xa2,61,0}},
      {13,{0xd2,90}}, {15,{0xd2,0}}, {18,{0xe2,0,0}}, {20,{0xe2,0,64}},
      {23,{0xe2,127,127}}, {25,{0xe2,1,2}}, {28,{0xb2,64,127}},
      {30,{0x82,60,37}}, {30,{0xb2,64,0}}, {33,{0xb2,123,0}}};
  const auto& channel_events = midi.tracks[0].channel_events;
  require(channel_events.size() == 17, "not all five channel-event families were imported");
  std::size_t channel_index = 0;
  for (std::size_t i = 0; i < expected.size(); ++i) {
    const auto& wire = expected[i];
    const unsigned type = wire.bytes[0] & 0xf0;
    if (type == 0x80 || type == 0x90) continue;
    const auto& stored = channel_events[channel_index++];
    require(stored.tick == wire.tick && static_cast<unsigned>(stored.type) == type &&
            stored.channel == 2 && stored.data1 == wire.bytes[1] &&
            stored.data2 == (wire.bytes.size() == 3 ? wire.bytes[2] : 0) && stored.order == i + 1,
            "channel-event model changed raw bytes, normalized absolute tick, or source ordinal");
  }
  for (std::size_t i = 10; i < 13; ++i) {
    const auto& bend = channel_events[i];
    const unsigned value = bend.data1 | (static_cast<unsigned>(bend.data2) << 7);
    const unsigned expected_value[] = {0,8192,16383};
    require(value == expected_value[i - 10], "14-bit pitch bend value changed");
  }
  checkExport(midi, output, {expected}, "all channel types at 384 PPQ");
}

void sameTickSourceOrder(const std::filesystem::path& input, const std::filesystem::path& output) {
  Bytes track;
  event(track, 0, {0xb0,0,3});
  event(track, 0, {0xb0,32,4});
  event(track, 0, {0xc0,6});
  event(track, 0, {0xb0,64,127});
  event(track, 0, {0x90,72,100});       // order intentionally opposes pitch sorting
  event(track, 0, {0x90,60,80});
  event(track, 100, {0x80,60,5});
  event(track, 0, {0xb0,64,0});        // release before pedal-up
  event(track, 0, {0x90,60,95});       // same-pitch retrigger after release
  event(track, 100, {0xb0,64,0});      // pedal-up before release
  event(track, 0, {0x80,72,9});
  event(track, 0, {0x80,60,10});
  event(track, 0, {0x90,60,90});
  event(track, 100, {0x90,65,70});     // source attack precedes another pitch's release
  event(track, 0, {0x80,60,11});
  event(track, 100, {0x90,65,0});      // zero-velocity note-on is a release
  event(track, 0, {0xff,0x2f,0});
  save(input, smf(960, {track}));
  daw::MidiFile midi;
  std::string error;
  require(daw::readMidiFile(input.string(), &midi, &error), error);
  require(midi.tracks[0].notes.size() == 5, "source ordering fixture note count");
  checkExport(midi, output, {{
      {0,{0xb0,0,3}}, {0,{0xb0,32,4}}, {0,{0xc0,6}}, {0,{0xb0,64,127}},
      {0,{0x90,72,100}}, {0,{0x90,60,80}}, {100,{0x80,60,5}}, {100,{0xb0,64,0}},
      {100,{0x90,60,95}}, {200,{0xb0,64,0}}, {200,{0x80,72,9}}, {200,{0x80,60,10}},
      {200,{0x90,60,90}}, {300,{0x90,65,70}}, {300,{0x80,60,11}}, {400,{0x80,65,0}}
  }}, "source ordering across notes and controls");
}

void eventOnlyTracks(const std::filesystem::path& input, const std::filesystem::path& output) {
  Bytes automation;
  event(automation, 0, {0xff,3,7,'c','o','n','t','r','o','l'});
  event(automation, 0, {0xbf,64,127});
  event(automation, 96, {0xcf,5});
  event(automation, 0, {0xdf,31});
  event(automation, 96, {0xbf,64,0});
  event(automation, 0, {0xff,0x2f,0});
  Bytes notes;
  event(notes, 0, {0x90,60,80});
  event(notes, 384, {0x80,60,17});
  event(notes, 0, {0xff,0x2f,0});
  const WireTrack expected_automation{{0,{0xbf,64,127}}, {240,{0xcf,5}},
                                      {240,{0xdf,31}}, {480,{0xbf,64,0}}};
  for (unsigned format : {0U,1U}) {
    save(input, smf(384, format == 0 ? std::vector<Bytes>{automation} : std::vector<Bytes>{notes,automation}, format));
    daw::MidiFile midi;
    daw::MidiImportReport report;
    std::string error;
    require(daw::readMidiFile(input.string(), &midi, &error, &report), error);
    require(midi.tracks.size() == format + 1 && midi.tracks.back().name == "control" &&
            midi.tracks.back().notes.empty() && midi.tracks.back().channel_events.size() == 4 &&
            report.preserved_channel_events == 4, "event-only track was dropped during import");
    std::vector<WireTrack> expected;
    if (format == 1) expected.push_back({{0,{0x90,60,80}}, {960,{0x80,60,17}}});
    expected.push_back(expected_automation);
    checkExport(midi, output, expected, "event-only track export");
    daw::MidiFile reread;
    require(daw::readMidiFile(output.string(), &reread, &error), error);
    require(reread.tracks.size() == format + 1 && reread.tracks.back().name == "control" &&
            reread.tracks.back().notes.empty() && reread.tracks.back().channel_events.size() == 4,
            "event-only track did not survive read/write/read");
  }
}

daw::MidiFile authoredFile() {
  daw::MidiFile midi;
  daw::MidiTrack track;
  track.name = "authored";
  track.notes = {{100,100,65,90,3,7}, {0,100,60,80,3,9}};
  track.channel_events = {{100,EventType::ControlChange,3,64,0},
                          {100,EventType::ProgramChange,3,6,0}};
  midi.tracks.push_back(track);
  return midi;
}
void defaultOrder(const std::filesystem::path& output) {
  const auto midi = authoredFile();
  const std::vector<WireTrack> expected{{{0,{0x93,60,80}}, {100,{0x83,60,9}},
      {100,{0xb3,64,0}}, {100,{0xc3,6}}, {100,{0x93,65,90}}, {200,{0x83,65,7}}}};
  checkExport(midi, output, expected, "default-zero release/channel/attack order");
  const auto first = load(output);
  checkExport(midi, output, expected, "repeat default-zero order");
  require(load(output) == first, "repeated export was not deterministic");
}

void mixedSourceAndDefaultRetriggers(const std::filesystem::path& output) {
  const auto verify = [&](const daw::MidiFile& midi, const WireTrack& expected,
                          const std::string& label) {
    checkExport(midi, output, {expected}, label);
    daw::MidiFile reread;
    daw::MidiImportReport report;
    std::string error;
    require(daw::readMidiFile(output.string(), &reread, &error, &report), label + ": " + error);
    require(reread.tracks.size() == 1 && reread.tracks[0].notes.size() == 2 &&
            report.overlapping_same_pitch_notes == 0, label + ": adjacent notes did not survive import");
    for (std::size_t i = 0; i < 2; ++i) {
      const auto& note = reread.tracks[0].notes[i];
      require(note.start == static_cast<daw::Tick>(i) * 960 && note.duration == 960 &&
              note.pitch == 60 && note.velocity == 100 && note.channel == 0 &&
              note.release_velocity == 0, label + ": retrigger changed a note boundary or field");
    }
  };
  daw::MidiFile midi;
  midi.tracks.resize(1);
  midi.tracks[0].notes = {{0,960,60,100,0,0,0,0}, {960,960,60,100,0,0,5,6}};
  const WireTrack adjacent{{0,{0x90,60,100}}, {960,{0x80,60,0}},
                           {960,{0x90,60,100}}, {1920,{0x80,60,0}}};
  // This minimal mixture previously emitted the imported attack before the
  // authored release at tick 960, producing a zero-duration note on reimport.
  verify(midi, adjacent, "authored release before imported same-pitch retrigger");

  midi.tracks[0].notes = {{0,960,60,100,0,0,5,6}, {960,960,60,100,0,0,0,0}};
  verify(midi, adjacent, "imported release before authored same-pitch retrigger");

  midi.tracks[0].notes = {{0,960,60,100,0,0,0,0}, {960,960,60,100,0,0,5,8}};
  // Only the new release gets elevated: imported attack/CC ordering must
  // still follow source ordinals even when that puts an attack before a CC.
  // A new CC retains its fallback position after all source-ordered events.
  for (std::uint64_t cc_order : {4U,6U}) {
    midi.tracks[0].channel_events = {{960,EventType::ControlChange,0,64,0,cc_order},
                                    {960,EventType::ControlChange,0,11,75,0}};
    WireTrack expected{{0,{0x90,60,100}}, {960,{0x80,60,0}}};
    if (cc_order < 5) {
      expected.push_back({960,{0xb0,64,0}});
      expected.push_back({960,{0x90,60,100}});
    } else {
      expected.push_back({960,{0x90,60,100}});
      expected.push_back({960,{0xb0,64,0}});
    }
    expected.push_back({960,{0xb0,11,75}});
    expected.push_back({1920,{0x80,60,0}});
    verify(midi, expected, "new release preserves imported CC/attack source order " +
                          std::to_string(cc_order));
  }

  // Moving imported notes can leave source ordinals that contradict their
  // new adjacency. Reject that conflict before opening the destination;
  // clearing the edited notes' ordinals selects the safe authored fallback.
  midi.tracks[0].notes = {{0,960,60,100,0,0,3,4}, {960,960,60,100,0,0,1,2}};
  midi.tracks[0].channel_events.clear();
  const Bytes sentinel{'u','n','t','o','u','c','h','e','d',0,255};
  save(output, sentinel);
  std::string error;
  require(!daw::writeMidiFile(midi, output.string(), &error),
          "stale source ordinals exported an invalid adjacent same-pitch retrigger");
  require(load(output) == sentinel, "stale source-order rejection modified the destination");
  require(error.find("clear orders on edited notes") != std::string::npos,
          "stale source-order rejection omitted the actionable repair diagnostic");
  for (auto& note : midi.tracks[0].notes) {
    note.on_order = 0;
    note.off_order = 0;
  }
  verify(midi, adjacent, "cleared source ordinals restore edited adjacent notes");
}

void invalidWriterFields(const std::filesystem::path& output) {
  using Mutator = std::function<void(daw::MidiFile&)>;
  const std::vector<std::pair<std::string,Mutator>> cases{
    {"nonzero program data2", [](auto& m) { m.tracks[0].channel_events[1].data2 = 1; }},
    {"nonzero channel pressure data2", [](auto& m) {
       auto& e = m.tracks[0].channel_events[1]; e.type = EventType::ChannelPressure; e.data2 = 1;
     }},
    {"unsupported event type", [](auto& m) { m.tracks[0].channel_events[0].type = static_cast<EventType>(0x90); }},
    {"negative channel tick", [](auto& m) { m.tracks[0].channel_events[0].tick = -1; }},
    {"channel 16", [](auto& m) { m.tracks[0].channel_events[0].channel = 16; }},
    {"data1 128", [](auto& m) { m.tracks[0].channel_events[0].data1 = 128; }},
    {"data2 128", [](auto& m) { m.tracks[0].channel_events[0].data2 = 128; }},
    {"release velocity 128", [](auto& m) { m.tracks[0].notes[0].release_velocity = 128; }}
  };
  const Bytes sentinel{'k','e','e','p',0,255};
  for (const auto& test : cases) {
    auto midi = authoredFile();
    test.second(midi);
    save(output, sentinel);
    std::string error;
    require(!daw::writeMidiFile(midi, output.string(), &error), test.first + " was exported");
    require(!error.empty() && load(output) == sentinel,
            test.first + " modified the destination or supplied no diagnostic");
  }
}

void malformedReadIsAtomic(const std::filesystem::path& input) {
  const std::vector<std::pair<std::string,Bytes>> malformed{
    {"truncated CC", {0,0xb0,64}},
    {"truncated poly pressure", {0,0xa0,60}},
    {"truncated pitch bend", {0,0xe0,0}},
    {"truncated program", {0,0xc0}},
    {"truncated channel pressure", {0,0xd0}},
    {"truncated running two-byte event", {0,0xb0,11,99,1,64}},
    {"truncated running one-byte event", {0,0xc0,5,1}},
    {"status in channel data", {0,0xb0,64,0xff,0x2f,0}}
  };
  for (const auto& test : malformed) {
    save(input, smf(384, {test.second}));
    auto midi = authoredFile();
    midi.format = 0;
    midi.tempo.addChange(100, 75);
    midi.tracks[0].channel_events[0].order = 31;
    daw::MidiImportReport report;
    report.source_ticks_per_quarter = 42;
    report.rounded_note_boundaries = 43;
    report.rounded_tempo_events = 44;
    report.ignored_channel_events = 45;
    report.preserved_channel_events = 46;
    report.rounded_channel_events = 47;
    report.ignored_meta_events = 48;
    report.ignored_sysex_events = 49;
    report.ignored_time_signature_events = 50;
    report.overlapping_same_pitch_notes = 51;
    std::string error;
    require(!daw::readMidiFile(input.string(), &midi, &error, &report), test.first + " was accepted");
    require(!error.empty() && midi.format == 0 && midi.ticks_per_quarter == 960 &&
            midi.tracks.size() == 1 && midi.tracks[0].name == "authored" &&
            midi.tracks[0].notes.size() == 2 && midi.tracks[0].notes[0].start == 100 &&
            midi.tracks[0].notes[0].release_velocity == 7 && midi.tracks[0].channel_events.size() == 2 &&
            midi.tracks[0].channel_events[0].tick == 100 && midi.tracks[0].channel_events[0].order == 31 &&
            midi.tempo.changes().size() == 2 && midi.tempo.changes().back().bpm == 75,
            test.first + " changed the caller's MIDI model");
    require(report.source_ticks_per_quarter == 42 && report.rounded_note_boundaries == 43 &&
            report.rounded_tempo_events == 44 && report.ignored_channel_events == 45 &&
            report.preserved_channel_events == 46 && report.rounded_channel_events == 47 &&
            report.ignored_meta_events == 48 && report.ignored_sysex_events == 49 &&
            report.ignored_time_signature_events == 50 && report.overlapping_same_pitch_notes == 51,
            test.first + " changed the caller's import diagnostics");
  }
}
}  // namespace

int main() {
  const auto directory = std::filesystem::temp_directory_path();
  const auto input = directory / "classical_daw_channel_events_fixture.mid";
  const auto output = directory / "classical_daw_channel_events_output.mid";
  try {
    allTypesAndRounding(input, output);
    sameTickSourceOrder(input, output);
    eventOnlyTracks(input, output);
    defaultOrder(output);
    mixedSourceAndDefaultRetriggers(output);
    invalidWriterFields(output);
    malformedReadIsAtomic(input);
    std::filesystem::remove(input);
    std::filesystem::remove(output);
    std::cout << "MIDI channel event regression tests passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::filesystem::remove(input);
    std::filesystem::remove(output);
    std::cerr << e.what() << '\n';
    return 1;
  }
}
