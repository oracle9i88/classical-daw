#include "daw/wav.hpp"
#include "daw/audio_limits.hpp"
#include <array>
#include <filesystem>

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


struct WavPcm16Writer::Impl {
  struct Staging {
    std::filesystem::path path;
    explicit Staging(const std::string& output) : path(output + ".writing") {
      if (!std::filesystem::create_directory(path)) throw std::runtime_error("WAV staging already exists");
    }
    ~Staging() { std::error_code ec; std::filesystem::remove(path / "audio.tmp", ec); std::filesystem::remove(path, ec); }
  };
  std::string path;
  Staging staging;
  std::ofstream out;
  std::size_t frames = 0, written = 0;
  bool failed = false, finished = false;
  Impl(const std::string& output, std::size_t total)
      : path(output), staging(output), out(staging.path / "audio.tmp", std::ios::binary), frames(total) {
    const auto bytes = static_cast<std::uint32_t>(frames * 4);
    out.write("RIFF", 4); putU32(out, 36 + bytes); out.write("WAVEfmt ", 8);
    putU32(out, 16); putU16(out, 1); putU16(out, 2); putU32(out, 48000);
    putU32(out, 48000 * 4); putU16(out, 4); putU16(out, 16);
    out.write("data", 4); putU32(out, bytes);
    if (!out) throw std::runtime_error("WAV header write failed");
  }
};
WavPcm16Writer::WavPcm16Writer(const std::string& path, std::size_t frames) {
  if (!frames || frames > kMaxStreamAudioFrames || frames > (std::numeric_limits<std::uint32_t>::max() - 36ULL) / 4)
    throw std::invalid_argument("streamed WAV frame count out of bounds");
  if (path.empty() || std::filesystem::exists(std::filesystem::symlink_status(path)))
    throw std::runtime_error("WAV output must be new");
  impl_ = std::make_unique<Impl>(path, frames);
}
WavPcm16Writer::~WavPcm16Writer() = default;
void WavPcm16Writer::appendFrames(const float* stereo, std::size_t count) {
  auto& w = *impl_;
  if (w.failed || w.finished) throw std::runtime_error("WAV writer is closed or failed");
  if (count > 8192 || count > w.frames - w.written || (count && !stereo)) throw std::invalid_argument("WAV append out of bounds");
  std::array<char, 32768> bytes{};
  for (std::size_t i = 0; i < count * 2; ++i) {
    const auto sample = stereo[i];
    if (!std::isfinite(sample) || sample < -1 || sample > 1) throw std::invalid_argument("streamed WAV requires finite samples within unity");
    const auto value = static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lrint(sample * 32767.F)));
    bytes[i * 2] = static_cast<char>(value & 255U); bytes[i * 2 + 1] = static_cast<char>(value >> 8);
  }
  w.failed = true;
  w.out.write(bytes.data(), static_cast<std::streamsize>(count * 4));
  if (!w.out) throw std::runtime_error("WAV chunk write failed");
  w.written += count; w.failed = false;
}
void WavPcm16Writer::finish() {
  auto& w = *impl_;
  if (w.failed || w.finished) throw std::runtime_error("WAV writer is closed or failed");
  if (w.written != w.frames) throw std::runtime_error("WAV frame count incomplete");
  w.failed = true; w.out.close();
  if (!w.out) throw std::runtime_error("WAV final write failed");
  std::filesystem::create_hard_link(w.staging.path / "audio.tmp", w.path);
  w.finished = true; w.failed = false;
}

}  // namespace daw
