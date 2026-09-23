#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace daw {

struct AudioBuffer {
  std::uint32_t sample_rate = 48000;
  std::uint16_t channels = 1;
  std::vector<float> samples;  // Interleaved float; staging/mix buffers may exceed unity.

  [[nodiscard]] std::size_t frameCount() const;
};

bool writeWavPcm16(const AudioBuffer& buffer, const std::string& path, std::string* error = nullptr);

}  // namespace daw
