#pragma once

#include <cstdint>
#include <string>
#include <memory>
#include <vector>

namespace daw {

struct AudioBuffer {
  std::uint32_t sample_rate = 48000;
  std::uint16_t channels = 1;
  std::vector<float> samples;  // Interleaved float; staging/mix buffers may exceed unity.

  [[nodiscard]] std::size_t frameCount() const;
};

bool writeWavPcm16(const AudioBuffer& buffer, const std::string& path, std::string* error = nullptr);

// Worker/control-thread incremental PCM16 WAV, fixed stereo 48 kHz, up to two
// hours including tail. No clipping: non-finite or out-of-range samples fail
// before writing the chunk. finish requires the exact frame count and publishes
// a new file atomically without overwrite. Chunks at most 8192 frames.
class WavPcm16Writer {
 public:
  WavPcm16Writer(const std::string& path, std::size_t frames);
  ~WavPcm16Writer();
  WavPcm16Writer(const WavPcm16Writer&) = delete;
  WavPcm16Writer& operator=(const WavPcm16Writer&) = delete;
  void appendFrames(const float* stereo, std::size_t count);
  void finish();
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace daw
