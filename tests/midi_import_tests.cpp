#include "daw/midi.hpp"
#include "daw/score_midi.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Bytes = std::vector<std::uint8_t>;
void require(bool value, const std::string& message) {
  if (!value) throw std::runtime_error(message);
}
void u32(Bytes& out, std::size_t n) {
  for (int shift : {24, 16, 8, 0}) out.push_back(static_cast<std::uint8_t>(n >> shift));
}
void vlq(Bytes& out, unsigned n) {
  unsigned shift = 0;
  for (unsigned value = n; value > 127; value >>= 7) shift += 7;
  for (;;) {
    out.push_back(static_cast<std::uint8_t>(((n >> shift) & 127) | (shift ? 128 : 0)));
    if (!shift) break;
    shift -= 7;
  }
}
void event(Bytes& track, unsigned delta, std::initializer_list<std::uint8_t> payload) {
  vlq(track, delta);
  track.insert(track.end(), payload);
}
Bytes file(unsigned ppq, const std::vector<Bytes>& tracks, unsigned format = 0) {
  Bytes out{'M','T','h','d',0,0,0,6,0,static_cast<std::uint8_t>(format),
    0,static_cast<std::uint8_t>(tracks.size()),static_cast<std::uint8_t>(ppq >> 8),static_cast<std::uint8_t>(ppq)};
  for (const auto& track : tracks) {
    out.insert(out.end(), {'M','T','r','k'});
    u32(out, track.size());
    out.insert(out.end(), track.begin(), track.end());
  }
  return out;
}
Bytes singleNote(unsigned start, unsigned duration) {
  Bytes track;
  event(track, start, {0x92,60,87});
  event(track, duration, {0x82,60,0});
  event(track, 0, {0xff,0x2f,0});
  return track;
}
void save(const std::filesystem::path& path, const Bytes& bytes) {
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  require(static_cast<bool>(out), "fixture write failed");
}
}

int main() {
  const auto path = std::filesystem::temp_directory_path() / "classical_daw_ppq_import.mid";
  const auto roundtrip = std::filesystem::temp_directory_path() / "classical_daw_ppq_roundtrip.mid";
  try {
    daw::MidiFile midi;
    daw::MidiImportReport report;
    std::string error;
    // Three exact LilyPond-style eighth-note triplets: 128 / 384 quarters.
    Bytes triplets;
    event(triplets, 0, {0xff,0x51,3,0x07,0xa1,0x20});
    for (std::uint8_t pitch : {60,62,64}) {
      event(triplets, 0, {0x90,pitch,91});
      event(triplets, 128, {0x90,pitch,0});
    }
    event(triplets, 0, {0xff,0x51,3,0x0f,0x42,0x40});
    event(triplets, 0, {0xff,0x2f,0});
    save(path, file(384, {triplets}));
    require(daw::readMidiFile(path.string(), &midi, &error, &report), error);
    require(midi.ticks_per_quarter == 960 && report.source_ticks_per_quarter == 384, "PPQ metadata");
    require(midi.tracks[0].notes.size() == 3 && report.rounded_note_boundaries == 0, "exact triplet count");
    for (int i = 0; i < 3; ++i) {
      const auto& note = midi.tracks[0].notes[static_cast<std::size_t>(i)];
      require(note.start == i * 320 && note.duration == 320, "triplet was quantized to a coarser grid");
    }
    require(midi.tempo.changes().back().tick == 960 &&
            std::abs(midi.tempo.tickToSeconds(1920) - 1.5) < 1e-9, "tempo event not normalized");
    daw::Score score;
    require(daw::readMidiScoreFile(path.string(), &score, &error) &&
            score.parts[0].measures[0].notes[1].start == 320, "file-to-score 384 PPQ path");

    // Odd 384-PPQ absolute boundaries round half up independently. Repeated
    // 1-tick deltas must not introduce cumulative drift.
    Bytes fractional;
    event(fractional, 0, {0xc0,40});
    event(fractional, 0, {0xb0,11,70});
    event(fractional, 1, {0x90,60,95});
    for (int i = 0; i < 127; ++i) event(fractional, 1, {0xb0,11,70});
    event(fractional, 1, {0x80,60,0});
    event(fractional, 64, {0xff,0x51,3,0x0f,0x42,0x40});
    event(fractional, 0, {0xff,0x2f,0});
    save(path, file(384, {fractional}));
    require(daw::readMidiFile(path.string(), &midi, &error, &report), error);
    require(midi.tracks[0].notes[0].start == 3 && midi.tracks[0].notes[0].end() == 323,
            "absolute-boundary rounding drift");
    require(report.rounded_note_boundaries == 2 && report.rounded_tempo_events == 1 &&
            report.ignored_channel_events == 129, "rounding/content loss report");
    require(midi.tempo.changes().back().tick == 483, "fractional tempo position");
    require(daw::writeMidiFile(midi, roundtrip.string(), &error), error);
    daw::MidiFile reloaded;
    require(daw::readMidiFile(roundtrip.string(), &reloaded, &error) &&
            reloaded.tracks[0].notes[0].start == 3 && reloaded.tracks[0].notes[0].end() == 323,
            "canonical file round-trip");

    for (unsigned ppq : {1U,96U,384U,480U,960U,1000U,1920U,32767U}) {
      save(path, file(ppq, {singleNote(ppq, ppq)}));
      require(daw::readMidiFile(path.string(), &midi, &error, &report), error);
      const auto& n = midi.tracks[0].notes[0];
      require(n.start == 960 && n.end() == 1920 && n.channel == 2 && n.velocity == 87,
              "supported PPQ import or channel/velocity changed");
    }
    // Diagnostics distinguish musical content that the current model skips
    // from ambiguous same-pitch overlap pairing, without hiding either.
    Bytes diagnosed;
    event(diagnosed, 0, {0xff,0x58,4,4,2,24,8});
    event(diagnosed, 0, {0xff,0x01,1,'x'});
    event(diagnosed, 0, {0xf0,2,0x7e,0xf7});
    event(diagnosed, 0, {0x90,60,50});
    event(diagnosed, 100, {0x90,60,80});
    event(diagnosed, 100, {0x80,60,0});
    event(diagnosed, 100, {0x80,60,0});
    event(diagnosed, 100, {0xff,0x58,4,3,2,24,8});
    event(diagnosed, 0, {0xff,0x2f,0});
    save(path, file(960, {diagnosed}));
    require(daw::readMidiFile(path.string(), &midi, &error, &report), error);
    require(report.ignored_meta_events == 1 && report.ignored_sysex_events == 1 &&
            report.ignored_time_signature_events == 1 && report.overlapping_same_pitch_notes == 1 &&
            report.ignored_channel_events == 0 && report.rounded_note_boundaries == 0,
            "lost event classes or ambiguity missing from report");
    require(midi.tracks[0].notes[0].duration == 300 && midi.tracks[0].notes[1].duration == 100 &&
            midi.time_signature.numerator == 4, "documented LIFO pairing or first meter changed");
    // Velocity-zero note-on means note-off in MIDI. Reject before touching an
    // existing destination, rather than writing a file our own reader rejects.
    const Bytes destination_bytes{'k','e','e','p'};
    save(roundtrip, destination_bytes);
    midi.tracks[0].notes[0].velocity = 0;
    require(!daw::writeMidiFile(midi, roundtrip.string(), &error), "zero attack velocity exported");
    std::ifstream destination(roundtrip, std::ios::binary);
    const Bytes after((std::istreambuf_iterator<char>(destination)), std::istreambuf_iterator<char>());
    require(after == destination_bytes, "failed export modified destination");
    const auto reject = [&](const Bytes& bytes, const std::string& label) {
      save(path, bytes);
      daw::MidiFile sentinel;
      sentinel.tracks = {{"keep", {{123,456,60,70,1}}}};
      daw::MidiImportReport sentinel_report;
      sentinel_report.source_ticks_per_quarter = 42;
      require(!daw::readMidiFile(path.string(), &sentinel, &error, &sentinel_report), label + " was accepted");
      require(sentinel.tracks.size() == 1 && sentinel.tracks[0].name == "keep" &&
              sentinel.tracks[0].notes[0].start == 123 && sentinel_report.source_ticks_per_quarter == 42 &&
              !error.empty(), label + " changed output on failure");
    };
    reject(file(0, {singleNote(0, 1)}), "zero PPQ");
    reject(file(0xe728, {singleNote(0, 1)}), "SMPTE");
    reject(file(960, {singleNote(0, 960)}, 2), "Type 2");
    reject(file(960, {singleNote(0, 960),singleNote(0, 960)}), "Type 0 multiple tracks");
    reject(file(32767, {singleNote(1, 1)}), "collapsed note");
    reject(file(960, {singleNote(0, 0)}), "zero duration");
    reject(file(960, {{0,0x80,60,0,0,0xff,0x2f,0}}), "orphan note-off");
    reject(file(960, {{0,0x90,60,90,0,0xff,0x2f,0}}), "unclosed note");
    reject(file(960, {{0,0xff,0x2f,0,0}}), "data after EOT");
    reject(file(960, {{0x81},singleNote(0,960)}, 1), "VLQ crosses track boundary");
    reject(file(960, {{0,0xff,1,0x81},singleNote(0,960)}, 1), "meta VLQ crosses track boundary");
    reject(file(960, {{0,0xf0,0x81},singleNote(0,960)}, 1), "SysEx VLQ crosses track boundary");
    std::filesystem::remove(path);
    std::filesystem::remove(roundtrip);
    std::cout << "MIDI PPQ/import regression tests passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::filesystem::remove(path);
    std::filesystem::remove(roundtrip);
    std::cerr << e.what() << '\n';
    return 1;
  }
}
