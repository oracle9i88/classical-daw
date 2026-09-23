#pragma once
#include <cstdint>

namespace daw {
// Prepared on the control thread, borrowed until output has stopped. render()
// runs on the audio thread: no allocation, locks, file I/O or plugin setup.
class AudioOutputSource {
 public:
  virtual ~AudioOutputSource() = default;
  virtual bool acceptsFormat(double rate, std::uint32_t channels) const noexcept = 0;
  virtual void render(float* interleaved, std::uint32_t frames) noexcept = 0;
};
}
