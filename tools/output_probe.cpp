// Opt-in hardware acceptance. Real output callbacks drive the real player;
// an observing wrapper replaces their samples with silence before the speaker.
#include "coreaudio_output.hpp"
#include "daw/session_player.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
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
int main() {
  try {
    daw::Session session{"score.dawproj", 0, {{"piano", "pianoteq", 0, 0, "", ""},
                                           {"cello", "swam-cello", 0, 0, "", ""}}};
    std::vector<daw::AudioBuffer> buffers;
    buffers.push_back({48000, 2, std::vector<float>(48000 * 2 * 10, .1F)});
    buffers.push_back({48000, 2, std::vector<float>(48000 * 2 * 10, .2F)});
    daw::SessionPlayer player(session, std::move(buffers));
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
    send(A::Solo, 1, 1); level(.2);
    send(A::Mute, 1, 1); level(0);
    send(A::Solo, 0, 1); level(.1);
    send(A::Gain, 0, -20 * std::log10(2.)); level(.05);
    send(A::Master, 0, -20 * std::log10(2.)); level(.025);
    send(A::Pause);
    until([&] { return !player.status().playing; }, "pause not applied"); level(0);
    const auto paused_at = player.status().frame;
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    require(player.status().frame == paused_at, "paused position moved");
    send(A::Seek, 0, 0, 96000);
    until([&] { return player.status().frame == 96000; }, "paused seek not applied");
    send(A::Play); level(.025);
    require(player.status().frame > 96000, "resume failed");
    send(A::Stop);
    until([&] { return !player.status().playing && player.status().frame == 0; }, "stop failed"); level(0);
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
    std::cout << "PASS: actual hardware callbacks, play/pause/seek/stop, gain, master, mute/solo, restart; speaker output silenced\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << "Output probe failed: " << e.what() << '\n'; return 1; }
}
