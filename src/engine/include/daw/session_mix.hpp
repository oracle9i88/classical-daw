#pragma once
#include "daw/session.hpp"

namespace daw {
class SessionPlayer;
enum class MixParameter { Gain, Balance, Mute, Solo, Master };
struct MixEdit {
  MixParameter parameter;
  std::string part_id; // Required for track edits; empty for Master.
  double value;
};

// Control-thread editable mix state, reusable by CLI and a future native UI.
// Tracks are addressed by stable part ID. If attached, the player must have
// matching route order and this object must be its only mix-command producer.
// State describes accepted targets, not the intermediate values of audio ramps.
class SessionMixState {
 public:
  explicit SessionMixState(Session initial);
  // Invalid edits throw. A full player queue returns false and leaves the
  // document unchanged. All allocations/validation precede queue submission.
  bool apply(const MixEdit& edit, SessionPlayer* player = nullptr);
  const Session& current() const noexcept { return session_; }
 private:
  Session session_;
};

// Save a new sibling session referencing the same media; never overwrite any
// existing file/symlink. Publishes a complete file by an atomic hard link.
// Fails explicitly on filesystems without hard-link support. No full-project
// transaction, autosave or power-loss durability guarantee is implied.
void saveNewSessionMix(const Session& session, const std::string& source_session,
                       const std::string& destination);
}
