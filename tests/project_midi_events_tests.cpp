#include "daw/history.hpp"
#include "daw/project.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace daw;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

struct TestFiles {
  std::filesystem::path directory = std::filesystem::temp_directory_path() /
      ("classical_daw_project_midi_" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  TestFiles() { std::filesystem::create_directory(directory); }
  ~TestFiles() {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
};

std::string readText(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  require(static_cast<bool>(input), "read fixture");
  std::ostringstream output;
  output << input.rdbuf();
  return output.str();
}

void writeText(const std::filesystem::path& path, const std::string& text) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << text;
  require(static_cast<bool>(output), "write fixture");
}

std::string replace(const std::string& text, const std::string& from, const std::string& to) {
  std::string result = text;
  const auto position = result.find(from);
  require(position != std::string::npos, "missing mutation target: " + from);
  result.replace(position, from.size(), to);
  return result;
}

bool equal(const Score& a, const Score& b) {
  if (a.divisions != b.divisions || a.bpm != b.bpm ||
      a.time_signature.numerator != b.time_signature.numerator ||
      a.time_signature.denominator != b.time_signature.denominator || a.parts.size() != b.parts.size()) return false;
  for (std::size_t p = 0; p < a.parts.size(); ++p) {
    const auto& x = a.parts[p];
    const auto& y = b.parts[p];
    if (x.id != y.id || x.name != y.name || x.measures.size() != y.measures.size() ||
        x.midi_events.size() != y.midi_events.size()) return false;
    for (std::size_t e = 0; e < x.midi_events.size(); ++e) {
      const auto& xe = x.midi_events[e];
      const auto& ye = y.midi_events[e];
      if (xe.tick != ye.tick || xe.type != ye.type || xe.channel != ye.channel || xe.data1 != ye.data1 ||
          xe.data2 != ye.data2 || xe.order != ye.order) return false;
    }
    for (std::size_t m = 0; m < x.measures.size(); ++m) {
      const auto& xm = x.measures[m];
      const auto& ym = y.measures[m];
      if (xm.number != ym.number || xm.start != ym.start || xm.notes.size() != ym.notes.size()) return false;
      for (std::size_t n = 0; n < xm.notes.size(); ++n) {
        const auto& xn = xm.notes[n];
        const auto& yn = ym.notes[n];
        if (xn.start != yn.start || xn.duration != yn.duration || xn.pitch.step != yn.pitch.step ||
            xn.pitch.alter != yn.pitch.alter || xn.pitch.octave != yn.pitch.octave || xn.rest != yn.rest ||
            xn.chord != yn.chord || xn.tie_start != yn.tie_start || xn.tie_stop != yn.tie_stop ||
            xn.velocity != yn.velocity || xn.voice != yn.voice || xn.staff != yn.staff ||
            xn.tuplet_actual != yn.tuplet_actual || xn.tuplet_normal != yn.tuplet_normal || xn.lyric != yn.lyric ||
            xn.midi_channel != yn.midi_channel || xn.midi_on_order != yn.midi_on_order ||
            xn.midi_off_order != yn.midi_off_order || xn.midi_release_velocity != yn.midi_release_velocity) return false;
      }
    }
  }
  return true;
}

Score fixture() {
  Score score;
  score.parts = {
      ScorePart{"P1", "Performance", {ScoreMeasure{1, 0, {ScoreNote{}, ScoreNote{}}}}, {}},
      ScorePart{"P2", "Events only", {}, {}},
  };
  score.parts[0].measures[0].notes[1].start = 960;
  auto& note = score.parts[0].measures[0].notes[0];
  note.lyric = "元数据";
  note.midi_channel = 15;
  note.midi_on_order = std::numeric_limits<std::uint64_t>::max();
  note.midi_off_order = std::numeric_limits<std::uint64_t>::max() - 1;
  note.midi_release_velocity = 127;
  score.parts[0].midi_events = {
      {0, MidiChannelEventType::ControlChange, 15, 64, 127, 9007199254740993ULL},
      {0, MidiChannelEventType::PolyPressure, 0, 60, 127, 9007199254740992ULL},
      {960, MidiChannelEventType::PitchBend, 3, 0, 64, 0},
  };
  score.parts[1].midi_events = {
      {0, MidiChannelEventType::ProgramChange, 1, 127, 0, std::numeric_limits<std::uint64_t>::max()},
      {1920, MidiChannelEventType::ChannelPressure, 2, 53, 0, 42},
  };
  return score;
}

void run() {
  TestFiles files;
  const auto path = files.directory / "performance.cdaw";
  const auto invalid_path = files.directory / "invalid.cdaw";
  const Score original = fixture();
  std::string error;
  require(writeProjectFile(original, path.string(), &error), "write v3: " + error);
  const std::string serialized = readText(path);
  require(serialized.rfind("CLASSICAL_DAW_PROJECT 3\n", 0) == 0, "v3 header");
  Score loaded;
  require(readProjectFile(path.string(), &loaded, &error) && equal(loaded, original),
          "all MIDI event types, full uint64 orders and event-only part round trip: " + error);

  // Real legacy layouts omit every new field, rather than accepting v3 data
  // under an old header. Existing note data and v2 lyrics remain intact.
  for (const int version : {1, 2}) {
    std::istringstream input(serialized);
    std::ostringstream legacy;
    std::string line;
    while (std::getline(input, line)) {
      if (line.rfind("midi_", 0) == 0 || (version == 1 && line.rfind("lyric ", 0) == 0)) continue;
      if (line.rfind("CLASSICAL_DAW_PROJECT ", 0) == 0) line = "CLASSICAL_DAW_PROJECT " + std::to_string(version);
      legacy << line << '\n';
    }
    Score expected = original;
    for (auto& part : expected.parts) {
      part.midi_events.clear();
      for (auto& measure : part.measures) {
        for (auto& note : measure.notes) {
          note.midi_channel = -1;
          note.midi_on_order = note.midi_off_order = 0;
          note.midi_release_velocity = 0;
          if (version == 1) note.lyric.clear();
        }
      }
    }
    writeText(invalid_path, legacy.str());
    loaded = original;
    require(readProjectFile(invalid_path.string(), &loaded, &error) && equal(loaded, expected),
            "legacy MIDI defaults, version " + std::to_string(version));
  }

  // Parsing must reject malformed or out-of-range metadata before replacing
  // a caller's score. Include values that would otherwise wrap in narrow casts.
  const std::vector<std::pair<std::string, std::string>> invalid_fields = {
      {"midi_channel 15\n", "midi_channel -2\n"},
      {"midi_channel 15\n", "midi_channel 16\n"},
      {"midi_channel 15\n", "midi_channel 65535\n"},
      {"midi_release_velocity 127\n", "midi_release_velocity 128\n"},
      {"midi_release_velocity 127\n", "midi_release_velocity 256\n"},
      {"midi_on_order 18446744073709551615\n", "midi_on_order 18446744073709551616\n"},
      {"midi_off_order 18446744073709551614\n", "midi_off_order -1\n"},
      {"velocity 100\n", "velocity 256\n"},
      {"velocity 100\n", "velocity 128\n"},
      {"midi_event 0 176 15 64 127 9007199254740993\n", "midi_event -1 176 15 64 127 0\n"},
      {"midi_event 0 176 15 64 127 9007199254740993\n", "midi_event 0 144 15 64 127 0\n"},
      {"midi_event 0 176 15 64 127 9007199254740993\n", "midi_event 0 432 15 64 127 0\n"},
      {"midi_event 0 176 15 64 127 9007199254740993\n", "midi_event 0 176 16 64 127 0\n"},
      {"midi_event 0 176 15 64 127 9007199254740993\n", "midi_event 0 176 256 64 127 0\n"},
      {"midi_event 0 176 15 64 127 9007199254740993\n", "midi_event 0 176 15 128 127 0\n"},
      {"midi_event 0 176 15 64 127 9007199254740993\n", "midi_event 0 176 15 64 256 0\n"},
      {"midi_event 0 176 15 64 127 9007199254740993\n", "midi_event 0 176 15 64 127 -1\n"},
      {"midi_event 0 176 15 64 127 9007199254740993\n", "midi_event 0 176 15 64 127 18446744073709551616\n"},
      {"midi_event 0 192 1 127 0 18446744073709551615\n", "midi_event 0 192 1 127 1 0\n"},
      {"midi_event 1920 208 2 53 0 42\n", "midi_event 1920 208 2 53 1 42\n"},
      {"midi_events 3\n", "midi_events 1000001\n"},
      {"midi_events 3\n", "midi_events 18446744073709551616\n"},
      {"midi_events 3\n", "midi_events -1\n"},
      {"end_project\n", "end_project\nmidi_events 0\n"},
  };
  for (const auto& mutation : invalid_fields) {
    writeText(invalid_path, replace(serialized, mutation.first, mutation.second));
    loaded = original;
    require(!readProjectFile(invalid_path.string(), &loaded, &error) && !error.empty(),
            "accepted invalid field: " + mutation.second);
    require(equal(loaded, original), "invalid read changed caller score: " + mutation.second);
  }
  writeText(invalid_path, serialized.substr(0, serialized.find("midi_events 2\n") + 14));
  require(!readProjectFile(invalid_path.string(), &loaded, &error) && equal(loaded, original),
          "truncated MIDI section changed caller score");

  const std::vector<std::function<void(Score&)>> invalid_scores = {
      [](Score& s) { s.parts[0].measures[0].notes[0].midi_channel = -2; },
      [](Score& s) { s.parts[0].measures[0].notes[0].midi_channel = 16; },
      [](Score& s) { s.parts[0].measures[0].notes[0].midi_release_velocity = 128; },
      [](Score& s) { s.parts[0].midi_events[0].tick = -1; },
      [](Score& s) { s.parts[0].midi_events[0].type = static_cast<MidiChannelEventType>(0x90); },
      [](Score& s) { s.parts[0].midi_events[0].channel = 16; },
      [](Score& s) { s.parts[0].midi_events[0].data1 = 128; },
      [](Score& s) { s.parts[0].midi_events[0].data2 = 128; },
      [](Score& s) { s.parts[1].midi_events[0].data2 = 1; },
      [](Score& s) { s.parts[1].midi_events[1].data2 = 1; },
  };
  for (const auto& mutate : invalid_scores) {
    Score invalid = original;
    mutate(invalid);
    const Score before = invalid;
    require(!writeProjectFile(invalid, path.string(), &error), "accepted invalid in-memory metadata");
    require(equal(invalid, before) && readText(path) == serialized &&
            !std::filesystem::exists(path.string() + ".tmp"), "failed save changed input or existing file");
  }

  // Both per-part and aggregate caps are checked before serialization. The
  // aggregate read fixture need only contain the first part: the next count
  // must fail before attempting to allocate/read its advertised events.
  {
    Score excessive;
    excessive.parts.resize(1);
    excessive.parts[0].midi_events.resize(1'000'001);
    require(!writeProjectFile(excessive, path.string(), &error) && readText(path) == serialized,
            "per-part MIDI event write cap");
    excessive.parts[0].midi_events.resize(500'001);
    excessive.parts.resize(2);
    excessive.parts[1].midi_events.resize(500'000);
    require(!writeProjectFile(excessive, path.string(), &error) && readText(path) == serialized,
            "aggregate MIDI event write cap");
  }
  {
    std::ofstream output(invalid_path, std::ios::binary | std::ios::trunc);
    output << "CLASSICAL_DAW_PROJECT 3\ndivisions 960\nbpm 120\nmeter 4 4\nparts 2\n"
              "part 0\nid 5031\nname -\nmeasures 0\nmidi_events 500001\n";
    for (std::size_t i = 0; i < 500'001; ++i) output << "midi_event 0 176 0 64 127 0\n";
    output << "end_part\npart 1\nid 5032\nname -\nmeasures 0\nmidi_events 500000\n";
    require(static_cast<bool>(output), "aggregate count fixture write");
  }
  loaded = original;
  require(!readProjectFile(invalid_path.string(), &loaded, &error) &&
          error == "project has too many MIDI events" && equal(loaded, original),
          "aggregate MIDI event read cap before allocation: " + error);

  // A performance edit must survive history navigation, normal save and both
  // recovery candidates with the same exact source ordinals.
  Score edited = original;
  edited.parts[0].measures[0].notes[0].midi_channel = 3;
  edited.parts[0].measures[0].notes[0].midi_on_order = 9007199254740995ULL;
  edited.parts[0].measures[0].notes[0].midi_release_velocity = 57;
  edited.parts[0].midi_events[0].data2 = 0;
  edited.parts[1].midi_events.push_back({2400, MidiChannelEventType::ControlChange, 3, 11, 80, 77});
  ScoreHistory history(original);
  require(history.commit(edited, &error) && history.undo(&loaded, &error) && equal(loaded, original),
          "MIDI metadata undo");
  require(writeProjectRecoveryFile(history, path.string(), &error), "write undo state recovery");
  require(readProjectFile(path.string() + ".recovery", &loaded, &error) && equal(loaded, original),
          "recovery snapshots current undo state");
  require(history.redo(&loaded, &error) && equal(loaded, edited), "MIDI metadata redo");
  require(writeProjectFile(loaded, path.string(), &error) &&
          readProjectFile(path.string(), &loaded, &error) && equal(loaded, edited), "save after redo");
  require(writeProjectRecoveryFile(history, path.string(), &error), "history recovery write");
  const std::string recovery_text = readText(path.string() + ".recovery");
  Score invalid_recovery = edited;
  invalid_recovery.parts[0].midi_events[0].channel = 16;
  require(!writeProjectRecoveryFile(invalid_recovery, path.string(), &error) &&
          readText(path.string() + ".recovery") == recovery_text, "failed recovery save preserves sidecar");
  writeText(path, "corrupt primary\n");
  ProjectLoadSource source = ProjectLoadSource::None;
  require(readProjectFileWithRecovery(path.string(), &loaded, &source, &error) &&
          source == ProjectLoadSource::Recovery && equal(loaded, edited), "metadata recovery fallback");
  require(readText(path) == "corrupt primary\n", "recovery load touched primary file");
  std::filesystem::remove(path.string() + ".recovery");
  require(writeProjectFile(edited, path.string() + ".tmp", &error), "temporary recovery fixture");
  require(readProjectFileWithRecovery(path.string(), &loaded, &source, &error) &&
          source == ProjectLoadSource::Temporary && equal(loaded, edited), "metadata temporary fallback");
  writeText(path.string() + ".tmp", "corrupt temporary\n");
  require(!readProjectFileWithRecovery(path.string(), &loaded, &source, &error) && equal(loaded, edited),
          "failed recovery changed MIDI metadata");
}

}  // namespace

int main() {
  try {
    run();
    std::cout << "classical-daw project MIDI metadata tests passed\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "FAIL: " << exception.what() << '\n';
    return 1;
  }
}
