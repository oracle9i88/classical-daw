#include "daw/history.hpp"
#include "daw/project.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

int fail(const std::string& message) {
  std::cerr << "FAIL: " << message << '\n';
  return 1;
}

daw::Score score(char step, double bpm = 120.0) {
  daw::Score value;
  value.bpm = bpm;
  value.parts = {daw::ScorePart{"P1", "History", {daw::ScoreMeasure{
      1, 0, {daw::ScoreNote{0, daw::kTicksPerQuarter, daw::ScorePitch{step, 0, 4}}}}}}};
  return value;
}

bool isScore(const daw::Score& value, char step, double bpm = 120.0) {
  return value.bpm == bpm && value.parts.size() == 1U && value.parts[0].measures.size() == 1U &&
         value.parts[0].measures[0].notes.size() == 1U &&
         value.parts[0].measures[0].notes[0].pitch.step == step;
}

}  // namespace

int main() {
  using namespace daw;
  std::string error;

  ScoreHistory history(score('C'), 8U);
  if (!history.commit(score('E', 88.0), &error) || !history.commit(score('G', 66.0), &error)) {
    return fail("history setup: " + error);
  }

  Score output = score('X', 1.0);
  if (!history.snapshot(&output, &error) || !isScore(output, 'G', 66.0)) {
    return fail("snapshot current state: " + error);
  }
  if (history.snapshot(nullptr, &error) || error != "snapshot output score is null" ||
      !history.canUndo() || history.canRedo()) {
    return fail("snapshot null validation mutated history");
  }

  // Reset replaces the document baseline and drops both branches while
  // retaining the configured capacity.
  if (!history.reset(score('A', 44.0), &error) || history.size() != 1U || history.canUndo() || history.canRedo() ||
      !history.snapshot(&output, &error) || !isScore(output, 'A', 44.0)) {
    return fail("history reset: " + error);
  }

  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                     ("classical_daw_history_" + std::to_string(stamp) + ".cdaw");
  const std::filesystem::path recovery = path.string() + ".recovery";
  std::error_code cleanup_error;
  std::filesystem::remove(path, cleanup_error);
  std::filesystem::remove(recovery, cleanup_error);

  ScoreHistory persisted(score('C'), 8U);
  if (!persisted.commit(score('E', 99.0), &error) || !persisted.commit(score('G', 77.0), &error)) {
    return fail("persisted history setup: " + error);
  }
  if (!writeProjectRecoveryFile(persisted, path.string(), &error)) {
    return fail("history recovery sidecar write: " + error);
  }
  if (!std::filesystem::exists(recovery) || std::filesystem::exists(recovery.string() + ".tmp")) {
    return fail("history recovery sidecar replacement");
  }

  Score loaded = score('Z', 1.0);
  ProjectLoadSource source = ProjectLoadSource::None;
  if (!readProjectFileWithRecovery(path.string(), &loaded, &source, &error) ||
      source != ProjectLoadSource::Recovery || !isScore(loaded, 'G', 77.0)) {
    return fail("history recovery load: " + error);
  }

  // Rebuilding after load starts a clean history with no stale undo branch.
  ScoreHistory restored(score('X'), 4U);
  if (!restored.commit(score('Y'), &error) || !restored.reset(loaded, &error) || restored.size() != 1U ||
      restored.canUndo() || restored.canRedo() || !restored.snapshot(&output, &error) ||
      !isScore(output, 'G', 77.0)) {
    return fail("history rebuild after recovery: " + error);
  }

  // An invalid destination is rejected after snapshotting without changing
  // the history; this also exercises the overload's failure path.
  if (writeProjectRecoveryFile(restored, "", &error) || error != "project path is empty" || restored.size() != 1U ||
      !restored.snapshot(&output, &error) || !isScore(output, 'G', 77.0)) {
    return fail("history recovery invalid path protection: " + error);
  }

  std::filesystem::remove(path, cleanup_error);
  std::filesystem::remove(recovery, cleanup_error);
  std::filesystem::remove(recovery.string() + ".tmp", cleanup_error);
  std::cout << "classical-daw project history tests passed\n";
  return 0;
}
