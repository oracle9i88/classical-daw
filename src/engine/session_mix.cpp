#include "daw/session_mix.hpp"
#include "daw/session_player.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <type_traits>
#include <limits>

namespace daw {
namespace {
struct PreparedEdit {
  Session next;
  PlaybackCommand command;
  double previous = 0;
};
PreparedEdit prepareEdit(const Session& current, const MixEdit& edit) {
  PreparedEdit prepared{current, {}, 0};
  auto& next = prepared.next;
  auto& command = prepared.command;
  if (edit.parameter == MixParameter::Master) {
    if (!edit.part_id.empty()) throw std::invalid_argument("master edit must not name a part");
    prepared.previous = next.master_gain_db;
    next.master_gain_db = edit.value;
    command.action = PlaybackAction::Master;
  } else {
    auto route = std::find_if(next.routes.begin(), next.routes.end(), [&](const auto& r) { return r.part_id == edit.part_id; });
    if (route == next.routes.end()) throw std::invalid_argument("unknown part ID");
    command.track = static_cast<std::size_t>(route - next.routes.begin());
    switch (edit.parameter) {
      case MixParameter::Gain:
        prepared.previous = route->gain_db; route->gain_db = edit.value; command.action = PlaybackAction::Gain; break;
      case MixParameter::Balance:
        prepared.previous = route->balance; route->balance = edit.value; command.action = PlaybackAction::Balance; break;
      case MixParameter::Mute: case MixParameter::Solo:
        if (edit.value != 0 && edit.value != 1) throw std::invalid_argument("mute/solo must be 0 or 1");
        if (edit.parameter == MixParameter::Mute) {
          prepared.previous = route->mute ? 1 : 0; route->mute = edit.value == 1; command.action = PlaybackAction::Mute;
        } else {
          prepared.previous = route->solo ? 1 : 0; route->solo = edit.value == 1; command.action = PlaybackAction::Solo;
        }
        break;
      default: throw std::invalid_argument("unknown mix parameter");
    }
  }
  validateSession(next);
  command.value = edit.value;
  return prepared;
}
void requireRevisionSpace(std::uint64_t revision) {
  if (revision == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("mix revision overflow");
}
}
SessionMixState::SessionMixState(Session initial, std::size_t max_edits)
    : session_(std::move(initial)), max_edits_(max_edits) {
  validateSession(session_);
  if (!max_edits_ || max_edits_ > 1024) throw std::invalid_argument("mix history limit must be 1..1024");
}
bool SessionMixState::apply(const MixEdit& edit, SessionPlayer* player) {
  auto prepared = prepareEdit(session_, edit);
  if (player && !player->matchesRoutes(session_)) throw std::invalid_argument("player/document routes do not match");
  // No-op edits preserve redo and do not consume queue slots or history.
  if (prepared.previous == edit.value) return true;
  requireRevisionSpace(revision_);
  std::vector<Change> next_history;
  const auto start = cursor_ == max_edits_ ? 1U : 0U;
  next_history.reserve(cursor_ - start + 1);
  next_history.insert(next_history.end(), history_.begin() + start,
                      history_.begin() + static_cast<std::ptrdiff_t>(cursor_));
  next_history.push_back({edit, {edit.parameter, edit.part_id, prepared.previous}});
  // Every potentially throwing step precedes submission; publishing accepted
  // targets/history after it uses only noexcept swaps and integer updates.
  if (player && !player->enqueue(prepared.command)) return false;
  static_assert(std::is_nothrow_swappable<Session>::value, "accepted mix publication must not fail");
  std::swap(session_, prepared.next);
  history_.swap(next_history);
  cursor_ = history_.size();
  ++revision_;
  return true;
}
bool SessionMixState::replay(bool forward, SessionPlayer* player) {
  if (forward ? !canRedo() : !canUndo()) return false;
  const auto& change = history_[forward ? cursor_ : cursor_ - 1];
  auto prepared = prepareEdit(session_, forward ? change.forward : change.inverse);
  if (player && !player->matchesRoutes(session_)) throw std::invalid_argument("player/document routes do not match");
  requireRevisionSpace(revision_);
  if (player && !player->enqueue(prepared.command)) return false;
  std::swap(session_, prepared.next);
  if (forward) ++cursor_; else --cursor_;
  ++revision_;
  return true;
}
bool SessionMixState::undo(SessionPlayer* player) { return replay(false, player); }
bool SessionMixState::redo(SessionPlayer* player) { return replay(true, player); }

void saveNewSessionMix(const Session& session, const std::string& source_session, const std::string& destination) {
  namespace fs = std::filesystem;
  const auto text = serializeSession(session);
  const fs::path source(source_session), output(destination);
  if (!fs::is_regular_file(fs::symlink_status(source))) throw std::runtime_error("source session must be a regular file");
  auto parent = [](const fs::path& p) { return fs::canonical(p.has_parent_path() ? p.parent_path() : fs::path(".")); };
  if (parent(source) != parent(output)) throw std::runtime_error("new mix must stay beside its source session/media");
  if (fs::exists(fs::symlink_status(output))) throw std::runtime_error("output session must be new: destination session already exists");
  const auto staging = output.parent_path() / (output.filename().string() + ".saving");
  // Reserve a private staging directory exclusively. Never open a stale file
  // or remove a directory owned by another writer/previous interrupted save.
  if (!fs::create_directory(staging)) throw std::runtime_error("mix save staging path already exists");
  struct Cleanup {
    fs::path directory;
    ~Cleanup() { std::error_code ec; fs::remove(directory / "session.tmp", ec); fs::remove(directory, ec); }
  } cleanup{staging};
  const auto temp = staging / "session.tmp";
  std::ofstream file(temp, std::ios::binary);
  file.write(text.data(), static_cast<std::streamsize>(text.size())); file.close();
  if (!file) throw std::runtime_error("mix save write failed");
  fs::create_hard_link(temp, output); // Atomic no-replace publication, also rejects dangling symlinks.
}
}
