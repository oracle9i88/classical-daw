#include "daw/session_bundle.hpp"
#include "daw/session.hpp"
#include "daw/project.hpp"
#include "daw/frozen_track.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F f) { bool failed = false; try { f(); } catch (const std::exception&) { failed = true; } require(failed, "invalid collection accepted"); }
std::string read(const fs::path& path) { std::ifstream in(path, std::ios::binary); return {std::istreambuf_iterator<char>(in), {}}; }
void write(const fs::path& path, const std::string& bytes) { std::ofstream out(path, std::ios::binary); out << bytes; }
struct Temp {
  fs::path root = fs::temp_directory_path()/("daw-collection-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Temp() { require(fs::create_directory(root), "cannot create test directory"); }
  ~Temp() { std::error_code ec; fs::remove_all(root, ec); }
};
}
int main() {
  try {
    Temp temp;
    const auto source = temp.root/"source"; fs::create_directory(source);
    daw::Score score;
    daw::ScoreNote note; note.duration = 960; note.midi_channel = 0;
    score.parts = {{"piano", "Piano", {{1, 0, {note}, 3840}}}, {"second", "Second", {{1, 0, {note}, 3840}}}};
    // Source names deliberately collide with the OTHER roles' target names.
    daw::Session session{"track-1.aupreset", -3,
      {{"second", "pianoteq", -9, .25, "", "score.dawproj", true, false, "second.raw"},
       {"piano", "pianoteq", -4, -.3, "", "score.dawproj", false, true, "piano.raw"}}};
    std::string error;
    require(daw::writeProjectFile(score, (source/session.score_file).string(), &error), "fixture score failed");
    const auto score_bytes = read(source/session.score_file);
    const std::string state_bytes("state\0bytes", 11);
    write(source/"score.dawproj", state_bytes);
    const std::vector<std::uint8_t> state(state_bytes.begin(), state_bytes.end());
    const auto plan = daw::planSession(session, score);
    daw::AudioBuffer audio{48000, 2, std::vector<float>(plan.frames*2, .25F)};
    audio.samples[1] = -0.F; audio.samples[7] = 1.5F; // Raw headroom must not be quantized/clipped.
    for (const auto& route : session.routes) daw::writeFrozenTrack(audio,
        daw::frozenTrackIdentity(score_bytes, route.part_id, route.instrument, state), "Preset", 123,
        (source/route.frozen_file).string());
    const auto entry = source/"edited mix.dawsession";
    const auto session_bytes = daw::serializeSession(session); write(entry, session_bytes);
    write(source/"mix.wav", "stale export must not be copied");
    fs::create_directory(source/"old.mix-recovery-1");
    const auto original_first = read(source/"second.raw");
    const auto destination = temp.root/"collected";
    const auto report = daw::collectSessionBundle(entry.string(), destination.string());
    require(report.tracks == 2 && report.frames == plan.frames, "collection plan changed");
    require(!fs::exists(destination.string()+".collecting"), "staging leaked after publish");
    require(!fs::exists(destination/"mix.wav") && !fs::exists(destination/"old.mix-recovery-1"), "unreferenced exports/recovery were copied");
    require(read(destination/"score.dawproj") == score_bytes && read(destination/"track-1.aupreset") == state_bytes &&
            read(destination/"track-2.aupreset") == state_bytes && read(destination/"track-1.dawfreeze") == original_first,
            "source data changed during collection");
    auto expected = session; expected.score_file = "score.dawproj";
    for (std::size_t i = 0; i < expected.routes.size(); ++i) {
      expected.routes[i].state_file = "track-"+std::to_string(i+1)+".aupreset";
      expected.routes[i].frozen_file = "track-"+std::to_string(i+1)+".dawfreeze";
    }
    require(read(destination/"session.dawsession") == daw::serializeSession(expected), "saved mix or part routing changed");
    require(!fs::equivalent(source/"second.raw", destination/"track-1.dawfreeze") &&
            !fs::equivalent(source/"score.dawproj", destination/"track-1.aupreset"), "collection shared mutable source inodes");
    std::uint64_t size = 0; for (const auto& file : fs::directory_iterator(destination)) size += fs::file_size(file.path());
    require(size == report.referenced_bytes, "collection byte accounting wrong");
    fs::rename(source, temp.root/"source-moved");
    require(daw::verifySessionBundle((destination/"session.dawsession").string()).frames == plan.frames,
            "collected session still depends on old source location");
    fs::rename(temp.root/"source-moved", source);
    require(read(entry) == session_bytes && read(source/"second.raw") == original_first, "source modified");

    auto failure = [&](const std::string& name) {
      const auto path = temp.root/name;
      rejects([&] { daw::collectSessionBundle(entry.string(), path.string()); });
      require(!fs::exists(path) && !fs::exists(path.string()+".collecting"), "failed collection left output or owned staging");
    };
    write(source/"second.raw", original_first.substr(0, original_first.size()-1)); failure("truncated");
    auto corrupt = original_first; corrupt.back() ^= 1; write(source/"second.raw", corrupt); failure("corrupt");
    write(source/"second.raw", original_first);
    write(source/"score.dawproj", "different state"); failure("stale-state");
    write(source/"score.dawproj", ""); failure("empty-state");
    write(source/"score.dawproj", state_bytes);
    write(source/session.score_file, score_bytes+"\n"); failure("stale-score");
    write(source/session.score_file, score_bytes);
    fs::rename(source/"second.raw", source/"hidden.raw"); failure("missing");
    fs::create_symlink(source/"hidden.raw", source/"second.raw"); failure("symlink");
    fs::remove(source/"second.raw"); fs::rename(source/"hidden.raw", source/"second.raw");
    auto no_media = session; no_media.routes[0].frozen_file.clear(); write(entry, daw::serializeSession(no_media)); failure("not-rendered");
    write(entry, session_bytes);
    auto missing_route = session; missing_route.routes[0].part_id = "absent"; write(entry, daw::serializeSession(missing_route)); failure("bad-route");
    write(entry, session_bytes);
    // Existing destination, including empty directory and dangling link, is never replaced.
    rejects([&] { daw::collectSessionBundle(entry.string(), destination.string()); });
    const auto empty = temp.root/"empty"; fs::create_directory(empty);
    rejects([&] { daw::collectSessionBundle(entry.string(), empty.string()); }); require(fs::is_empty(empty), "empty destination touched");
    const auto link = temp.root/"link"; fs::create_symlink(temp.root/"absent", link);
    rejects([&] { daw::collectSessionBundle(entry.string(), link.string()); }); require(fs::is_symlink(link), "destination link deleted");
    const auto stale = temp.root/"stale"; fs::create_directory(stale.string()+".collecting");
    write(fs::path(stale.string()+".collecting")/"keep", "previous crash");
    rejects([&] { daw::collectSessionBundle(entry.string(), stale.string()); });
    require(read(fs::path(stale.string()+".collecting")/"keep") == "previous crash", "stale staging removed");
    // Concurrent collectors can publish exactly one complete result.
    const auto racing = (temp.root/"racing").string(); std::atomic<int> successes{0};
    auto collect = [&] { try { daw::collectSessionBundle(entry.string(), racing); ++successes; } catch (const std::exception&) {} };
    std::thread a(collect), b(collect); a.join(); b.join();
    require(successes == 1 && daw::verifySessionBundle((fs::path(racing)/"session.dawsession").string()).tracks == 2,
            "concurrent collectors published an incomplete or duplicate result");
    require(read(entry) == session_bytes && read(source/"second.raw") == original_first, "validation altered source");
    std::cout << "PASS self-contained relocation, exact bytes/mix, shared states, corruption cleanup, no-overwrite and concurrent collection\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
