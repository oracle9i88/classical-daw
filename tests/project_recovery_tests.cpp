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

daw::Score score(char step, double bpm) {
  daw::Score result;
  result.bpm = bpm;
  result.parts = {daw::ScorePart{"P1", "Recovery", {daw::ScoreMeasure{
      1, 0, {daw::ScoreNote{0, daw::kTicksPerQuarter, daw::ScorePitch{step, 0, 4}}}}}}};
  return result;
}

bool same(const daw::Score& left, const daw::Score& right) {
  return left.bpm == right.bpm && left.parts.size() == right.parts.size() &&
         !left.parts.empty() && !left.parts[0].measures.empty() && !left.parts[0].measures[0].notes.empty() &&
         left.parts[0].measures[0].notes[0].pitch.step == right.parts[0].measures[0].notes[0].pitch.step;
}

}  // namespace

int main() {
  using namespace daw;
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                     ("classical_daw_recovery_" + std::to_string(stamp) + ".cdaw");
  const std::filesystem::path recovery = path.string() + ".recovery";
  const std::filesystem::path temporary = path.string() + ".tmp";
  std::error_code cleanup_error;
  std::filesystem::remove(path, cleanup_error);
  std::filesystem::remove(recovery, cleanup_error);
  std::filesystem::remove(temporary, cleanup_error);

  const Score primary = score('C', 120.0);
  const Score recovered = score('E', 88.0);
  const Score interrupted = score('G', 66.0);
  std::string error;
  if (!writeProjectFile(primary, path.string(), &error)) return fail("primary write: " + error);
  if (!writeProjectRecoveryFile(recovered, path.string(), &error)) return fail("recovery write: " + error);
  if (!std::filesystem::exists(recovery) || std::filesystem::exists(recovery.string() + ".tmp")) {
    return fail("recovery sidecar replacement");
  }
  Score loaded = score('Z', 1.0);
  ProjectLoadSource source = ProjectLoadSource::None;
  if (!readProjectFileWithRecovery(path.string(), &loaded, &source, &error) ||
      source != ProjectLoadSource::Primary || !same(loaded, primary)) {
    return fail("primary load precedence: " + error);
  }

  std::filesystem::remove(path, cleanup_error);
  loaded = score('Z', 1.0);
  source = ProjectLoadSource::None;
  if (!readProjectFileWithRecovery(path.string(), &loaded, &source, &error) ||
      source != ProjectLoadSource::Recovery || !same(loaded, recovered)) {
    return fail("recovery load: " + error);
  }

  std::filesystem::remove(recovery, cleanup_error);
  if (!writeProjectFile(interrupted, temporary.string(), &error)) return fail("temporary candidate write: " + error);
  loaded = score('Z', 1.0);
  source = ProjectLoadSource::None;
  if (!readProjectFileWithRecovery(path.string(), &loaded, &source, &error) ||
      source != ProjectLoadSource::Temporary || !same(loaded, interrupted)) {
    return fail("temporary recovery load: " + error);
  }

  std::filesystem::remove(temporary, cleanup_error);
  loaded = score('Z', 1.0);
  const Score sentinel = loaded;
  source = ProjectLoadSource::Primary;
  if (readProjectFileWithRecovery(path.string(), &loaded, &source, &error) ||
      source != ProjectLoadSource::None || !same(loaded, sentinel)) {
    return fail("failed recovery mutated output");
  }

  if (readProjectFileWithRecovery(path.string(), nullptr, &source, &error) ||
      error != "project score output is null") {
    return fail("null recovery output validation");
  }

  std::filesystem::remove(path, cleanup_error);
  std::filesystem::remove(recovery, cleanup_error);
  std::filesystem::remove(temporary, cleanup_error);
  std::cout << "classical-daw project recovery tests passed\n";
  return 0;
}
