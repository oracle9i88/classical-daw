#pragma once

#include "daw/realtime.hpp"

#include <AudioToolbox/AudioToolbox.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace daw {

struct CoreAudioOutputConfig {
  double sample_rate = 48000.0;
  std::uint32_t block_size = 256;
  std::uint32_t channels = 2;
  // Zero uses the system default output device. A non-zero value is an
  // AudioDeviceID returned by enumerateOutputDevices().
  std::uint32_t device_id = 0;
};

struct CoreAudioOutputDeviceInfo {
  std::uint32_t id = 0;
  std::string name;
  bool is_default = false;
};

// Minimal macOS host for the platform-neutral BlockScheduler. The callback
// uses a prepared diagnostic renderer and clears/counts malformed output
// buffers. Device enumeration and explicit selection stay on the control
// thread; the realtime callback never touches CoreAudio object properties.
class CoreAudioOutput {
 public:
  explicit CoreAudioOutput(CoreAudioOutputConfig config = {});
  ~CoreAudioOutput();

  CoreAudioOutput(const CoreAudioOutput&) = delete;
  CoreAudioOutput& operator=(const CoreAudioOutput&) = delete;

  bool start(std::string* error = nullptr);
  void stop() noexcept;
  [[nodiscard]] std::vector<CoreAudioOutputDeviceInfo> enumerateOutputDevices(
      std::string* error = nullptr) const;
  // Select a device for the next start. Changing a live device is rejected so
  // callers cannot race an active AudioUnit from the control thread.
  bool setOutputDevice(std::uint32_t device_id, std::string* error = nullptr);
  [[nodiscard]] std::uint32_t currentDeviceId() const noexcept { return current_device_id_; }
  [[nodiscard]] bool running() const noexcept { return running_.load(std::memory_order_acquire); }
  [[nodiscard]] std::uint64_t xrunCount() const noexcept { return scheduler_.snapshot().xrun_count; }
  [[nodiscard]] BlockScheduler& scheduler() noexcept { return scheduler_; }
  [[nodiscard]] SineVoiceBank& synth() noexcept { return synth_; }

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
  SineVoiceBank synth_;
  std::atomic<bool> running_{false};
  std::uint32_t current_device_id_ = 0;
};

}  // namespace daw
