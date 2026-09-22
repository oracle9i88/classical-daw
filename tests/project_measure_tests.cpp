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
      ("classical_daw_project_measure_" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  TestFiles() { require(std::filesystem::create_directory(directory), "create test directory"); }
  ~TestFiles() {
    std::error_code ignored;
    for (const char* name : {"measure.cdaw", "measure.cdaw.recovery", "measure.cdaw.tmp", "invalid.cdaw"}) {
      std::filesystem::remove(directory / name, ignored);
    }
    std::filesystem::remove(directory, ignored);
  }
};

bool equalScore(const daw::Score& left, const daw::Score& right) {
  if (left.divisions != right.divisions || left.time_signature.numerator != right.time_signature.numerator ||
      left.time_signature.denominator != right.time_signature.denominator || left.bpm != right.bpm ||
      left.time_signature.clocks_per_click != right.time_signature.clocks_per_click ||
      left.time_signature.notated_32nds_per_quarter != right.time_signature.notated_32nds_per_quarter ||
      left.parts.size() != right.parts.size() || left.tempo_changes.size() != right.tempo_changes.size() ||
      left.meter_changes.size() != right.meter_changes.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.tempo_changes.size(); ++index) {
    if (left.tempo_changes[index].tick != right.tempo_changes[index].tick ||
        left.tempo_changes[index].bpm != right.tempo_changes[index].bpm) return false;
  }
  for (std::size_t index = 0; index < left.meter_changes.size(); ++index) {
    const auto& a = left.meter_changes[index];
    const auto& b = right.meter_changes[index];
    if (a.tick != b.tick || a.signature.numerator != b.signature.numerator ||
        a.signature.denominator != b.signature.denominator ||
        a.signature.clocks_per_click != b.signature.clocks_per_click ||
        a.signature.notated_32nds_per_quarter != b.signature.notated_32nds_per_quarter) return false;
  }
  for (std::size_t part_index = 0; part_index < left.parts.size(); ++part_index) {
    const daw::ScorePart& a = left.parts[part_index];
    const daw::ScorePart& b = right.parts[part_index];
    if (a.id != b.id || a.name != b.name || a.measures.size() != b.measures.size() ||
        a.midi_events.size() != b.midi_events.size()) return false;
    for (std::size_t event_index = 0; event_index < a.midi_events.size(); ++event_index) {
      const auto& ae = a.midi_events[event_index];
      const auto& be = b.midi_events[event_index];
      if (ae.tick != be.tick || ae.type != be.type || ae.channel != be.channel || ae.data1 != be.data1 ||
          ae.data2 != be.data2 || ae.order != be.order) return false;
    }
    for (std::size_t measure_index = 0; measure_index < a.measures.size(); ++measure_index) {
      const daw::ScoreMeasure& am = a.measures[measure_index];
      const daw::ScoreMeasure& bm = b.measures[measure_index];
      if (am.number != bm.number || am.start != bm.start || am.duration != bm.duration ||
          am.notes.size() != bm.notes.size()) return false;
      for (std::size_t note_index = 0; note_index < am.notes.size(); ++note_index) {
        const daw::ScoreNote& an = am.notes[note_index];
        const daw::ScoreNote& bn = bm.notes[note_index];
        if (an.start != bn.start || an.duration != bn.duration || an.pitch.step != bn.pitch.step ||
            an.pitch.alter != bn.pitch.alter || an.pitch.octave != bn.pitch.octave || an.rest != bn.rest ||
            an.chord != bn.chord || an.tie_start != bn.tie_start || an.tie_stop != bn.tie_stop ||
            an.velocity != bn.velocity || an.voice != bn.voice || an.staff != bn.staff ||
            an.tuplet_actual != bn.tuplet_actual || an.tuplet_normal != bn.tuplet_normal || an.lyric != bn.lyric ||
            an.midi_channel != bn.midi_channel || an.midi_on_order != bn.midi_on_order ||
            an.midi_off_order != bn.midi_off_order || an.midi_release_velocity != bn.midi_release_velocity) {
          return false;
        }
      }
    }
  }
  return true;
}

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

std::string replace(std::string text, const std::string& from, const std::string& to) {
  const auto position = text.find(from);
  require(position != std::string::npos, "missing mutation target: " + from);
  text.replace(position, from.size(), to);
  return text;
}

void run() {
  TestFiles files;
  const auto path = files.directory / "measure.cdaw";
  const auto invalid_path = files.directory / "invalid.cdaw";
  Score original;
  original.parts.resize(1);
  original.parts[0].measures = {ScoreMeasure{1, 0, {}, 3840}, ScoreMeasure{2, 3840, {}, 1440}};
  ScoreNote note;
  note.duration = 480;
  original.parts[0].measures[0].notes.push_back(note);
  note.start = 3840;
  note.lyric = "tail";
  original.parts[0].measures[1].notes.push_back(note);
  // The last 1440-tick measure is short in 4/4. Its 960 ticks of terminal
  // silence cannot be inferred from the last note or a next measure start.
  std::string error;
  require(writeProjectFile(original, path.string(), &error), "write v6: " + error);
  const std::string serialized = readText(path);
  require(serialized.rfind("CLASSICAL_DAW_PROJECT 6\n", 0) == 0, "v6 header");
  require(serialized.find("measure 1\nnumber 2\nstart 3840\nduration 1440\nnotes 1\n") != std::string::npos,
          "explicit measure duration follows measure start");
  Score loaded;
  require(readProjectFile(path.string(), &loaded, &error) && equalScore(loaded, original),
          "short terminal measure and trailing silence round trip: " + error);
  require(!std::filesystem::exists(path.string() + ".tmp"), "successful save left temporary file");

  // Only measure durations disappear in old layouts. Note durations remain
  // present and unchanged, including the terminal 480-tick sounding note.
  for (const int version : {1, 2, 3, 4, 5}) {
    std::istringstream input(serialized);
    std::ostringstream legacy;
    std::string line;
    bool in_note = false;
    while (std::getline(input, line)) {
      if (line.rfind("note ", 0) == 0) in_note = true;
      if (line == "end_note") in_note = false;
      if ((!in_note && line.rfind("duration ", 0) == 0) ||
          (version < 5 && (line.rfind("meter_changes ", 0) == 0 || line.rfind("meter_change ", 0) == 0)) ||
          (version < 4 && (line.rfind("tempo_changes ", 0) == 0 || line.rfind("tempo ", 0) == 0)) ||
          (version < 3 && line.rfind("midi_", 0) == 0) ||
          (version == 1 && line.rfind("lyric ", 0) == 0)) continue;
      if (version < 5 && line.rfind("meter ", 0) == 0) line = "meter 4 4";
      if (line.rfind("CLASSICAL_DAW_PROJECT ", 0) == 0) line = "CLASSICAL_DAW_PROJECT " + std::to_string(version);
      legacy << line << '\n';
    }
    Score expected = original;
    for (auto& measure : expected.parts[0].measures) {
      measure.duration = 0;
      if (version == 1) for (auto& entry : measure.notes) entry.lyric.clear();
    }
    writeText(invalid_path, legacy.str());
    loaded = original;
    require(readProjectFile(invalid_path.string(), &loaded, &error) && equalScore(loaded, expected),
            "legacy unspecified measure duration, version " + std::to_string(version));
  }

  const std::vector<std::pair<std::string, std::string>> invalid_fields = {
      {"start 3840\nduration 1440\n", "start 3840\nduration -1\n"},
      {"start 3840\nduration 1440\n", "start 3840\nduration 9223372036854775808\n"},
      {"start 3840\nduration 1440\n", "start 3840\nduration 1.5\n"},
      {"start 0\nduration 3840\n", "start 0\nduration 3600\n"},
      {"start 0\nduration 3840\n", "start 0\nduration 4000\n"},
      {"note 0\nstart 3840\nduration 480\n", "note 0\nstart 3840\nduration 1920\n"},
      {"note 0\nstart 3840\nduration 480\n", "note 0\nstart 3839\nduration 480\n"},
      {"start 3840\nduration 1440\n", "start 3840\n"},
  };
  for (const auto& mutation : invalid_fields) {
    writeText(invalid_path, replace(serialized, mutation.first, mutation.second));
    loaded = original;
    require(!readProjectFile(invalid_path.string(), &loaded, &error) && !error.empty() && equalScore(loaded, original),
            "invalid measure read preserved destination: " + mutation.second);
  }
  // Leave the preceding extent unspecified so its adjacency check cannot
  // mask the overflowing terminal start + duration being tested here.
  std::string overflow = replace(serialized, "start 0\nduration 3840\n", "start 0\nduration 0\n");
  overflow = replace(overflow, "start 3840\nduration 1440\n", "start 9223372036854775800\nduration 1440\n");
  writeText(invalid_path, overflow);
  loaded = original;
  require(!readProjectFile(invalid_path.string(), &loaded, &error) &&
          error == "project measure duration out of range" && equalScore(loaded, original),
          "overflowing explicit measure endpoint rejected before addition");
  const std::vector<std::function<void(Score&)>> invalid_scores = {
      [](Score& s) { s.parts[0].measures[1].duration = -1; },
      [](Score& s) {
        s.parts[0].measures[0].duration = 0;
        s.parts[0].measures[1].start = std::numeric_limits<Tick>::max() - 7;
      },
      [](Score& s) { s.parts[0].measures[0].duration = 3600; },
      [](Score& s) { s.parts[0].measures[0].duration = 4000; },
      [](Score& s) { s.parts[0].measures[1].start = 0; },
      [](Score& s) { s.parts[0].measures[1].notes[0].duration = 1920; },
      [](Score& s) { s.parts[0].measures[1].notes[0].start = 3839; },
  };
  for (const auto& mutate : invalid_scores) {
    Score invalid = original;
    mutate(invalid);
    const Score before = invalid;
    require(!writeProjectFile(invalid, path.string(), &error) && !error.empty() && equalScore(invalid, before) &&
            readText(path) == serialized && !std::filesystem::exists(path.string() + ".tmp"),
            "invalid measure save preserved input and existing project");
  }

  // Zero retains prior project semantics: saving must not infer an extent or
  // newly reject historical start ordering/note placement for such measures.
  Score unspecified = original;
  for (auto& measure : unspecified.parts[0].measures) measure.duration = 0;
  unspecified.parts[0].measures[0].start = 5000;
  require(writeProjectFile(unspecified, invalid_path.string(), &error) &&
          readProjectFile(invalid_path.string(), &loaded, &error) && equalScore(loaded, unspecified),
          "unspecified durations keep legacy validation semantics");

  Score edited = original;
  edited.parts[0].measures[1].duration = 1920;
  const Score committed = edited;
  ScoreHistory history(original);
  require(history.commit(edited, &error), "commit terminal silence edit");
  edited.parts[0].measures[1].duration = 480;
  require(history.snapshot(&loaded, &error) && equalScore(loaded, committed), "history owns measure duration copy");
  require(history.undo(&loaded, &error) && equalScore(loaded, original), "measure duration undo");
  require(writeProjectRecoveryFile(history, path.string(), &error) &&
          readProjectFile(path.string() + ".recovery", &loaded, &error) && equalScore(loaded, original),
          "recovery preserves undo state's terminal silence");
  require(history.redo(&loaded, &error) && equalScore(loaded, committed), "measure duration redo");
  require(writeProjectRecoveryFile(history, path.string(), &error) && readText(path) == serialized,
          "measure recovery write leaves primary alone");
  const std::string recovery = readText(path.string() + ".recovery");
  Score invalid_recovery = committed;
  invalid_recovery.parts[0].measures[1].duration = 240;
  require(!writeProjectRecoveryFile(invalid_recovery, path.string(), &error) &&
          readText(path.string() + ".recovery") == recovery, "invalid recovery preserves measure state");
  writeText(path, "invalid primary\n");
  ProjectLoadSource source = ProjectLoadSource::None;
  require(readProjectFileWithRecovery(path.string(), &loaded, &source, &error) &&
          source == ProjectLoadSource::Recovery && equalScore(loaded, committed), "measure recovery fallback");
  require(history.reset(loaded, &error) && !history.canUndo() && !history.canRedo() &&
          history.snapshot(&loaded, &error) && equalScore(loaded, committed), "measure history reset after recovery");
  std::filesystem::remove(path.string() + ".recovery");
  require(writeProjectFile(committed, path.string() + ".tmp", &error), "temporary measure recovery fixture");
  require(readProjectFileWithRecovery(path.string(), &loaded, &source, &error) &&
          source == ProjectLoadSource::Temporary && equalScore(loaded, committed), "temporary measure recovery fallback");
  require(readText(path) == "invalid primary\n", "recovery load modified primary");
}

}  // namespace

int main() {
  try {
    run();
    std::cout << "classical-daw project measure tests passed\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "FAIL: " << exception.what() << '\n';
    return 1;
  }
}
