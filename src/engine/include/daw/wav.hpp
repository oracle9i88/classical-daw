#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace daw {

struct AudioBuffer {
  std::uint32_t sample_rate = 48000;
  std::uint16_t channels = 1;
  std::vector<float> samples;  // interleaved, normalized to [-1, 1]

  [[nodiscard]] std::size_t frameCount() const;
};

bool writeWavPcm16(const AudioBuffer& buffer, const std::string& path, std::string* error = nullptr);

}  // namespace daw
