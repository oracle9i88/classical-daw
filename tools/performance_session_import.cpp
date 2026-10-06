// Data-only migration of a saved score + explicit AU states. No plugin/device.
#include "daw/performance.hpp"
#include "daw/import_receipt.hpp"
#include "daw/project.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <algorithm>

namespace {
std::string read(const std::filesystem::path &path, std::size_t limit) {
  if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path)))
    throw std::runtime_error("dependency must be a regular file");
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  const auto n = in.tellg();
  if (!in || n <= 0 || static_cast<std::uint64_t>(n) > limit)
    throw std::runtime_error("dependency missing or exceeds bound");
  std::string text(static_cast<std::size_t>(n), '\0');
  in.seekg(0);
  in.read(text.data(), n);
  if (!in || in.peek() != std::char_traits<char>::eof())
    throw std::runtime_error("dependency read failed or changed");
  return text;
}
} // namespace
int main(int argc, char **argv) {
  std::string stage = "arguments";
  try {
    const bool unfreeze = argc == 4 && std::string(argv[3]) == "--unfreeze";
    if (argc != 3 && !unfreeze)
      throw std::runtime_error("usage: daw_performance_session_import SAVED_SESSION.dawsession "
                               "NEW_DIRECTORY [--unfreeze]");
    const std::filesystem::path source(argv[1]);
    stage = "session";
    const auto routing = daw::parseSession(read(source, 1024U * 1024));
    // The document keeps a nonpositive master and a +12 dB per-route ceiling.
    // Moving a positive master into EVERY route preserves even currently muted
    // or solo-excluded routes. Never clip/reduce gain to fit that representation.
    stage = "gain";
    const double gain_shift = std::max(0.0, routing.master_gain_db);
    for (const auto &route : routing.routes)
      if (route.gain_db + gain_shift > 12)
        throw std::runtime_error(
            "cannot preserve session output gain for part \"" + route.part_id +
            "\": positive master plus route gain exceeds the performance route limit of +12 dB; "
            "keep this session in the offline/frozen player or adjust its mix explicitly before "
            "importing. Nothing saved.");
    const auto score_path = source.parent_path() / routing.score_file;
    stage = "score";
    const auto fingerprint = daw::fingerprintImportSource(score_path.string());
    daw::PerformanceDocument document;
    std::string error;
    (void)read(score_path, 64U * 1024 * 1024);
    if (!daw::readProjectFile(score_path.string(), &document.score, &error))
      throw std::runtime_error(error);
    document.gain_db = routing.master_gain_db - gain_shift;
    std::size_t frozen = 0;
    for (const auto &route : routing.routes) {
      stage = "route_state";
      if (route.state_file.empty() || !route.preset.empty())
        throw std::runtime_error(
            "every route needs saved state; render/save the session once before migrating");
      if (!route.frozen_file.empty()) {
        if (!unfreeze)
          throw std::runtime_error(
              "frozen media migration is not implemented; --unfreeze explicitly rebuilds "
              "performance from score without copying audio caches");
        ++frozen;
      }
      const auto bytes = read(source.parent_path() / route.state_file, 16U * 1024 * 1024);
      document.routes.push_back({route.part_id,
                                 route.instrument,
                                 {bytes.begin(), bytes.end()},
                                 route.gain_db + gain_shift,
                                 route.balance,
                                 route.mute,
                                 route.solo,
                                 route.track_delay_us});
    }
    stage = "identity";
    daw::assignNoteIds(document.score);
    document.performances.push_back(daw::makePerformance(document.score, "Saved session timing"));
    stage = "validate";
    daw::validatePerformanceDocument(document);
    const auto after = daw::fingerprintImportSource(score_path.string());
    if (after.bytes != fingerprint.bytes || after.fnv1a64 != fingerprint.fnv1a64)
      throw std::runtime_error("score changed during migration");
    document.import_receipt =
        daw::makeImportReceipt(fingerprint, 0, {}, {}, {}, {}, document.score);
    stage = "save";
    daw::savePerformanceDocument(document, argv[2]);
    std::cout << "Saved multi-part performance: parts=" << document.score.parts.size()
              << " performed_notes=" << document.performances[0].mapping.size()
              << " routes=" << document.routes.size()
              << " plugins_opened=0 output_devices_opened=0 score_repairs=0 unfrozen_routes="
              << frozen << '\n';
    std::cout << "Saved score timing, controller bytes and route states retained. Author "
                 "part-scoped curves in the editor.\n";
    if (gain_shift)
      std::cout << "Positive master rebased: " << gain_shift
                << " dB moved to every route; document master is 0 dB and each route's effective "
                   "gain is unchanged.\n";
    if (frozen)
      std::cout << "Explicit unfreeze: performance rebuilt from score; frozen audio remains in the "
                   "source and was not migrated.\n";
  } catch (const std::exception &e) {
    std::cerr << "FAIL stage=" << stage << " reason=" << e.what() << '\n';
    return 1;
  }
}
