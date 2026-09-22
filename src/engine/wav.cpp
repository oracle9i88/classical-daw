#include "daw/wav.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace daw {
namespace {

void putU16(std::ofstream& out, std::uint16_t value) {
  const char bytes[2] = {static_cast<char>(value & 0xff), static_cast<char>((value >> 8) & 0xff)};
  out.write(bytes, 2);
}

void putU32(std::ofstream& out, std::uint32_t value) {
  const char bytes[4] = {static_cast<char>(value & 0xff), static_cast<char>((value >> 8) & 0xff),
                         static_cast<char>((value >> 16) & 0xff), static_cast<char>((value >> 24) & 0xff)};
  out.write(bytes, 4);
}

}  // namespace

std::size_t AudioBuffer::frameCount() const {
  return channels == 0 ? 0 : samples.size() / channels;
}

bool writeWavPcm16(const AudioBuffer& buffer, const std::string& path, std::string* error) {
  try {
    if (buffer.sample_rate == 0 || buffer.channels == 0) throw std::invalid_argument("WAV format has invalid rate or channel count");
    if (buffer.samples.size() % buffer.channels != 0) throw std::invalid_argument("WAV samples are not channel-aligned");
    const std::uint64_t data_bytes = static_cast<std::uint64_t>(buffer.samples.size()) * sizeof(std::int16_t);
    if (data_bytes > std::numeric_limits<std::uint32_t>::max() - 36) throw std::length_error("WAV file exceeds RIFF size limit");

    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("cannot open output WAV file");
    output.write("RIFF", 4);
    putU32(output, static_cast<std::uint32_t>(36 + data_bytes));
    output.write("WAVEfmt ", 8);
    putU32(output, 16);  // PCM fmt chunk size
    putU16(output, 1);   // PCM
    putU16(output, buffer.channels);
    putU32(output, buffer.sample_rate);
    const std::uint32_t byte_rate = buffer.sample_rate * buffer.channels * sizeof(std::int16_t);
    putU32(output, byte_rate);
    putU16(output, static_cast<std::uint16_t>(buffer.channels * sizeof(std::int16_t)));
    putU16(output, 16);
    output.write("data", 4);
    putU32(output, static_cast<std::uint32_t>(data_bytes));
    for (float sample : buffer.samples) {
      const float clamped = std::clamp(sample, -1.0f, 1.0f);
      const auto pcm = static_cast<std::int16_t>(std::lrint(clamped * 32767.0f));
      putU16(output, static_cast<std::uint16_t>(pcm));
    }
    if (!output) throw std::runtime_error("failed while writing WAV file");
    return true;
  } catch (const std::exception& exception) {
    if (error) *error = exception.what();
    return false;
  }
}

}  // namespace daw
