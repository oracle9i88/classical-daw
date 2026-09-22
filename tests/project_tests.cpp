#include "daw/project.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

int fail(const std::string& message) {
  std::cerr << "FAIL: " << message << '\n';
  return 1;
}

bool equalScore(const daw::Score& left, const daw::Score& right) {
  if (left.divisions != right.divisions || left.time_signature.numerator != right.time_signature.numerator ||
      left.time_signature.denominator != right.time_signature.denominator || left.bpm != right.bpm ||
      left.parts.size() != right.parts.size()) {
    return false;
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
      if (am.number != bm.number || am.start != bm.start || am.notes.size() != bm.notes.size()) return false;
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

bool readText(const std::filesystem::path& path, std::string* text) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return false;
  std::ostringstream output;
  output << input.rdbuf();
  *text = output.str();
  return static_cast<bool>(input) || input.eof();
}

bool writeText(const std::filesystem::path& path, const std::string& text) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) return false;
  output << text;
  return static_cast<bool>(output);
}

}  // namespace

int main() {
  using namespace daw;
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                     ("classical_daw_project_" + std::to_string(stamp) + ".cdaw");

  Score source;
  source.divisions = kTicksPerQuarter;
  source.time_signature = {6, 8};
  source.bpm = 72.5;
  source.parts = {
      ScorePart{"Piano & Strings", "钢琴 <主奏>",
                {ScoreMeasure{1, 0,
                              {ScoreNote{0, 1440, ScorePitch{'C', -1, 4}, false, false, true, false, 119, 2, 1, 3, 2},
                               ScoreNote{1440, 480, ScorePitch{'E', 0, 5}, false, true, false, true, 80, 3, 2, 0, 0},
                               ScoreNote{1920, 480, ScorePitch{'C', 0, 4}, true, false, false, false, 0, 1, 1, 0, 0}}}}},
      ScorePart{"P2", "Cello", {ScoreMeasure{2, 2880, {}}}},
  };
  source.parts[0].measures[0].notes[0].lyric = "你好 & verse";
  std::string error;
  if (!writeProjectFile(source, path.string(), &error)) return fail("project write: " + error);
  if (std::filesystem::exists(path.string() + ".tmp")) return fail("temporary project file was left behind");

  Score loaded;
  if (!readProjectFile(path.string(), &loaded, &error)) return fail("project read: " + error);
  if (!equalScore(source, loaded)) return fail("project score round-trip");

  std::string serialized;
  if (!readText(path, &serialized)) return fail("project fixture read");
  if (serialized.rfind("CLASSICAL_DAW_PROJECT 3\n", 0) != 0 || serialized.find("end_project\n") == std::string::npos ||
      serialized.find("lyric ") == std::string::npos) {
    return fail("project format header/footer");
  }

  // Legacy files lack MIDI fields, and version 1 also predates lyrics.
  for (const int version : {1, 2}) {
    Score legacy_expected = source;
    if (version == 1) legacy_expected.parts[0].measures[0].notes[0].lyric.clear();
    std::istringstream current(serialized);
    std::ostringstream legacy;
    std::string line;
    while (std::getline(current, line)) {
      if (line.rfind("midi_", 0) == 0 || (version == 1 && line.rfind("lyric ", 0) == 0)) continue;
      if (line.rfind("CLASSICAL_DAW_PROJECT ", 0) == 0) line = "CLASSICAL_DAW_PROJECT " + std::to_string(version);
      legacy << line << '\n';
    }
    if (!writeText(path, legacy.str())) return fail("legacy project fixture write");
    if (!readProjectFile(path.string(), &loaded, &error) || !equalScore(legacy_expected, loaded)) {
      return fail("version " + std::to_string(version) + " project compatibility");
    }
  }

  // Unknown versions are rejected before mutating the caller's score.
  loaded = source;
  const Score sentinel = loaded;
  std::string unknown_version = serialized;
  unknown_version.replace(0, std::string("CLASSICAL_DAW_PROJECT 3").size(), "CLASSICAL_DAW_PROJECT 99");
  if (!writeText(path, unknown_version)) return fail("unknown-version fixture write");
  if (readProjectFile(path.string(), &loaded, &error)) return fail("unknown project version accepted");
  if (!equalScore(loaded, sentinel)) return fail("unknown-version read mutated score");

  // Missing end_project is treated as truncation, rather than accepting a
  // parse that happened to end after a complete note.
  if (!writeText(path, serialized.substr(0, serialized.size() - std::string("end_project\n").size()))) {
    return fail("truncated fixture write");
  }
  if (readProjectFile(path.string(), &loaded, &error)) return fail("truncated project accepted");
  if (!equalScore(loaded, sentinel)) return fail("truncated read mutated score");

  // Numeric overflow and non-finite values are rejected.
  std::string overflow = serialized;
  const std::string old_parts = "parts 2";
  const std::size_t parts_position = overflow.find(old_parts);
  if (parts_position == std::string::npos) return fail("parts fixture missing");
  overflow.replace(parts_position, old_parts.size(), "parts 18446744073709551615");
  if (!writeText(path, overflow)) return fail("overflow fixture write");
  if (readProjectFile(path.string(), &loaded, &error)) return fail("project count overflow accepted");
  if (!equalScore(loaded, sentinel)) return fail("overflow read mutated score");

  std::string non_finite = serialized;
  const std::string old_bpm = "bpm 72.5";
  const std::size_t bpm_position = non_finite.find(old_bpm);
  if (bpm_position == std::string::npos) return fail("bpm fixture missing");
  non_finite.replace(bpm_position, old_bpm.size(), "bpm nan");
  if (!writeText(path, non_finite)) return fail("non-finite fixture write");
  if (readProjectFile(path.string(), &loaded, &error)) return fail("non-finite project bpm accepted");
  if (!equalScore(loaded, sentinel)) return fail("non-finite read mutated score");

  // A later successful save atomically replaces an existing project.
  if (!writeProjectFile(source, path.string(), &error)) return fail("project replacement write: " + error);
  Score replaced;
  if (!readProjectFile(path.string(), &replaced, &error) || !equalScore(source, replaced)) {
    return fail("atomic project replacement");
  }

  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + ".tmp");
  std::cout << "classical-daw project persistence tests passed\n";
  return 0;
}
