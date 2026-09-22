#pragma once

#include "daw/realtime.hpp"

#include <AudioToolbox/AudioToolbox.h>

#include <atomic>
#include <cstdint>
#include <string>

namespace daw {

struct CoreAudioOutputConfig {
  double sample_rate = 48000.0;
  std::uint32_t block_size = 256;
  std::uint32_t channels = 2;
};

// Minimal macOS host for the platform-neutral BlockScheduler. The current
// callback emits silence by design; instrument/mixer rendering will be plugged
// in after the device lifecycle is stable.
class CoreAudioOutput {
 public:
  explicit CoreAudioOutput(CoreAudioOutputConfig config = {});
  ~CoreAudioOutput();

  CoreAudioOutput(const CoreAudioOutput&) = delete;
  CoreAudioOutput& operator=(const CoreAudioOutput&) = delete;

  bool start(std::string* error = nullptr);
  void stop() noexcept;
  [[nodiscard]] bool running() const noexcept { return running_.load(std::memory_order_acquire); }
  [[nodiscard]] std::uint64_t xrunCount() const noexcept { return scheduler_.snapshot().xrun_count; }
  [[nodiscard]] BlockScheduler& scheduler() noexcept { return scheduler_; }

 private:
  static OSStatus renderCallback(void* reference,
                                 AudioUnitRenderActionFlags* action_flags,
                                 const AudioTimeStamp* timestamp,
                                 UInt32 bus_number,
                                 UInt32 frame_count,
                                 AudioBufferList* buffers) noexcept;

  CoreAudioOutputConfig config_;
  AudioUnit audio_unit_ = nullptr;
  BlockScheduler scheduler_;
  std::atomic<bool> running_{false};
};

}  // namespace daw
