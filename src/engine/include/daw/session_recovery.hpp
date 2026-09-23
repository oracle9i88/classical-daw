#pragma once
#include "daw/session.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace daw {
struct RecoveredMix {
  Session session;
  std::uint64_t revision = 0;
};
// Control-thread only. Each writer exclusively owns a new sibling directory;
// other runs' checkpoints and the source bundle are never overwritten/deleted.
// Call after an accepted edit. Failure does not roll back that audible edit:
// callers must report the failure and may retry the same revision.
class SessionMixRecovery {
 public:
  SessionMixRecovery(const std::string& source, const std::string& source_bytes,
                     const std::string& score_bytes);
  SessionMixRecovery(const SessionMixRecovery&) = delete;
  SessionMixRecovery& operator=(const SessionMixRecovery&) = delete;
  void checkpoint(const Session& current, std::uint64_t revision);
  const std::string& directory() const noexcept { return directory_; }
  std::uint64_t savedRevision() const noexcept { return saved_revision_; }
 private:
  std::string source_, source_bytes_, directory_;
  Session baseline_;
  std::uint64_t saved_revision_ = 0;
};
// List candidates, not validated recoveries. A load checks exact original
// session/score bytes, mix-only changes, bounded format and checksum. It does
// not restore media, plugin state, transport, or undo history. Frozen playback
// still independently validates the score/plugin-state/audio binding.
std::vector<std::string> listSessionMixRecoveries(const std::string& source);
RecoveredMix readSessionMixRecovery(const std::string& source, const std::string& directory);
// Restores only to a new sibling, using the normal no-overwrite save contract.
void recoverSessionMix(const std::string& source, const std::string& directory,
                       const std::string& destination);
} // namespace daw
