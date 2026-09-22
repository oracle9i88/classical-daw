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
      ("classical_daw_project_tempo_" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  TestFiles() { require(std::filesystem::create_directory(directory), "create test directory"); }
  ~TestFiles() {
    std::error_code ignored;
    std::filesystem::remove(directory / "tempo.cdaw", ignored);
    std::filesystem::remove(directory / "tempo.cdaw.recovery", ignored);
    std::filesystem::remove(directory / "tempo.cdaw.tmp", ignored);
    std::filesystem::remove(directory / "invalid.cdaw", ignored);
    std::filesystem::remove(directory, ignored);
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

std::string replace(std::string text, const std::string& from, const std::string& to) {
  const auto position = text.find(from);
  require(position != std::string::npos, "missing mutation target: " + from);
  text.replace(position, from.size(), to);
  return text;
}

// These fixtures deliberately contain no parts, so every editable field can
// be checked without weakening the failure-preservation assertions.
bool equal(const Score& left, const Score& right) {
  if (left.divisions != right.divisions || left.bpm != right.bpm ||
      left.time_signature.numerator != right.time_signature.numerator ||
      left.time_signature.denominator != right.time_signature.denominator ||
      left.time_signature.clocks_per_click != right.time_signature.clocks_per_click ||
      left.time_signature.notated_32nds_per_quarter != right.time_signature.notated_32nds_per_quarter ||
      !left.parts.empty() || !right.parts.empty() ||
      left.tempo_changes.size() != right.tempo_changes.size() ||
      left.meter_changes.size() != right.meter_changes.size()) return false;
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
  return true;
}

void run() {
  TestFiles files;
  const auto path = files.directory / "tempo.cdaw";
  const auto invalid_path = files.directory / "invalid.cdaw";
  Score original;
  original.bpm = 120.0;
  original.tempo_changes = {{240, 90.25}, {960, 60.125}, {7680, 89.12345678901234}};
  std::string error;
  require(writeProjectFile(original, path.string(), &error), "write v5: " + error);
  const std::string serialized = readText(path);
  require(serialized.rfind("CLASSICAL_DAW_PROJECT 5\ndivisions 960\nbpm 120\ntempo_changes 3\n", 0) == 0,
          "v5 tempo section follows initial BPM");
  require(serialized.find("tempo 960 60.125\n") < serialized.find("meter 4 4 24 8\n"),
          "v5 tempo section precedes meter");
  Score loaded;
  require(readProjectFile(path.string(), &loaded, &error) && equal(loaded, original),
          "tempo ticks and full-precision BPM round trip: " + error);
  require(!std::filesystem::exists(path.string() + ".tmp"), "successful save left temporary file");

  // Version 4 preserves the tempo section. Earlier layouts must clear the
  // destination's changes rather than retaining data from a previous load.
  for (const int version : {1, 2, 3, 4}) {
    std::istringstream input(serialized);
    std::ostringstream legacy;
    std::string line;
    while (std::getline(input, line)) {
      if (line.rfind("meter_changes ", 0) == 0 || line.rfind("meter_change ", 0) == 0 ||
          (version < 4 && (line.rfind("tempo_changes ", 0) == 0 || line.rfind("tempo ", 0) == 0))) continue;
      if (line.rfind("meter ", 0) == 0) line = "meter 4 4";
      if (line.rfind("CLASSICAL_DAW_PROJECT ", 0) == 0) line = "CLASSICAL_DAW_PROJECT " + std::to_string(version);
      legacy << line << '\n';
    }
    writeText(invalid_path, legacy.str());
    loaded = original;
    Score expected = original;
    if (version < 4) expected.tempo_changes.clear();
    require(readProjectFile(invalid_path.string(), &loaded, &error) && equal(loaded, expected),
            "legacy tempo preservation, version " + std::to_string(version));
  }

  // Count errors must be diagnosed immediately, before allocating the
  // advertised vector or attempting to read any missing tempo rows.
  for (const std::string count : {"1000001", "18446744073709551616", "-1", "2.5"}) {
    writeText(invalid_path, "CLASSICAL_DAW_PROJECT 4\ndivisions 960\nbpm 120\ntempo_changes " + count + "\n");
    loaded = original;
    require(!readProjectFile(invalid_path.string(), &loaded, &error) &&
            error == "project tempo change count out of range" && equal(loaded, original),
            "tempo count rejected before rows: " + count + ": " + error);
  }
  const std::vector<std::pair<std::string, std::string>> invalid_fields = {
      {"tempo 960 60.125\n", "tempo 200 60.125\n"},
      {"tempo 960 60.125\n", "tempo 240 60.125\n"},
      {"tempo 240 90.25\n", "tempo 0 90.25\n"},
      {"tempo 240 90.25\n", "tempo -1 90.25\n"},
      {"tempo 240 90.25\n", "tempo 9223372036854775808 90.25\n"},
      {"tempo 960 60.125\n", "tempo 960 0\n"},
      {"tempo 960 60.125\n", "tempo 960 -1\n"},
      {"tempo 960 60.125\n", "tempo 960 nan\n"},
      {"tempo 960 60.125\n", "tempo 960 inf\n"},
      {"tempo 960 60.125\n", "tempo 960 1000001\n"},
      {"bpm 120\n", "bpm 0\n"},
      {"bpm 120\n", "bpm nan\n"},
      {"bpm 120\n", "bpm 1000001\n"},
      {"tempo_changes 3\n", "tempo_changes 4\n"},
      {"tempo_changes 3\n", "tempo_changes 2\n"},
  };
  for (const auto& mutation : invalid_fields) {
    writeText(invalid_path, replace(serialized, mutation.first, mutation.second));
    loaded = original;
    require(!readProjectFile(invalid_path.string(), &loaded, &error) && !error.empty() && equal(loaded, original),
            "invalid tempo read preserved destination: " + mutation.second);
  }
  writeText(invalid_path, serialized.substr(0, serialized.find("tempo 960")));
  loaded = original;
  require(!readProjectFile(invalid_path.string(), &loaded, &error) && equal(loaded, original),
          "truncated tempo section preserved destination");

  const std::vector<std::function<void(Score&)>> invalid_scores = {
      [](Score& s) { s.tempo_changes[1].tick = 200; },
      [](Score& s) { s.tempo_changes[1].tick = s.tempo_changes[0].tick; },
      [](Score& s) { s.tempo_changes[0].tick = 0; },
      [](Score& s) { s.tempo_changes[0].tick = -1; },
      [](Score& s) { s.tempo_changes[0].bpm = 0.0; },
      [](Score& s) { s.tempo_changes[0].bpm = -1.0; },
      [](Score& s) { s.tempo_changes[0].bpm = std::numeric_limits<double>::quiet_NaN(); },
      [](Score& s) { s.tempo_changes[0].bpm = std::numeric_limits<double>::infinity(); },
      [](Score& s) { s.tempo_changes[0].bpm = 1'000'001.0; },
      [](Score& s) { s.bpm = 0.0; },
      [](Score& s) { s.bpm = std::numeric_limits<double>::quiet_NaN(); },
      [](Score& s) { s.bpm = 1'000'001.0; },
      [](Score& s) { s.tempo_changes.resize(kMaxScoreTempoChanges + 1U); },
  };
  for (const auto& mutate : invalid_scores) {
    Score invalid = original;
    mutate(invalid);
    require(!writeProjectFile(invalid, path.string(), &error) && !error.empty() &&
            readText(path) == serialized && !std::filesystem::exists(path.string() + ".tmp"),
            "invalid tempo save preserved existing project");
  }

  Score edited = original;
  edited.bpm = 88.0;
  edited.tempo_changes = {{480, 66.0}, {1440, 132.0}};
  const Score committed = edited;
  ScoreHistory history(original);
  require(history.commit(edited, &error), "commit tempo edit");
  edited.tempo_changes.clear();
  require(history.snapshot(&loaded, &error) && equal(loaded, committed), "tempo history owns independent copy");
  require(history.undo(&loaded, &error) && equal(loaded, original), "tempo undo");
  require(writeProjectRecoveryFile(history, path.string(), &error) &&
          readProjectFile(path.string() + ".recovery", &loaded, &error) && equal(loaded, original),
          "recovery persists current undo tempo state");
  require(history.redo(&loaded, &error) && equal(loaded, committed), "tempo redo");
  require(history.clear(&error) && history.snapshot(&loaded, &error) && equal(loaded, committed),
          "tempo history clear retains current state");
  require(writeProjectRecoveryFile(history, path.string(), &error) && readText(path) == serialized,
          "tempo recovery write leaves primary alone");
  const std::string recovery = readText(path.string() + ".recovery");
  Score invalid_recovery = committed;
  invalid_recovery.tempo_changes[0].tick = 0;
  require(!writeProjectRecoveryFile(invalid_recovery, path.string(), &error) &&
          readText(path.string() + ".recovery") == recovery, "invalid recovery preserves prior tempo state");
  writeText(path, "invalid primary\n");
  ProjectLoadSource source = ProjectLoadSource::None;
  require(readProjectFileWithRecovery(path.string(), &loaded, &source, &error) &&
          source == ProjectLoadSource::Recovery && equal(loaded, committed), "tempo recovery fallback");
  require(history.reset(loaded, &error) && !history.canUndo() && !history.canRedo() &&
          history.snapshot(&loaded, &error) && equal(loaded, committed), "tempo history reset after recovery");
  std::filesystem::remove(path.string() + ".recovery");
  require(writeProjectFile(committed, path.string() + ".tmp", &error), "temporary tempo recovery fixture");
  require(readProjectFileWithRecovery(path.string(), &loaded, &source, &error) &&
          source == ProjectLoadSource::Temporary && equal(loaded, committed), "temporary tempo recovery fallback");
  require(readText(path) == "invalid primary\n", "recovery load modified primary");
}

}  // namespace

int main() {
  try {
    run();
    std::cout << "classical-daw project tempo tests passed\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "FAIL: " << exception.what() << '\n';
    return 1;
  }
}
