#include "daw/session_recovery.hpp"
#include "daw/session_mix.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>

namespace daw {
namespace {
namespace fs = std::filesystem;
constexpr std::size_t session_limit = 1024U * 1024U, score_limit = 64U * 1024U * 1024U;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::string read(const fs::path& path, std::size_t limit) {
  require(fs::is_regular_file(fs::symlink_status(path)), "recovery input must be a regular file, not a symlink");
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  const auto size = in.tellg();
  require(bool(in) && size >= 0 && static_cast<std::uintmax_t>(size) <= limit, "recovery input unreadable or exceeds size limit");
  std::string bytes(static_cast<std::size_t>(size), '\0');
  in.seekg(0); in.read(bytes.data(), static_cast<std::streamsize>(size));
  require(bool(in), "recovery read failed");
  return bytes;
}
void write(const fs::path& path, const std::string& bytes) {
  require(!fs::exists(fs::symlink_status(path)), "recovery output already exists");
  std::ofstream out(path, std::ios::binary);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); out.close();
  require(bool(out), "recovery write failed");
}
fs::path sourcePath(const std::string& source) {
  require(fs::is_regular_file(fs::symlink_status(source)), "recovery source must be a regular file");
  return fs::canonical(source);
}
std::string prefix(const fs::path& source) { return source.filename().string() + ".mix-recovery-"; }
void mixOnly(const Session& baseline, const Session& current) {
  validateSession(current);
  require(baseline.routes.size() == current.routes.size(), "recovery accepts mix-only changes");
  Session normalized = current;
  normalized.master_gain_db = baseline.master_gain_db;
  for (std::size_t i = 0; i < baseline.routes.size(); ++i) {
    auto& r = normalized.routes[i]; const auto& old = baseline.routes[i];
    r.gain_db = old.gain_db; r.balance = old.balance; r.mute = old.mute; r.solo = old.solo;
  }
  require(serializeSession(normalized) == serializeSession(baseline), "recovery accepts mix-only changes");
}
void append(std::string& bytes, std::uint64_t value, unsigned count) {
  for (unsigned i = 0; i < count; ++i) bytes.push_back(static_cast<char>((value >> (8 * i)) & 255U));
}
std::uint64_t integer(const std::string& bytes, std::size_t offset, unsigned count) {
  std::uint64_t value = 0;
  for (unsigned i = 0; i < count; ++i) value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes.at(offset + i))) << (8 * i);
  return value;
}
std::uint32_t crc(const std::string& bytes, std::size_t size) {
  std::uint32_t value = 0xffffffffU;
  for (std::size_t i = 0; i < size; ++i) {
    value ^= static_cast<unsigned char>(bytes[i]);
    for (int bit = 0; bit < 8; ++bit) value = (value >> 1) ^ ((value & 1) ? 0xedb88320U : 0U);
  }
  return value ^ 0xffffffffU;
}
}
SessionMixRecovery::SessionMixRecovery(const std::string& source, const std::string& source_bytes,
                                       const std::string& score_bytes)
    : source_(sourcePath(source).string()), source_bytes_(source_bytes), baseline_(parseSession(source_bytes)) {
  require(read(source_, session_limit) == source_bytes_, "source session changed since opening");
  const fs::path root = fs::path(source_).parent_path();
  require(!score_bytes.empty() && read(root / baseline_.score_file, score_limit) == score_bytes,
          "source score changed since opening");
  std::random_device random;
  for (unsigned attempt = 0; attempt < 32; ++attempt) {
    const auto name = prefix(source_) + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(random());
    const auto candidate = root / name;
    if (fs::create_directory(candidate)) { directory_ = candidate.string(); break; }
  }
  require(!directory_.empty(), "cannot reserve recovery directory");
  try {
    fs::permissions(directory_, fs::perms::owner_all, fs::perm_options::replace);
    write(fs::path(directory_) / "source.dawsession", source_bytes_);
    write(fs::path(directory_) / "source.dawproj", score_bytes);
  } catch (...) {
    std::error_code ec;
    fs::remove(fs::path(directory_) / "source.dawsession", ec);
    fs::remove(fs::path(directory_) / "source.dawproj", ec);
    fs::remove(directory_, ec);
    throw;
  }
}
void SessionMixRecovery::checkpoint(const Session& current, std::uint64_t revision) {
  require(revision > 0 && revision > saved_revision_, "recovery revision must advance");
  mixOnly(baseline_, current);
  require(read(source_, session_limit) == source_bytes_, "source session changed since opening");
  const auto session = serializeSession(current);
  std::string bytes = "DAWMIX01";
  append(bytes, revision, 8); append(bytes, session.size(), 4); bytes += session;
  append(bytes, crc(bytes, bytes.size()), 4);
  const fs::path directory(directory_), staging = directory / ".saving", latest = directory / "latest.mixrecovery";
  require(fs::is_directory(fs::symlink_status(directory)), "recovery directory is missing or a symlink");
  require(!fs::exists(fs::symlink_status(latest)) || fs::is_regular_file(fs::symlink_status(latest)),
          "recovery target must be a regular file");
  require(fs::create_directory(staging), "recovery staging already exists; last checkpoint preserved");
  struct Cleanup {
    fs::path directory;
    ~Cleanup() { std::error_code ec; fs::remove(directory / "checkpoint.tmp", ec); fs::remove(directory, ec); }
  } cleanup{staging};
  write(staging / "checkpoint.tmp", bytes);
  fs::rename(staging / "checkpoint.tmp", latest);
  saved_revision_ = revision;
}
std::vector<std::string> listSessionMixRecoveries(const std::string& source) {
  const auto path = sourcePath(source);
  std::vector<std::string> result;
  for (const auto& entry : fs::directory_iterator(path.parent_path())) {
    if (entry.path().filename().string().rfind(prefix(path), 0) != 0 ||
        !fs::is_directory(entry.symlink_status())) continue;
    if (fs::is_regular_file(fs::symlink_status(entry.path() / "latest.mixrecovery"))) result.push_back(entry.path().string());
  }
  std::sort(result.begin(), result.end());
  return result;
}
RecoveredMix readSessionMixRecovery(const std::string& source, const std::string& directory) {
  const auto path = sourcePath(source);
  require(fs::is_directory(fs::symlink_status(directory)), "recovery directory must not be a symlink");
  const auto dir = fs::canonical(directory);
  require(dir.parent_path() == path.parent_path() && dir.filename().string().rfind(prefix(path), 0) == 0,
          "recovery directory must belong to this sibling session");
  const auto baseline_bytes = read(dir / "source.dawsession", session_limit);
  require(read(path, session_limit) == baseline_bytes, "recovery source session changed; refusing stale recovery");
  const auto baseline = parseSession(baseline_bytes);
  require(read(path.parent_path() / baseline.score_file, score_limit) == read(dir / "source.dawproj", score_limit),
          "recovery score changed; refusing stale recovery");
  const auto bytes = read(dir / "latest.mixrecovery", session_limit + 24);
  require(bytes.size() >= 24 && bytes.compare(0, 8, "DAWMIX01") == 0, "unknown or truncated recovery format");
  const auto size = integer(bytes, 16, 4);
  require(size <= session_limit && size == bytes.size() - 24, "invalid recovery length");
  require(integer(bytes, bytes.size() - 4, 4) == crc(bytes, bytes.size() - 4), "recovery checksum mismatch");
  const auto revision = integer(bytes, 8, 8);
  require(revision > 0, "invalid recovery revision");
  auto current = parseSession(bytes.substr(20, static_cast<std::size_t>(size)));
  mixOnly(baseline, current);
  return {std::move(current), revision};
}
void recoverSessionMix(const std::string& source, const std::string& directory, const std::string& destination) {
  const auto recovered = readSessionMixRecovery(source, directory);
  saveNewSessionMix(recovered.session, source, destination);
}
} // namespace daw
