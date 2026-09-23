#include "daw/frozen_track.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace fs = std::filesystem;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template <class F> void rejects(F function) {
  bool failed = false;
  try { function(); } catch (const std::exception&) { failed = true; }
  require(failed, "invalid frozen operation accepted");
}
struct Directory {
  fs::path path = fs::temp_directory_path() / ("daw-freeze-test-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Directory() { require(fs::create_directory(path), "cannot create test directory"); }
  ~Directory() { std::error_code ignored; fs::remove_all(path, ignored); }
};
}
int main() {
  try {
    Directory dir;
    const auto identity = daw::frozenTrackIdentity("score", "piano", "pianoteq", {1, 2, 3});
    const daw::AudioBuffer audio{48000, 2, {0.F, -0.F, 1.5F, -2.5F, .1234567F, -.25F}};
    const auto path = (dir.path / "raw.dawfreeze").string();
    daw::writeFrozenTrack(audio, identity, "Preset", 1234, path);
    const auto loaded = daw::readFrozenTrack(path, identity, 3);
    require(loaded.audio.samples == audio.samples && loaded.preset == "Preset" && loaded.component_version == 1234,
            "float headroom, precision or plugin metadata changed");
    rejects([&] { daw::writeFrozenTrack(audio, identity, "Preset", 1234, path); });
    rejects([&] { daw::readFrozenTrack(path, identity, 4); });
    rejects([&] { daw::readFrozenTrack(path, identity, 32U * 1024U * 1024U + 1); });
    for (const auto& stale : {daw::frozenTrackIdentity("new score", "piano", "pianoteq", {1, 2, 3}),
                            daw::frozenTrackIdentity("score", "cello", "pianoteq", {1, 2, 3}),
                            daw::frozenTrackIdentity("score", "piano", "swam-cello", {1, 2, 3}),
                            daw::frozenTrackIdentity("score", "piano", "pianoteq", {1, 2, 4})}) {
      rejects([&] { daw::readFrozenTrack(path, stale, 3); });
    }
    auto bad = audio; bad.samples[0] = std::numeric_limits<float>::quiet_NaN();
    const auto invalid = (dir.path / "invalid").string();
    rejects([&] { daw::writeFrozenTrack(bad, identity, "", 0, invalid); });
    require(!fs::exists(invalid), "invalid writer created output");
    const auto link = dir.path / "link"; fs::create_symlink(path, link);
    rejects([&] { daw::readFrozenTrack(link.string(), identity, 3); });
    const auto corrupt = dir.path / "corrupt";
    fs::copy_file(path, corrupt);
    {
      std::fstream file(corrupt, std::ios::in | std::ios::out | std::ios::binary);
      file.seekp(-5, std::ios::end); file.put('\x3e');  // finite float modification, bad CRC
    }
    rejects([&] { daw::readFrozenTrack(corrupt.string(), identity, 3); });
    fs::resize_file(corrupt, fs::file_size(corrupt) - 1);
    rejects([&] { daw::readFrozenTrack(corrupt.string(), identity, 3); });
    { std::ofstream file(path, std::ios::app | std::ios::binary); file.put('x'); }
    rejects([&] { daw::readFrozenTrack(path, identity, 3); });
    std::cout << "Frozen audio precision, identity, corruption and bounds checks passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
