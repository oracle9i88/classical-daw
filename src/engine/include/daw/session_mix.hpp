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
  // Retains at most max_edits single-parameter edits (1..1024).
  explicit SessionMixState(Session initial, std::size_t max_edits = 128);
  // Invalid edits throw. A full player queue returns false and leaves the
  // document unchanged. All allocations/validation precede queue submission.
  bool apply(const MixEdit& edit, SessionPlayer* player = nullptr);
  // History is control-thread only and is not saved in .dawsession files.
  // Failed queue submissions leave state, revision and history cursor intact.
  bool undo(SessionPlayer* player = nullptr);
  bool redo(SessionPlayer* player = nullptr);
  bool canUndo() const noexcept { return cursor_ != 0; }
  bool canRedo() const noexcept { return cursor_ < history_.size(); }
  std::size_t historySize() const noexcept { return history_.size(); }
  std::uint64_t revision() const noexcept { return revision_; }
  const Session& current() const noexcept { return session_; }
 private:
  struct Change { MixEdit forward, inverse; };
  bool replay(bool forward, SessionPlayer* player);
  Session session_;
  std::vector<Change> history_;
  std::size_t cursor_ = 0, max_edits_ = 128;
  std::uint64_t revision_ = 0;
};

// Save a new sibling session referencing the same media; never overwrite any
// existing file/symlink. Publishes a complete file by an atomic hard link.
// Fails explicitly on filesystems without hard-link support. No full-project
// transaction, autosave or power-loss durability guarantee is implied.
void saveNewSessionMix(const Session& session, const std::string& source_session,
                       const std::string& destination);
}
