#include "daw/session_recovery.hpp"
#include "daw/session_mix.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template <class F> void rejects(F f) {
  bool rejected = false; try { f(); } catch (const std::exception&) { rejected = true; }
  require(rejected, "invalid recovery operation succeeded");
}
std::string read(const fs::path& path) { std::ifstream in(path, std::ios::binary); std::ostringstream text; text << in.rdbuf(); return text.str(); }
void write(const fs::path& path, const std::string& text) { std::ofstream out(path, std::ios::binary); out << text; if (!out) throw std::runtime_error("fixture write failed"); }
struct Temp {
  fs::path path;
  Temp() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path = fs::temp_directory_path() / ("daw-recovery-test-" + std::to_string(stamp));
    require(fs::create_directory(path), "cannot reserve test directory");
  }
  ~Temp() { std::error_code ec; fs::remove_all(path, ec); }
};
}
int main() {
  try {
    Temp temp;
    const auto source = temp.path / "source.dawsession", score = temp.path / "score.dawproj";
    const daw::Session initial{"score.dawproj", -3, {{"piano", "pianoteq", -4, .2, "", "p.aupreset", false, false, "p.dawfreeze"}}};
    const auto original = daw::serializeSession(initial);
    const std::string score_bytes = "exact score source bytes\n";
    write(source, original); write(score, score_bytes);
    daw::SessionMixState mix(initial);
    daw::SessionMixRecovery recovery(source.string(), original, score_bytes);
    const fs::path directory(recovery.directory()), latest = directory / "latest.mixrecovery";
    require(daw::listSessionMixRecoveries(source.string()).empty(), "incomplete run advertised as recovery");
    mix.apply({daw::MixParameter::Gain, "piano", -9});
    recovery.checkpoint(mix.current(), mix.revision());
    const auto first = read(latest);
    auto restored = daw::readSessionMixRecovery(source.string(), directory.string());
    require(restored.revision == 1 && restored.session.routes[0].gain_db == -9, "checkpoint lost edit");
    require(daw::listSessionMixRecoveries(source.string()).size() == 1, "checkpoint not discoverable");
    mix.apply({daw::MixParameter::Mute, "piano", 1});
    recovery.checkpoint(mix.current(), mix.revision());
    require(daw::readSessionMixRecovery(source.string(), directory.string()).session.routes[0].mute, "replacement lost mute");
    mix.undo(); recovery.checkpoint(mix.current(), mix.revision());
    require(!daw::readSessionMixRecovery(source.string(), directory.string()).session.routes[0].mute, "undo not checkpointed");
    mix.redo(); recovery.checkpoint(mix.current(), mix.revision());
    const auto expected = daw::serializeSession(mix.current()), good = read(latest);
    rejects([&] { recovery.checkpoint(mix.current(), 0); });
    rejects([&] { recovery.checkpoint(mix.current(), recovery.savedRevision()); });
    auto changed = mix.current(); changed.routes[0].instrument = "swam-cello";
    rejects([&] { recovery.checkpoint(changed, 99); });
    changed = mix.current(); changed.routes[0].frozen_file = "other.dawfreeze";
    rejects([&] { recovery.checkpoint(changed, 99); });
    changed = mix.current(); changed.score_file = "other.dawproj";
    rejects([&] { recovery.checkpoint(changed, 99); });
    require(read(latest) == good && recovery.savedRevision() == 4, "rejected checkpoint damaged prior state");

    // A second writer has independent ownership; it cannot replace the first run.
    daw::SessionMixRecovery other(source.string(), original, score_bytes);
    require(other.directory() != directory.string(), "writers share a recovery directory");
    other.checkpoint(initial, 1);
    require(daw::listSessionMixRecoveries(source.string()).size() == 2 && read(latest) == good, "other run overwrote history");
    daw::recoverSessionMix(source.string(), directory.string(), (temp.path / "restored.dawsession").string());
    require(read(temp.path / "restored.dawsession") == expected && read(source) == original && read(score) == score_bytes,
            "recovery modified source or lost mix fields");
    rejects([&] { daw::recoverSessionMix(source.string(), directory.string(), source.string()); });
    rejects([&] { daw::recoverSessionMix(source.string(), directory.string(), (temp.path / "restored.dawsession").string()); });

    // Simulate interrupted write: the old complete checkpoint remains valid.
    fs::create_directory(directory / ".saving"); write(directory / ".saving" / "checkpoint.tmp", "partial");
    mix.apply({daw::MixParameter::Master, "", -12});
    rejects([&] { recovery.checkpoint(mix.current(), mix.revision()); });
    require(read(latest) == good && recovery.savedRevision() == 4 &&
        read(directory / ".saving" / "checkpoint.tmp") == "partial", "failure damaged checkpoint or foreign staging");
    require(daw::readSessionMixRecovery(source.string(), directory.string()).revision == 4, "partial write hid complete checkpoint");
    fs::remove(directory / ".saving" / "checkpoint.tmp"); fs::remove(directory / ".saving");
    recovery.checkpoint(mix.current(), mix.revision()); // same failed revision can retry
    require(recovery.savedRevision() == 5, "failed revision could not retry");

    // Never silently attach an old snapshot to modified source material.
    write(source, daw::serializeSession(mix.current()));
    rejects([&] { daw::readSessionMixRecovery(source.string(), directory.string()); });
    rejects([&] { recovery.checkpoint(mix.current(), 6); });
    write(source, original); write(score, "changed score\n");
    rejects([&] { daw::readSessionMixRecovery(source.string(), directory.string()); });
    rejects([&] { daw::SessionMixRecovery stale(source.string(), original, score_bytes); });
    write(score, score_bytes);

    // Corruption, bounds, stale names, symlinks and malformed files fail closed.
    for (std::size_t n : {0U, 7U, 20U}) {
      write(latest, good.substr(0, n));
      rejects([&] { daw::readSessionMixRecovery(source.string(), directory.string()); });
    }
    auto corrupt = good; corrupt[corrupt.size() - 8] ^= 1;
    write(latest, corrupt); rejects([&] { daw::readSessionMixRecovery(source.string(), directory.string()); });
    write(latest, good + "extra"); rejects([&] { daw::readSessionMixRecovery(source.string(), directory.string()); });
    write(latest, std::string(1024U * 1024U + 25, 'x'));
    rejects([&] { daw::readSessionMixRecovery(source.string(), directory.string()); });
    write(latest, good);
    const auto alias = temp.path / "source.dawsession.mix-recovery-alias";
    fs::create_directory_symlink(directory, alias);
    rejects([&] { daw::readSessionMixRecovery(source.string(), alias.string()); });
    require(daw::listSessionMixRecoveries(source.string()).size() == 2, "symlink candidate included");
    fs::remove(latest); fs::create_symlink(temp.path / "absent", latest);
    rejects([&] { recovery.checkpoint(mix.current(), 6); });
    rejects([&] { daw::readSessionMixRecovery(source.string(), directory.string()); });
    require(!fs::exists(temp.path / "absent"), "recovery followed dangling symlink");
    fs::remove(latest); write(latest, first);
    require(daw::readSessionMixRecovery(source.string(), directory.string()).revision == 1, "valid old checkpoint failed");
    std::cout << "Mix recovery source binding, atomic replacement, isolation, corruption, undo/redo and no-overwrite tests passed\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
