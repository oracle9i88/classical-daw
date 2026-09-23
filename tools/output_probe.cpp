// Opt-in hardware acceptance. Real output callbacks drive the real player;
// an observing wrapper replaces their samples with silence before the speaker.
#include "coreaudio_output.hpp"
#include "daw/session_player.hpp"
#include "daw/session_mix.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <filesystem>
#include <stdexcept>
#include <thread>

namespace {
void require(bool condition, const char* error) { if (!condition) throw std::runtime_error(error); }
template <typename Predicate> void until(Predicate predicate, const char* error) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= end) throw std::runtime_error(error);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}
class SilentObserver final : public daw::AudioOutputSource {
 public:
  explicit SilentObserver(daw::SessionPlayer& p) : player(p) {}
  bool acceptsFormat(double rate, std::uint32_t channels) const noexcept override {
    return player.acceptsFormat(rate, channels);
  }
  void render(float* output, std::uint32_t frames) noexcept override {
    player.render(output, frames);
    double value = 0;
    for (std::size_t i = 0; i < static_cast<std::size_t>(frames) * 2; ++i) {
      value = std::max(value, std::abs(static_cast<double>(output[i])));
      output[i] = 0; // No test tones reach the speaker.
    }
    peak.store(value, std::memory_order_relaxed);
  }
  daw::SessionPlayer& player;
  std::atomic<double> peak{0};
};
}
int main(int argc, char** argv) {
  try {
    const bool streaming = argc == 2 && std::string(argv[1]) == "--stream";
    require(argc == 1 || streaming, "usage: daw_output_probe [--stream]");
    struct Temporary {
      std::filesystem::path path;
      ~Temporary() { if (!path.empty()) { std::error_code ec; std::filesystem::remove_all(path, ec); } }
    } temp;
    daw::Session session{"score.dawproj", 0, {{"piano", "pianoteq", 0, 0, "", ""},
                                           {"cello", "swam-cello", 0, 0, "", ""}}};
    std::vector<daw::AudioBuffer> buffers;
    buffers.push_back({48000, 2, std::vector<float>(48000 * 2 * 10, .1F)});
    buffers.push_back({48000, 2, std::vector<float>(48000 * 2 * 10, .2F)});
    std::unique_ptr<daw::StreamingAudio> stream;
    if (streaming) {
      const auto candidate = std::filesystem::temp_directory_path() / ("daw-hardware-stream-" +
          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
      require(std::filesystem::create_directory(candidate), "cannot reserve probe directory");
      temp.path = candidate;
      std::vector<std::unique_ptr<daw::FrozenTrackReader>> readers;
      for (std::size_t i = 0; i < buffers.size(); ++i) {
        const auto path = (temp.path / (std::to_string(i) + ".dawfreeze")).string();
        daw::writeFrozenTrack(buffers[i], "probe", "probe", 1, path);
        readers.push_back(std::make_unique<daw::FrozenTrackReader>(path, "probe", buffers[i].frameCount()));
      }
      stream = std::make_unique<daw::StreamingAudio>(std::move(readers));
      buffers.clear(); buffers.shrink_to_fit();
    }
    auto owner = streaming ? std::make_unique<daw::SessionPlayer>(std::move(stream), session) :
                             std::make_unique<daw::SessionPlayer>(session, std::move(buffers));
    auto& player = *owner;
    daw::SessionMixState mix(session);
    SilentObserver observer(player);
    daw::CoreAudioOutput output;
    std::string error;
    if (!output.setAudioSource(&observer, &error) || !output.start(&error)) throw std::runtime_error(error);
    require(player.status().frame == 0 && !player.status().playing, "startup not paused");
    require(!output.setAudioSource(nullptr, &error), "live source replacement accepted");
    require(!output.setOutputDevice(0, &error), "live device replacement accepted");
    using A = daw::PlaybackAction;
    auto send = [&](A a, std::size_t track = 0, double value = 0, std::uint64_t frame = 0) {
      require(player.enqueue({a, track, value, frame}), "probe command rejected");
    };
    auto level = [&](double expected) {
      until([&] { return std::abs(observer.peak.load(std::memory_order_relaxed) - expected) < 1e-6; }, "hardware callback mix level mismatch");
    };
    send(A::Play); level(.3);
    require(player.status().frame > 0, "playback clock not advancing");
    using P = daw::MixParameter;
    require(mix.apply({P::Solo, "cello", 1}, &player), "solo rejected"); level(.2);
    require(mix.apply({P::Mute, "cello", 1}, &player), "mute rejected"); level(0);
    require(mix.apply({P::Solo, "piano", 1}, &player), "solo rejected"); level(.1);
    require(mix.apply({P::Gain, "piano", -20 * std::log10(2.)}, &player), "gain rejected"); level(.05);
    require(mix.apply({P::Master, "", -20 * std::log10(2.)}, &player), "master rejected"); level(.025);
    require(mix.undo(&player), "master undo rejected"); level(.05);
    require(mix.undo(&player), "gain undo rejected"); level(.1);
    require(mix.redo(&player), "gain redo rejected"); level(.05);
    require(mix.redo(&player), "master redo rejected"); level(.025);
    send(A::Pause);
    until([&] { return !player.status().playing; }, "pause not applied"); level(0);
    const auto paused_at = player.status().frame;
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    require(player.status().frame == paused_at, "paused position moved");
    send(A::Seek, 0, 0, 96000);
    until([&] { return player.status().frame == 96000; }, "paused seek not applied");
    send(A::Play); level(.025);
    require(player.status().frame > 96000, "resume failed");
    if (!output.checkHealth(&error)) throw std::runtime_error(error);
    const auto selected_device = output.currentDeviceId();
    output.stop();
    player.suspendAfterOutputStopped();
    const auto disconnected_at = player.status().frame;
    require(!player.status().playing && disconnected_at > 96000, "device stop lost position");
    require(!output.checkHealth(&error), "stopped output reported healthy");
    // A mix edit accepted while there is no callback must survive restart.
    require(mix.apply({P::Master, "", 0}, &player), "disconnected mix edit rejected");
    player.suspendAfterOutputStopped();
    if (!output.setOutputDevice(selected_device, &error)) throw std::runtime_error(error);
    if (!output.start(&error)) throw std::runtime_error(error);
    level(0);
    require(player.status().frame == disconnected_at && !player.status().playing, "reconnect resumed without play");
    if (!output.checkHealth(&error)) throw std::runtime_error(error);
    send(A::Play); level(.05);
    require(mix.undo(&player), "history lost across output restart"); level(.025);
    send(A::Stop);
    until([&] { return !player.status().playing && player.status().frame == 0; }, "stop failed"); level(0);
    require(!player.status().stream_failed, "stream disk error during hardware probe");
    std::cout << "mode=" << (streaming ? "streaming" : "resident")
              << " buffering_frames=" << player.status().buffering_frames << '\n';
    std::cout << output.diagnostics() << '\n';
    output.stop();
    const auto stopped_frames = output.renderedFrames();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    require(stopped_frames > 0 && output.renderedFrames() == stopped_frames, "callbacks continued after stop");
    require(output.xrunCount() == 0, "callback errors during control probe");
    if (!output.start(&error)) throw std::runtime_error(error);
    require(output.renderedFrames() > 0, "restart did not produce callbacks");
    output.stop();
    require(output.xrunCount() == 0 && player.status().clipped_samples == 0 &&
        player.status().rejected_commands == 0, "errors during restart");
    require(output.setAudioSource(nullptr, &error), "stopped source detach failed");
    std::cout << "PASS: actual hardware callbacks, play/pause/seek/stop, gain, master, mute/solo, undo/redo, health polling, paused reconnect and retained edits; speaker output silenced\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << "Output probe failed: " << e.what() << '\n'; return 1; }
}
