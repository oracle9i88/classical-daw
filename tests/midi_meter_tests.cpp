#include "daw/midi.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;
using MeterRow = std::tuple<std::size_t, daw::Tick, unsigned, unsigned, unsigned, unsigned>;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

struct TestFiles {
  std::filesystem::path directory = std::filesystem::temp_directory_path() /
      ("classical_daw_midi_meter_" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  TestFiles() { std::filesystem::create_directory(directory); }
  ~TestFiles() {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
};

void u32(Bytes& out, std::size_t value) {
  for (int shift : {24, 16, 8, 0}) out.push_back(static_cast<std::uint8_t>(value >> shift));
}

void vlq(Bytes& out, unsigned value) {
  unsigned shift = 0;
  for (unsigned rest = value; rest > 127; rest >>= 7) shift += 7;
  for (;;) {
    out.push_back(static_cast<std::uint8_t>(((value >> shift) & 127) | (shift ? 128 : 0)));
    if (shift == 0) break;
    shift -= 7;
  }
}

void event(Bytes& track, unsigned delta, std::initializer_list<std::uint8_t> bytes) {
  vlq(track, delta);
  track.insert(track.end(), bytes);
}

Bytes smf(unsigned ppq, const std::vector<Bytes>& tracks) {
  Bytes bytes{'M', 'T', 'h', 'd', 0, 0, 0, 6, 0,
              static_cast<std::uint8_t>(tracks.size() == 1 ? 0 : 1),
              static_cast<std::uint8_t>(tracks.size() >> 8), static_cast<std::uint8_t>(tracks.size()),
              static_cast<std::uint8_t>(ppq >> 8), static_cast<std::uint8_t>(ppq)};
  for (const auto& track : tracks) {
    bytes.insert(bytes.end(), {'M', 'T', 'r', 'k'});
    u32(bytes, track.size());
    bytes.insert(bytes.end(), track.begin(), track.end());
  }
  return bytes;
}

void save(const std::filesystem::path& path, const Bytes& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  require(static_cast<bool>(output), "fixture write failed");
}

Bytes load(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  require(static_cast<bool>(input), "fixture read failed");
  return Bytes(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

// This byte decoder deliberately does not call the production MIDI reader.
// It verifies raw 0x58 payloads, their absolute ticks and conductor placement.
std::vector<MeterRow> decodeMeters(const Bytes& bytes) {
  std::size_t at = 0;
  const auto number = [&](unsigned width) {
    require(width <= 4 && at + width <= bytes.size(), "decoder truncated integer");
    std::uint32_t value = 0;
    for (unsigned i = 0; i < width; ++i) value = (value << 8) | bytes[at++];
    return value;
  };
  require(number(4) == 0x4d546864 && number(4) == 6, "decoder MIDI header");
  (void)number(2);
  const auto track_count = number(2);
  require(number(2) == 960, "writer did not emit 960 PPQ");
  std::vector<MeterRow> result;
  for (std::size_t track = 0; track < track_count; ++track) {
    require(number(4) == 0x4d54726b, "decoder track header");
    const auto length = number(4);
    require(length <= bytes.size() - at, "decoder track length");
    const auto end = at + length;
    const auto variable = [&]() {
      unsigned value = 0;
      for (unsigned i = 0; i < 4; ++i) {
        require(at < end, "decoder truncated VLQ");
        const auto byte = bytes[at++];
        value = (value << 7) | (byte & 127);
        if ((byte & 128) == 0) return value;
      }
      throw std::runtime_error("decoder invalid VLQ");
    };
    daw::Tick tick = 0;
    std::uint8_t running = 0;
    bool eot = false;
    while (at < end) {
      tick += variable();
      require(at < end, "decoder truncated status");
      std::uint8_t status = bytes[at++];
      if (status < 0x80) {
        require(running != 0, "decoder missing running status");
        --at;
        status = running;
      } else if (status < 0xf0) running = status;
      if (status == 0xff) {
        running = 0;
        require(at < end, "decoder truncated meta type");
        const auto type = bytes[at++];
        const auto size = variable();
        require(size <= end - at, "decoder truncated meta payload");
        if (type == 0x58) {
          require(size == 4, "writer meter length");
          result.emplace_back(track, tick, bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]);
        }
        if (type == 0x2f) {
          require(size == 0 && at == end, "writer EOT placement");
          eot = true;
        }
        at += size;
      } else {
        require(status >= 0x80 && status < 0xf0, "unexpected writer system event");
        const auto command = status & 0xf0;
        const unsigned size = (command == 0xc0 || command == 0xd0) ? 1 : 2;
        require(size <= end - at, "decoder truncated channel payload");
        at += size;
      }
    }
    require(eot, "writer missing EOT");
  }
  require(at == bytes.size(), "writer trailing bytes");
  return result;
}

bool same(const daw::TimeSignature& a, const daw::TimeSignature& b) {
  return a.numerator == b.numerator && a.denominator == b.denominator &&
         a.clocks_per_click == b.clocks_per_click &&
         a.notated_32nds_per_quarter == b.notated_32nds_per_quarter;
}

void run() {
  TestFiles files;
  const auto input = files.directory / "input.mid";
  const auto output = files.directory / "output.mid";
  daw::MidiFile midi;
  daw::MidiImportReport report;
  std::string error;

  // All four source bytes survive; per-track encounter order resolves
  // duplicate normalized positions, without accumulating rounded deltas.
  Bytes first;
  event(first, 0, {0x90, 60, 90});
  event(first, 0, {0xff, 0x58, 4, 6, 3, 36, 8});
  event(first, 1, {0xff, 0x58, 4, 7, 4, 5, 16});
  event(first, 1, {0xff, 0x58, 4, 5, 3, 0, 4});
  event(first, 0, {0xff, 0x58, 4, 11, 5, 128, 255});
  event(first, 382, {0x80, 60, 0});
  event(first, 0, {0xff, 0x2f, 0});
  Bytes second;
  event(second, 0, {0xff, 0x58, 4, 9, 4, 255, 1});
  event(second, 1, {0xff, 0x58, 4, 3, 1, 72, 8});
  event(second, 383, {0xff, 0x58, 4, 4, 2, 24, 8});
  event(second, 0, {0xff, 0x2f, 0});
  save(input, smf(384, {first, second}));
  require(daw::readMidiFile(input.string(), &midi, &error, &report), error);
  require(same(midi.time_signature, {9, 16, 255, 1}) && midi.meter_changes.size() == 3,
          "initial meter or canonical map size");
  require(midi.meter_changes[0].tick == 3 && same(midi.meter_changes[0].signature, {3, 2, 72, 8}) &&
          midi.meter_changes[1].tick == 5 && same(midi.meter_changes[1].signature, {11, 32, 128, 255}) &&
          midi.meter_changes[2].tick == 960 && same(midi.meter_changes[2].signature, {4, 4, 24, 8}),
          "last encountered meter or absolute rounding");
  require(report.source_ticks_per_quarter == 384 && report.preserved_time_signature_events == 7 &&
          report.rounded_time_signature_events == 2 && report.coalesced_time_signature_events == 3 &&
          report.ignored_time_signature_events == 0, "meter diagnostics");
  require(midi.tracks[0].notes[0].end() == 960 && midi.tracks[1].notes.empty(),
          "meter import changed notes or metadata-only track");
  require(daw::writeMidiFile(midi, output.string(), &error), error);
  require(decodeMeters(load(output)) == std::vector<MeterRow>{
      {0, 0, 9, 4, 255, 1}, {0, 3, 3, 1, 72, 8},
      {0, 5, 11, 5, 128, 255}, {0, 960, 4, 2, 24, 8}}, "raw writer meter payload/tick/track");

  // A first signature in the middle must not be backdated to zero.
  Bytes late;
  event(late, 384, {0xff, 0x58, 4, 3, 3, 0, 8});
  event(late, 0, {0xff, 0x2f, 0});
  save(input, smf(384, {late}));
  require(daw::readMidiFile(input.string(), &midi, &error, &report), error);
  require(same(midi.time_signature, {}) && midi.meter_changes.size() == 1 &&
          midi.meter_changes[0].tick == 960 && same(midi.meter_changes[0].signature, {3, 8, 0, 8}) &&
          report.preserved_time_signature_events == 1 && report.coalesced_time_signature_events == 0,
          "late first meter replaced implicit initial 4/4");
  require(daw::writeMidiFile(midi, output.string(), &error), error);
  require(decodeMeters(load(output)) == std::vector<MeterRow>{{0, 0, 4, 2, 24, 8}, {0, 960, 3, 3, 0, 8}},
          "metadata-only meter map export");

  // Different source positions may collapse to one engine tick. The first
  // explicit tick-zero event is not a duplicate of the implicit default.
  Bytes collapsed;
  event(collapsed, 1, {0xff, 0x58, 4, 5, 2, 24, 8});
  event(collapsed, 1, {0xff, 0x58, 4, 7, 3, 36, 8});
  event(collapsed, 0, {0xff, 0x2f, 0});
  save(input, smf(32767, {collapsed}));
  require(daw::readMidiFile(input.string(), &midi, &error, &report) &&
          same(midi.time_signature, {7, 8, 36, 8}) && midi.meter_changes.empty() &&
          report.preserved_time_signature_events == 2 && report.coalesced_time_signature_events == 1 &&
          report.rounded_time_signature_events == 2, "rounded duplicate at zero");
  save(input, smf(960, {{0, 0xff, 0x2f, 0}}));
  require(daw::readMidiFile(input.string(), &midi, &error, &report) && same(midi.time_signature, {}) &&
          midi.meter_changes.empty() && report.preserved_time_signature_events == 0 &&
          report.rounded_time_signature_events == 0 && report.coalesced_time_signature_events == 0,
          "absent meters or report reset");

  const auto reject = [&](const Bytes& bytes, const std::string& label) {
    save(input, bytes);
    daw::MidiFile sentinel;
    sentinel.time_signature = {7, 16, 91, 13};
    sentinel.meter_changes = {{960, {5, 8, 17, 3}}};
    sentinel.tracks = {{"untouched", {}}};
    daw::MidiImportReport diagnostic;
    diagnostic.preserved_time_signature_events = 81;
    diagnostic.rounded_time_signature_events = 82;
    diagnostic.coalesced_time_signature_events = 83;
    require(!daw::readMidiFile(input.string(), &sentinel, &error, &diagnostic) && !error.empty(), label);
    require(same(sentinel.time_signature, {7, 16, 91, 13}) && sentinel.meter_changes.size() == 1 &&
            sentinel.meter_changes[0].tick == 960 && same(sentinel.meter_changes[0].signature, {5, 8, 17, 3}) &&
            sentinel.tracks[0].name == "untouched" && diagnostic.preserved_time_signature_events == 81 &&
            diagnostic.rounded_time_signature_events == 82 && diagnostic.coalesced_time_signature_events == 83,
            label + " changed caller state");
  };
  for (const auto& payload : std::vector<Bytes>{{0, 2, 24, 8}, {4, 8, 24, 8}, {4, 2, 24, 0}}) {
    Bytes bad{0, 0xff, 0x58, 4};
    bad.insert(bad.end(), payload.begin(), payload.end());
    event(bad, 0, {0xff, 0x2f, 0});
    reject(smf(960, {bad}), "invalid raw meter accepted");
  }
  reject(smf(960, {{0, 0xff, 0x58, 3, 4, 2, 24, 0, 0xff, 0x2f, 0}}), "bad meter length accepted");
  reject(smf(960, {{0, 0xff, 0x58, 4, 4, 2, 24}}), "truncated meter accepted");

  // The raw cap includes one initial event plus up to a million later
  // changes. Repeated events cannot bypass it by keeping the map small.
  Bytes flood;
  flood.reserve(8'000'020);
  for (std::size_t i = 0; i < 1'000'002; ++i) event(flood, 0, {0xff, 0x58, 4, 4, 2, 24, 8});
  event(flood, 0, {0xff, 0x2f, 0});
  reject(smf(960, {flood}), "raw duplicate event cap bypassed");

  const Bytes keep{'k', 'e', 'e', 'p'};
  const std::vector<std::function<void(daw::MidiFile&)>> invalid_maps{
      [](auto& f) { f.time_signature.numerator = 0; },
      [](auto& f) { f.time_signature.denominator = 3; },
      [](auto& f) { f.time_signature.notated_32nds_per_quarter = 0; },
      [](auto& f) { f.meter_changes = {{0, {3, 4}}}; },
      [](auto& f) { f.meter_changes = {{-1, {3, 4}}}; },
      [](auto& f) { f.meter_changes = {{960, {3, 4}}, {960, {6, 8}}}; },
      [](auto& f) { f.meter_changes = {{960, {3, 4}}, {480, {6, 8}}}; },
      [](auto& f) { f.meter_changes = {{960, {3, 0}}}; },
      [](auto& f) { f.meter_changes = {{960, {3, 4, 24, 0}}}; },
      [](auto& f) { f.meter_changes.resize(1'000'001, {1, {3, 4}}); },
  };
  for (const auto& mutate : invalid_maps) {
    daw::MidiFile invalid;
    mutate(invalid);
    save(output, keep);
    require(!daw::writeMidiFile(invalid, output.string(), &error) && !error.empty() && load(output) == keep,
            "invalid meter map accepted or touched destination");
  }

  // Meta payload bytes are full bytes, not channel-message 7-bit fields.
  daw::MidiFile extremes;
  extremes.time_signature = {255, 128, 255, 255};
  extremes.meter_changes = {{1, {1, 1, 0, 1}}};
  require(daw::writeMidiFile(extremes, output.string(), &error), error);
  require(decodeMeters(load(output)) == std::vector<MeterRow>{{0, 0, 255, 7, 255, 255}, {0, 1, 1, 0, 0, 1}},
          "meter boundary bytes or denominator exponents changed");
}

}  // namespace

int main() {
  try {
    run();
    std::cout << "MIDI meter-map regression tests passed\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "FAIL: " << exception.what() << '\n';
    return 1;
  }
}
