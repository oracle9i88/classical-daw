#pragma once
#include "daw/audio_output_source.hpp"
#include "daw/realtime.hpp"
#include "daw/session.hpp"
#include <array>

namespace daw {
enum class PlaybackAction { Play, Pause, Stop, Seek, Gain, Balance, Mute, Solo, Master };
struct PlaybackCommand {
  PlaybackAction action = PlaybackAction::Pause;
  std::size_t track = 0;
  double value = 0; // dB, balance, or exactly 0/1 for mute/solo
  std::uint64_t frame = 0;
};
struct PlaybackStatus {
  std::uint64_t frame = 0, clipped_samples = 0, rejected_commands = 0;
  bool playing = false;
  double block_peak = 0; // Pre safety-clamp, linear, last callback only
};

// Immutable aligned pre-fader buffers, bounded to 512 MiB total / 64 tracks.
// One command producer, one render consumer. Construct/destruct only with audio
// stopped. Status fields are independent atomic observations, not a transaction.
class SessionPlayer final : public AudioOutputSource {
 public:
  static constexpr std::size_t kCapacity = 64;
  static constexpr std::uint32_t kRampFrames = 240; // 5 ms at 48 kHz
  static constexpr std::size_t kMaxAudioBytes = 512U * 1024U * 1024U;
  SessionPlayer(const Session& session, std::vector<AudioBuffer> audio);
  bool acceptsFormat(double rate, std::uint32_t channels) const noexcept override;
  bool enqueue(const PlaybackCommand& command) noexcept;
  void render(float* stereo, std::uint32_t frames) noexcept override;
  PlaybackStatus status() const noexcept;
  std::size_t frameCount() const noexcept { return frames_; }
  std::size_t trackCount() const noexcept { return audio_.size(); }

 private:
  struct Ramp {
    double current = 0, target = 0;
    std::uint32_t remaining = 0;
    void set(double value) noexcept;
    double next() noexcept;
  };
  struct Track {
    double gain = 0, balance = 0;
    bool mute = false, solo = false;
    Ramp left, right;
  };
  void targets(bool immediate = false) noexcept;
  void apply(const PlaybackCommand& command) noexcept;
  void transition() noexcept;
  std::vector<AudioBuffer> audio_;
  std::array<Track, kCapacity> tracks_{};
  SpscRing<PlaybackCommand, kCapacity> commands_;
  Ramp master_;
  std::size_t frames_ = 0, position_ = 0;
  bool playing_ = false;
  std::uint32_t transition_left_ = 0;
  std::array<double, 2> transition_from_{}, last_{};
  std::uint64_t clipped_ = 0;
  std::atomic<std::uint64_t> visible_position_{0}, visible_clipped_{0}, rejected_{0};
  std::atomic<bool> visible_playing_{false};
  std::atomic<double> visible_peak_{0};
};
}
