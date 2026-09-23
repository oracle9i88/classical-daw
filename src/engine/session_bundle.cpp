#include "daw/session_bundle.hpp"
#include "daw/session.hpp"
#include "daw/project.hpp"
#include "daw/frozen_track.hpp"
#include "daw/audio_limits.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <system_error>
#if defined(__APPLE__)
#include <sys/stdio.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace daw {
namespace fs = std::filesystem;
namespace {
constexpr std::size_t session_limit = 1024U*1024U, score_limit = 64U*1024U*1024U, state_limit = 16U*1024U*1024U;
constexpr std::uint64_t frozen_limit = static_cast<std::uint64_t>(kMaxStreamAudioFrames)*8 + 81ULL*1024*1024 + 8192;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::ifstream open(const fs::path& path, std::uint64_t limit, std::uint64_t& size) {
  require(fs::is_regular_file(fs::symlink_status(path)), "bundle dependency must be a regular file, not a symlink");
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  const auto end = input.tellg();
  require(bool(input) && end >= 0 && static_cast<std::uint64_t>(end) <= limit, "bundle dependency unreadable or exceeds size limit");
  size = static_cast<std::uint64_t>(end); input.seekg(0);
  return input;
}
std::string read(const fs::path& path, std::size_t limit) {
  std::uint64_t size = 0; auto input = open(path, limit, size);
  std::string bytes(static_cast<std::size_t>(size), '\0');
  input.read(bytes.data(), static_cast<std::streamsize>(size));
  require(bool(input) && input.peek() == std::char_traits<char>::eof() && !input.bad(), "bundle dependency changed or read failed");
  return bytes;
}
void write(const fs::path& path, const std::string& bytes) {
  require(!fs::exists(fs::symlink_status(path)), "staged bundle file already exists");
  std::ofstream output(path, std::ios::binary);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); output.close();
  require(bool(output), "bundle metadata write failed");
}
void copy(const fs::path& source, const fs::path& destination, std::uint64_t limit) {
  std::uint64_t size = 0; auto input = open(source, limit, size);
  require(!fs::exists(fs::symlink_status(destination)), "staged bundle file already exists");
  std::ofstream output(destination, std::ios::binary);
  require(bool(output), "cannot create staged dependency");
  std::array<char, 65536> block{};
  for (std::uint64_t at = 0; at < size;) {
    const auto count = static_cast<std::streamsize>(std::min<std::uint64_t>(block.size(), size-at));
    input.read(block.data(), count);
    require(bool(input), "bundle dependency truncated during copy");
    output.write(block.data(), count);
    require(bool(output), "bundle dependency copy failed");
    at += static_cast<std::uint64_t>(count);
  }
  require(input.peek() == std::char_traits<char>::eof() && !input.bad(), "bundle dependency grew or read failed");
  output.close(); require(bool(output), "bundle dependency close failed");
}
void requireFrozen(const Session& session) {
  for (const auto& route : session.routes)
    require(!route.state_file.empty() && !route.frozen_file.empty(), "collection requires saved state and frozen audio for every route; render first");
}
struct Staging {
  fs::path root;
  std::vector<fs::path> owned;
  bool published = false;
  explicit Staging(fs::path path) : root(std::move(path)) {
    require(fs::create_directory(root), "collection staging already exists; preserve or inspect it before retrying");
  }
  fs::path file(const std::string& name) { owned.push_back(root/name); return owned.back(); }
  ~Staging() {
    if (published) return;
    std::error_code ec;
    for (const auto& path : owned) fs::remove(path, ec);
    fs::remove(root, ec); // Never recursively remove another owner's files.
  }
};
void publish(const fs::path& from, const fs::path& to) {
#if defined(__APPLE__)
  if (::renamex_np(from.c_str(), to.c_str(), RENAME_EXCL) == 0) return;
  throw std::system_error(errno, std::generic_category(), "exclusive bundle publication failed");
#elif defined(__linux__) && defined(SYS_renameat2)
  if (::syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE) == 0) return;
  throw std::system_error(errno, std::generic_category(), "exclusive bundle publication failed");
#else
  (void)from; (void)to;
  throw std::runtime_error("exclusive directory publication is unsupported on this platform");
#endif
}
}

SessionBundleReport verifySessionBundle(const std::string& session_path) {
  const fs::path input(session_path), root = input.parent_path();
  const auto session_bytes = read(input, session_limit);
  const auto session = parseSession(session_bytes); requireFrozen(session);
  const auto score_bytes = read(root/session.score_file, score_limit);
  Score score; std::string error;
  if (!readProjectFile((root/session.score_file).string(), &score, &error)) throw std::runtime_error("bundle score: "+error);
  const auto plan = planSession(session, score, 48000, 5, SessionPlanMode::Streaming);
  SessionBundleReport report{session.routes.size(), plan.frames,
      static_cast<std::uint64_t>(session_bytes.size()+score_bytes.size())};
  std::size_t total_states = 0;
  for (const auto& route : session.routes) {
    const auto bytes = read(root/route.state_file, state_limit);
    require(!bytes.empty(), "bundle state is empty");
    total_states += bytes.size(); require(total_states <= score_limit, "bundle instrument states exceed 64 MiB");
    const std::vector<std::uint8_t> state(bytes.begin(), bytes.end());
    const auto identity = frozenTrackIdentity(score_bytes, route.part_id, route.instrument, state);
    FrozenTrackReader reader((root/route.frozen_file).string(), identity, plan.frames);
    report.referenced_bytes += bytes.size() + fs::file_size(root/route.frozen_file);
  }
  return report;
}

SessionBundleReport collectSessionBundle(const std::string& source_session, const std::string& new_directory) {
  const fs::path source(source_session), destination(new_directory);
  require(!destination.empty() && !destination.filename().empty() && destination.filename() != "." && destination.filename() != "..",
          "collection destination needs a new directory name");
  require(!fs::exists(fs::symlink_status(destination)), "collection destination already exists");
  const auto original = parseSession(read(source, session_limit)); requireFrozen(original);
  const auto root = source.parent_path();
  Staging staging(destination.string()+".collecting");
  Session saved = original; saved.score_file = "score.dawproj";
  copy(root/original.score_file, staging.file(saved.score_file), score_limit);
  std::size_t state_total = 0;
  for (std::size_t i = 0; i < original.routes.size(); ++i) {
    const auto& route = original.routes[i]; auto& target = saved.routes[i];
    const auto base = "track-"+std::to_string(i+1);
    target.state_file = base+".aupreset"; target.frozen_file = base+".dawfreeze";
    const auto state = read(root/route.state_file, state_limit);
    require(!state.empty(), "bundle state is empty");
    state_total += state.size(); require(state_total <= score_limit, "bundle instrument states exceed 64 MiB");
    write(staging.file(target.state_file), state);
    copy(root/route.frozen_file, staging.file(target.frozen_file), frozen_limit);
  }
  const auto entry = staging.file("session.dawsession");
  write(entry, serializeSession(saved));
  // Validate the bytes that will actually move, not only the originals.
  const auto report = verifySessionBundle(entry.string());
  publish(staging.root, destination); staging.published = true;
  return report;
}
}
