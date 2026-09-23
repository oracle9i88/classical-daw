#pragma once

#include "daw/realtime.hpp"
#include "daw/audio_output_source.hpp"
#include "daw/output_health.hpp"
#include "daw/callback_timing.hpp"

#include <AudioToolbox/AudioToolbox.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace daw {

struct CoreAudioOutputConfig {
  double sample_rate = 48000.0;
  // Engine quantum, not a request to change the hardware's buffer size.
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

// macOS host for prepared audio sources or the diagnostic BlockScheduler. The
// callback splits hardware requests into engine quanta and clears/counts malformed output
// buffers. Device enumeration and explicit selection stay on the control
// thread; the realtime callback never touches CoreAudio object properties.
class CoreAudioOutput {
 public:
  explicit CoreAudioOutput(CoreAudioOutputConfig config = {});
  ~CoreAudioOutput();

  CoreAudioOutput(const CoreAudioOutput&) = delete;
  CoreAudioOutput& operator=(const CoreAudioOutput&) = delete;

  // Waits up to 2 seconds for a successfully rendered callback before reporting
  // readiness. Prepare/attach a paused source to avoid sound during startup.
  bool start(std::string* error = nullptr);
  void stop() noexcept;
  [[nodiscard]] std::vector<CoreAudioOutputDeviceInfo> enumerateOutputDevices(
      std::string* error = nullptr) const;
  // Select a device for the next start. Changing a live device is rejected so
  // callers cannot race an active AudioUnit from the control thread.
  bool setOutputDevice(std::uint32_t device_id, std::string* error = nullptr);
  // Control thread only, with output stopped. nullptr restores the diagnostic
  // synth path. The source must outlive this output's active callback.
  bool setAudioSource(AudioOutputSource* source, std::string* error = nullptr);
  // Control-thread polling, recommended every 100 ms, including while paused.
  // Latched faults require stop, source suspension, then explicit restart.
  // Does not stop/restart the AU or alter the source on the caller's behalf.
  bool checkHealth(std::string* error = nullptr);
  // Control-thread diagnostic only; reads AU/device properties, never called
  // by renderCallback. Does not change the system sample rate or buffer size.
  std::string diagnostics() const;
  // Control thread only, after stop() has returned. Throws if an AU still
  // exists, so no caller may race the callback's plain statistics fields.
  // Covers the complete client render callback, excluding surrounding output
  // AU conversion/HAL/driver work. A new start attempt resets the statistics.
  [[nodiscard]] CallbackTimingStats callbackTimingAfterStop() const;
  [[nodiscard]] std::uint32_t currentDeviceId() const noexcept { return current_device_id_; }
  [[nodiscard]] bool running() const noexcept { return running_.load(std::memory_order_acquire); }
  [[nodiscard]] std::uint64_t xrunCount() const noexcept { return callback_errors_.load(std::memory_order_relaxed); }
  [[nodiscard]] std::uint64_t renderedFrames() const noexcept { return rendered_frames_.load(std::memory_order_relaxed); }
  [[nodiscard]] BlockScheduler& scheduler() noexcept { return scheduler_; }
  [[nodiscard]] SineVoiceBank& synth() noexcept { return synth_; }

 private:
  OutputHealthSnapshot healthSnapshot() const noexcept;
  OutputHealthMonitor health_;
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
  AudioOutputSource* source_ = nullptr;
  std::atomic<bool> running_{false};
  std::atomic<std::uint64_t> callback_errors_{0};
  std::atomic<std::uint64_t> rendered_frames_{0};
  CallbackTimingStats callback_timing_;
  std::uint32_t current_device_id_ = 0;
  std::uint32_t maximum_callback_frames_ = 0;
};

}  // namespace daw
