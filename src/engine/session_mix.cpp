#include "daw/session_mix.hpp"
#include "daw/session_player.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <type_traits>

namespace daw {
SessionMixState::SessionMixState(Session initial) : session_(std::move(initial)) { validateSession(session_); }
bool SessionMixState::apply(const MixEdit& edit, SessionPlayer* player) {
  Session next = session_;
  PlaybackCommand command;
  if (edit.parameter == MixParameter::Master) {
    if (!edit.part_id.empty()) throw std::invalid_argument("master edit must not name a part");
    next.master_gain_db = edit.value;
    command.action = PlaybackAction::Master;
  } else {
    auto route = std::find_if(next.routes.begin(), next.routes.end(), [&](const auto& r) { return r.part_id == edit.part_id; });
    if (route == next.routes.end()) throw std::invalid_argument("unknown part ID");
    command.track = static_cast<std::size_t>(route - next.routes.begin());
    switch (edit.parameter) {
      case MixParameter::Gain: route->gain_db = edit.value; command.action = PlaybackAction::Gain; break;
      case MixParameter::Balance: route->balance = edit.value; command.action = PlaybackAction::Balance; break;
      case MixParameter::Mute: case MixParameter::Solo:
        if (edit.value != 0 && edit.value != 1) throw std::invalid_argument("mute/solo must be 0 or 1");
        if (edit.parameter == MixParameter::Mute) { route->mute = edit.value == 1; command.action = PlaybackAction::Mute; }
        else { route->solo = edit.value == 1; command.action = PlaybackAction::Solo; }
        break;
      default: throw std::invalid_argument("unknown mix parameter");
    }
  }
  validateSession(next);
  command.value = edit.value;
  if (player && !player->matchesRoutes(session_)) throw std::invalid_argument("player/document routes do not match");
  if (player && !player->enqueue(command)) return false;
  static_assert(std::is_nothrow_swappable<Session>::value, "accepted mix publication must not fail");
  std::swap(session_, next);
  return true;
}

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
