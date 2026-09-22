#include "daw/history.hpp"
#include "daw/meter_map.hpp"
#include "daw/project.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
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
      ("classical_daw_project_meter_" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  TestFiles() { require(std::filesystem::create_directory(directory), "create test directory"); }
  ~TestFiles() {
    std::error_code ignored;
    for (const char* name : {"meter.cdaw", "meter.cdaw.recovery", "meter.cdaw.tmp", "invalid.cdaw"}) {
      std::filesystem::remove(directory / name, ignored);
    }
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

bool equalMeter(const TimeSignature& a, const TimeSignature& b) {
  return a.numerator == b.numerator && a.denominator == b.denominator &&
         a.clocks_per_click == b.clocks_per_click &&
         a.notated_32nds_per_quarter == b.notated_32nds_per_quarter;
}

// No-part fixtures let these assertions compare every editable field, including
// the tempo map that v4 already retained before the meter format was added.
bool equal(const Score& a, const Score& b) {
  if (a.divisions != b.divisions || a.bpm != b.bpm || !equalMeter(a.time_signature, b.time_signature) ||
      !a.parts.empty() || !b.parts.empty() || a.tempo_changes.size() != b.tempo_changes.size() ||
      a.meter_changes.size() != b.meter_changes.size()) return false;
  for (std::size_t index = 0; index < a.tempo_changes.size(); ++index) {
    if (a.tempo_changes[index].tick != b.tempo_changes[index].tick ||
        a.tempo_changes[index].bpm != b.tempo_changes[index].bpm) return false;
  }
  for (std::size_t index = 0; index < a.meter_changes.size(); ++index) {
    if (a.meter_changes[index].tick != b.meter_changes[index].tick ||
        !equalMeter(a.meter_changes[index].signature, b.meter_changes[index].signature)) return false;
  }
  return true;
}

void run() {
  TestFiles files;
  const auto path = files.directory / "meter.cdaw";
  const auto invalid_path = files.directory / "invalid.cdaw";
  Score original;
  original.bpm = 96.5;
  original.time_signature = {7, 8, 36, 12};
  original.tempo_changes = {{960, 88.125}};
  original.meter_changes = {
      {1440, {3, 4, 0, 8}}, {2880, {5, 16, 255, 255}}, {3000, {255, 128, 1, 1}},
  };
  std::string error;
  require(writeProjectFile(original, path.string(), &error), "write v6: " + error);
  const std::string serialized = readText(path);
  require(serialized.rfind("CLASSICAL_DAW_PROJECT 6\n", 0) == 0, "v6 header");
  require(serialized.find("meter 7 8 36 12\nmeter_changes 3\nmeter_change 1440 3 4 0 8\n") != std::string::npos,
          "four-field initial meter followed by later meter section");
  require(serialized.find("meter_change 3000 255 128 1 1\nparts 0\n") != std::string::npos,
          "meter section precedes parts");
  Score loaded;
  require(readProjectFile(path.string(), &loaded, &error) && equal(loaded, original),
          "raw meter bytes, off-barline positions and tempos round trip: " + error);
  require(!std::filesystem::exists(path.string() + ".tmp"), "successful save left temporary file");

  // Pre-v5 files use two-field meter rows and 24/8 defaults. Version 5 must
  // retain the meter map, while versions 4/5 retain later tempos.
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
          (version < 4 && (line.rfind("tempo_changes ", 0) == 0 || line.rfind("tempo ", 0) == 0))) continue;
      if (version < 5 && line.rfind("meter ", 0) == 0) line = "meter 7 8";
      if (line.rfind("CLASSICAL_DAW_PROJECT ", 0) == 0) line = "CLASSICAL_DAW_PROJECT " + std::to_string(version);
      legacy << line << '\n';
    }
    writeText(invalid_path, legacy.str());
    Score expected = original;
    if (version < 5) {
      expected.time_signature = {7, 8};
      expected.meter_changes.clear();
    }
    if (version < 4) expected.tempo_changes.clear();
    loaded = original;
    require(readProjectFile(invalid_path.string(), &loaded, &error) && equal(loaded, expected),
            "legacy meter defaults and tempo preservation, version " + std::to_string(version));
  }

  for (const std::string count : {"1000001", "18446744073709551616", "-1", "2.5"}) {
    writeText(invalid_path, "CLASSICAL_DAW_PROJECT 5\ndivisions 960\nbpm 120\ntempo_changes 0\n"
                            "meter 4 4 24 8\nmeter_changes " + count + "\n");
    loaded = original;
    require(!readProjectFile(invalid_path.string(), &loaded, &error) &&
            error == "project meter change count out of range" && equal(loaded, original),
            "meter count rejected before allocation/rows: " + count + ": " + error);
  }
  const std::vector<std::pair<std::string, std::string>> invalid_fields = {
      {"meter 7 8 36 12\n", "meter 0 8 36 12\n"},
      {"meter 7 8 36 12\n", "meter 256 8 36 12\n"},
      {"meter 7 8 36 12\n", "meter 7 3 36 12\n"},
      {"meter 7 8 36 12\n", "meter 7 8 256 12\n"},
      {"meter 7 8 36 12\n", "meter 7 8 -1 12\n"},
      {"meter 7 8 36 12\n", "meter 7 8 36 0\n"},
      {"meter 7 8 36 12\n", "meter 7 8 36 256\n"},
      {"meter 7 8 36 12\n", "meter 7 8\n"},
      {"meter_change 1440 3 4 0 8\n", "meter_change 0 3 4 0 8\n"},
      {"meter_change 1440 3 4 0 8\n", "meter_change -1 3 4 0 8\n"},
      {"meter_change 1440 3 4 0 8\n", "meter_change 2999 3 4 0 8\n"},
      {"meter_change 1440 3 4 0 8\n", "meter_change 2880 3 4 0 8\n"},
      {"meter_change 1440 3 4 0 8\n", "meter_change 9223372036854775808 3 4 0 8\n"},
      {"meter_change 1440 3 4 0 8\n", "meter_change 1440 0 4 0 8\n"},
      {"meter_change 1440 3 4 0 8\n", "meter_change 1440 3 3 0 8\n"},
      {"meter_change 1440 3 4 0 8\n", "meter_change 1440 3 4 256 8\n"},
      {"meter_change 1440 3 4 0 8\n", "meter_change 1440 3 4 0 0\n"},
      {"meter_change 1440 3 4 0 8\n", "meter_change 1440 3 4 0 256\n"},
      {"meter_changes 3\n", "meter_changes 2\n"},
      {"meter_changes 3\n", "meter_changes 4\n"},
  };
  for (const auto& mutation : invalid_fields) {
    writeText(invalid_path, replace(serialized, mutation.first, mutation.second));
    loaded = original;
    require(!readProjectFile(invalid_path.string(), &loaded, &error) && !error.empty() && equal(loaded, original),
            "invalid meter read preserved destination: " + mutation.second);
  }
  writeText(invalid_path, serialized.substr(0, serialized.find("meter_change 2880")));
  loaded = original;
  require(!readProjectFile(invalid_path.string(), &loaded, &error) && equal(loaded, original),
          "truncated meter section preserved destination");

  const std::vector<std::function<void(Score&)>> invalid_scores = {
      [](Score& s) { s.time_signature.numerator = 0; },
      [](Score& s) { s.time_signature.denominator = 3; },
      [](Score& s) { s.time_signature.notated_32nds_per_quarter = 0; },
      [](Score& s) { s.meter_changes[0].tick = 0; },
      [](Score& s) { s.meter_changes[0].tick = -1; },
      [](Score& s) { s.meter_changes[0].tick = 2999; },
      [](Score& s) { s.meter_changes[0].tick = s.meter_changes[1].tick; },
      [](Score& s) { s.meter_changes[0].signature.numerator = 0; },
      [](Score& s) { s.meter_changes[0].signature.denominator = 3; },
      [](Score& s) { s.meter_changes[0].signature.notated_32nds_per_quarter = 0; },
      [](Score& s) { s.meter_changes.resize(kMaxMeterChanges + 1U); },
  };
  for (const auto& mutate : invalid_scores) {
    Score invalid = original;
    mutate(invalid);
    require(!writeProjectFile(invalid, path.string(), &error) && !error.empty() &&
            readText(path) == serialized && !std::filesystem::exists(path.string() + ".tmp"),
            "invalid meter save preserved existing project");
  }

  Score edited = original;
  edited.time_signature = {6, 8, 18, 16};
  edited.meter_changes = {{480, {2, 4, 12, 8}}, {1920, {9, 8, 72, 16}}};
  const Score committed = edited;
  ScoreHistory history(original);
  require(history.commit(edited, &error), "commit meter edit");
  edited.meter_changes.clear();
  require(history.snapshot(&loaded, &error) && equal(loaded, committed), "history owns independent meter vector");
  require(history.undo(&loaded, &error) && equal(loaded, original), "meter undo");
  require(writeProjectRecoveryFile(history, path.string(), &error) &&
          readProjectFile(path.string() + ".recovery", &loaded, &error) && equal(loaded, original),
          "recovery persists current undo meter state");
  require(history.redo(&loaded, &error) && equal(loaded, committed), "meter redo");
  require(history.clear(&error) && history.snapshot(&loaded, &error) && equal(loaded, committed),
          "history clear retains current meter state");
  require(writeProjectRecoveryFile(history, path.string(), &error) && readText(path) == serialized,
          "meter recovery write leaves primary alone");
  const std::string recovery = readText(path.string() + ".recovery");
  Score invalid_recovery = committed;
  invalid_recovery.meter_changes[0].signature.notated_32nds_per_quarter = 0;
  require(!writeProjectRecoveryFile(invalid_recovery, path.string(), &error) &&
          readText(path.string() + ".recovery") == recovery, "invalid recovery preserves existing meter state");
  writeText(path, "invalid primary\n");
  ProjectLoadSource source = ProjectLoadSource::None;
  require(readProjectFileWithRecovery(path.string(), &loaded, &source, &error) &&
          source == ProjectLoadSource::Recovery && equal(loaded, committed), "meter recovery fallback");
  require(history.reset(loaded, &error) && !history.canUndo() && !history.canRedo() &&
          history.snapshot(&loaded, &error) && equal(loaded, committed), "meter history reset after recovery");
  std::filesystem::remove(path.string() + ".recovery");
  require(writeProjectFile(committed, path.string() + ".tmp", &error), "temporary meter recovery fixture");
  require(readProjectFileWithRecovery(path.string(), &loaded, &source, &error) &&
          source == ProjectLoadSource::Temporary && equal(loaded, committed), "temporary meter recovery fallback");
  require(readText(path) == "invalid primary\n", "recovery load modified primary");
}

}  // namespace

int main() {
  try {
    run();
    std::cout << "classical-daw project meter tests passed\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "FAIL: " << exception.what() << '\n';
    return 1;
  }
}
