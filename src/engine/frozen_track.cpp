#include "daw/frozen_track.hpp"
#include "daw/audio_limits.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace daw {
namespace {
constexpr std::size_t max_frames = kMaxBufferedAudioFrames;
static_assert(kMaxStreamAudioFrames <= std::numeric_limits<std::uint32_t>::max(), "frozen frame count must fit u32");
static_assert(sizeof(std::streamoff) >= 8, "long frozen audio requires 64-bit offsets");
constexpr std::size_t max_identity = 81U * 1024U * 1024U;
void require(bool ok, const char* error) { if (!ok) throw std::runtime_error(error); }
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559, "freeze requires IEEE float32");
struct Crc {
  std::uint32_t value = 0xffffffffU;
  void update(const char* bytes, std::size_t count) {
    static const auto table = [] {
      std::array<std::uint32_t, 256> result{};
      for (std::uint32_t i = 0; i < 256; ++i) {
        auto value = i;
        for (int bit = 0; bit < 8; ++bit) value = (value >> 1) ^ ((value & 1) ? 0xedb88320U : 0U);
        result[i] = value;
      }
      return result;
    }();
    for (std::size_t i = 0; i < count; ++i) value = table[(value ^ static_cast<unsigned char>(bytes[i])) & 255U] ^ (value >> 8);
  }
};
std::array<char, 4> encoded(std::uint32_t value) {
  return {static_cast<char>(value & 255U), static_cast<char>((value >> 8) & 255U),
          static_cast<char>((value >> 16) & 255U), static_cast<char>((value >> 24) & 255U)};
}
std::uint32_t decoded(const char* p) {
  std::uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(static_cast<unsigned char>(p[i])) << (8 * i);
  return value;
}
struct Writer {
  std::ofstream stream;
  Crc crc;
  explicit Writer(const std::string& path) : stream(path, std::ios::binary) { require(bool(stream), "cannot create frozen audio"); }
  void bytes(const char* p, std::size_t n) {
    stream.write(p, static_cast<std::streamsize>(n)); crc.update(p, n);
    require(bool(stream), "frozen audio write failed");
  }
  void u32(std::uint32_t value) { const auto b = encoded(value); bytes(b.data(), b.size()); }
  void text(const std::string& s) { u32(static_cast<std::uint32_t>(s.size())); bytes(s.data(), s.size()); }
};
struct Reader {
  std::ifstream stream;
  Crc crc;
  explicit Reader(const std::string& path) : stream(path, std::ios::binary) { require(bool(stream), "cannot read frozen audio"); }
  void bytes(char* p, std::size_t n) {
    stream.read(p, static_cast<std::streamsize>(n));
    require(bool(stream), "truncated frozen audio"); crc.update(p, n);
  }
  std::uint32_t u32() { std::array<char, 4> b{}; bytes(b.data(), b.size()); return decoded(b.data()); }
  std::string text(std::size_t limit) {
    const auto n = u32(); require(n <= limit, "frozen metadata exceeds limit");
    std::string result(n, '\0'); bytes(result.data(), n); return result;
  }
};
}

std::string frozenTrackIdentity(const std::string& score, const std::string& part,
                               const std::string& instrument, const std::vector<std::uint8_t>& state) {
  require(!score.empty() && score.size() <= 64U * 1024U * 1024U && !part.empty() && part.size() <= 1024 &&
      !instrument.empty() && instrument.size() <= 1024 && !state.empty() && state.size() <= 16U * 1024U * 1024U,
      "invalid frozen source identity");
  std::string result;
  auto append = [&](const char* p, std::size_t n) {
    const auto prefix = encoded(static_cast<std::uint32_t>(n));
    result.append(prefix.data(), prefix.size()); result.append(p, n);
  };
  append(score.data(), score.size()); append(part.data(), part.size()); append(instrument.data(), instrument.size());
  append(reinterpret_cast<const char*>(state.data()), state.size());
  return result;
}

void writeFrozenTrack(const AudioBuffer& audio, const std::string& identity, const std::string& preset,
                      std::uint32_t version, const std::string& path) {
  require(audio.sample_rate == 48000 && audio.channels == 2 && audio.samples.size() % 2 == 0 &&
      audio.frameCount() > 0 && audio.frameCount() <= max_frames, "invalid frozen audio format/size");
  require(!identity.empty() && identity.size() <= max_identity && preset.size() <= 4096, "invalid frozen metadata");
  for (float sample : audio.samples) require(std::isfinite(sample), "non-finite frozen audio");
  require(!std::filesystem::exists(std::filesystem::symlink_status(path)), "frozen output already exists");
  Writer out(path);
  out.bytes("DAWFRZ01", 8); out.text(identity); out.text(preset); out.u32(version);
  out.u32(48000); out.u32(2); out.u32(static_cast<std::uint32_t>(audio.frameCount()));
  std::array<char, 65536> block{};
  for (std::size_t start = 0; start < audio.samples.size(); start += block.size() / 4) {
    const auto count = std::min(block.size() / 4, audio.samples.size() - start);
    for (std::size_t i = 0; i < count; ++i) {
      std::uint32_t bits; std::memcpy(&bits, &audio.samples[start + i], 4);
      const auto b = encoded(bits); std::copy(b.begin(), b.end(), block.begin() + static_cast<std::ptrdiff_t>(4 * i));
    }
    out.bytes(block.data(), count * 4);
  }
  const auto checksum = encoded(out.crc.value ^ 0xffffffffU);
  out.stream.write(checksum.data(), 4); out.stream.close();
  require(bool(out.stream), "frozen audio final write failed");
}

FrozenTrack readFrozenTrack(const std::string& path, const std::string& identity, std::size_t frames) {
  require(!identity.empty() && identity.size() <= max_identity && frames > 0 && frames <= max_frames,
          "invalid expected frozen identity/frames");
  require(std::filesystem::is_regular_file(std::filesystem::symlink_status(path)), "frozen audio must be a regular file, not a symlink");
  const auto size = std::filesystem::file_size(path);
  require(size <= max_identity + max_frames * 8ULL + 8192, "frozen audio exceeds file size limit");
  Reader in(path);
  std::array<char, 8> magic{}; in.bytes(magic.data(), magic.size());
  require(std::string(magic.data(), magic.size()) == "DAWFRZ01", "unknown frozen audio format");
  require(in.text(max_identity) == identity, "frozen audio is stale: score, part, instrument or state changed; render again");
  FrozenTrack result;
  result.preset = in.text(4096); result.component_version = in.u32();
  const auto rate = in.u32(), channels = in.u32(), stored_frames = in.u32();
  require(rate == 48000 && channels == 2 && stored_frames == frames, "frozen audio timing/format mismatch");
  const auto header_bytes = static_cast<std::uintmax_t>(in.stream.tellg());
  require(size == header_bytes + frames * 8ULL + 4, "frozen audio has truncated or trailing bytes");
  result.audio.sample_rate = rate; result.audio.channels = 2; result.audio.samples.resize(frames * 2);
  std::array<char, 65536> block{};
  for (std::size_t start = 0; start < result.audio.samples.size(); start += block.size() / 4) {
    const auto count = std::min(block.size() / 4, result.audio.samples.size() - start);
    in.bytes(block.data(), count * 4);
    for (std::size_t i = 0; i < count; ++i) {
      const auto bits = decoded(block.data() + i * 4);
      float value; std::memcpy(&value, &bits, 4);
      require(std::isfinite(value), "non-finite frozen audio"); result.audio.samples[start + i] = value;
    }
  }
  std::array<char, 4> checksum{}; in.stream.read(checksum.data(), 4);
  require(bool(in.stream) && decoded(checksum.data()) == (in.crc.value ^ 0xffffffffU), "frozen audio checksum mismatch");
  return result;
}

struct FrozenTrackReader::Impl {
  Reader reader;
  std::size_t frames = 0;
  std::string preset;
  std::uint32_t version = 0;
  std::streamoff offset = 0;
  Impl(const std::string& path, const std::string& identity, std::size_t expected) : reader(path) {
    frames = expected;
    std::array<char, 65536> block{};
    reader.bytes(block.data(), 8);
    require(std::string(block.data(), 8) == "DAWFRZ01", "unknown frozen audio format");
    const auto length = reader.u32();
    require(length == identity.size(), "frozen audio source identity mismatch");
    // Compare identity incrementally, never allocate a second score/state copy.
    for (std::size_t at = 0; at < length;) {
      const auto count = std::min(block.size(), length - at);
      reader.bytes(block.data(), count);
      require(std::memcmp(block.data(), identity.data() + at, count) == 0, "frozen audio source identity mismatch");
      at += count;
    }
    preset = reader.text(4096); version = reader.u32();
    const auto rate = reader.u32(), channels = reader.u32(), stored = reader.u32();
    require(rate == 48000 && channels == 2 && stored == frames, "frozen audio timing/format mismatch");
    offset = reader.stream.tellg();
    reader.stream.seekg(0, std::ios::end);
    require(reader.stream.tellg() == offset + static_cast<std::streamoff>(frames * 8 + 4),
            "frozen audio has truncated or trailing bytes");
    reader.stream.seekg(offset);
    for (std::size_t at = 0; at < frames;) {
      const auto count = std::min(kReadFrames, frames - at);
      reader.bytes(block.data(), count * 8);
      for (std::size_t i = 0; i < count * 2; ++i) {
        const auto bits = decoded(block.data() + i * 4);
        float value; std::memcpy(&value, &bits, 4);
        require(std::isfinite(value), "non-finite frozen audio");
      }
      at += count;
    }
    reader.stream.read(block.data(), 4);
    require(bool(reader.stream) && decoded(block.data()) == (reader.crc.value ^ 0xffffffffU), "frozen audio checksum mismatch");
  }
};
FrozenTrackReader::FrozenTrackReader(const std::string& path, const std::string& identity, std::size_t frames) {
  require(!identity.empty() && identity.size() <= max_identity && frames > 0 && frames <= kMaxStreamAudioFrames,
          "invalid expected frozen identity/frames");
  require(std::filesystem::is_regular_file(std::filesystem::symlink_status(path)), "frozen audio must be a regular file, not a symlink");
  require(std::filesystem::file_size(path) <= max_identity + kMaxStreamAudioFrames * 8ULL + 8192, "frozen audio exceeds file size limit");
  impl_ = std::make_unique<Impl>(path, identity, frames);
}
FrozenTrackReader::~FrozenTrackReader() = default;
std::size_t FrozenTrackReader::frameCount() const noexcept { return impl_->frames; }
const std::string& FrozenTrackReader::presetName() const noexcept { return impl_->preset; }
std::uint32_t FrozenTrackReader::componentVersion() const noexcept { return impl_->version; }
void FrozenTrackReader::readFrames(std::size_t start, std::size_t count, float* stereo) {
  require(start <= impl_->frames && count <= impl_->frames - start && count <= kReadFrames && (!count || stereo),
          "frozen block read out of bounds");
  if (!count) return;
  auto& in = impl_->reader.stream;
  in.clear(); in.seekg(impl_->offset + static_cast<std::streamoff>(start * 8));
  std::array<char, 65536> bytes{};
  in.read(bytes.data(), static_cast<std::streamsize>(count * 8));
  require(bool(in), "frozen block read failed");
  for (std::size_t i = 0; i < count * 2; ++i) {
    const auto bits = decoded(bytes.data() + i * 4);
    float sample; std::memcpy(&sample, &bits, 4);
    require(std::isfinite(sample), "non-finite frozen audio after validation");
    stereo[i] = sample;
  }
}

struct FrozenTrackWriter::Impl {
  struct Staging {
    std::filesystem::path path;
    explicit Staging(const std::filesystem::path& output) : path(output.string() + ".writing") {
      require(std::filesystem::create_directory(path), "frozen staging already exists");
    }
    ~Staging() {
      std::error_code ec;
      std::filesystem::remove(path / "audio.tmp", ec);
      std::filesystem::remove(path, ec);
    }
  };
  std::filesystem::path output;
  Staging staging;
  Writer writer;
  std::size_t frames = 0, written = 0;
  bool failed = false, finished = false;
  Impl(const std::string& path, const std::string& identity, const std::string& preset,
       std::uint32_t version, std::size_t total)
      : output(path), staging(output), writer((staging.path / "audio.tmp").string()), frames(total) {
    writer.bytes("DAWFRZ01", 8); writer.text(identity); writer.text(preset); writer.u32(version);
    writer.u32(48000); writer.u32(2); writer.u32(static_cast<std::uint32_t>(frames));
  }
};
FrozenTrackWriter::FrozenTrackWriter(const std::string& path, const std::string& identity,
                                   const std::string& preset, std::uint32_t version, std::size_t frames) {
  require(!identity.empty() && identity.size() <= max_identity && preset.size() <= 4096 &&
          frames > 0 && frames <= kMaxStreamAudioFrames, "invalid frozen stream metadata/frames");
  require(!path.empty() && !std::filesystem::exists(std::filesystem::symlink_status(path)), "frozen output already exists or is empty");
  impl_ = std::make_unique<Impl>(path, identity, preset, version, frames);
}
FrozenTrackWriter::~FrozenTrackWriter() = default;
std::size_t FrozenTrackWriter::writtenFrames() const noexcept { return impl_->written; }
void FrozenTrackWriter::appendFrames(const float* stereo, std::size_t count) {
  auto& w = *impl_;
  require(!w.finished && !w.failed, "frozen writer is closed or failed");
  require(count <= FrozenTrackReader::kReadFrames && count <= w.frames - w.written && (!count || stereo),
          "frozen append out of bounds");
  std::array<char, 65536> bytes{};
  for (std::size_t i = 0; i < count * 2; ++i) {
    require(std::isfinite(stereo[i]), "non-finite frozen audio");
    std::uint32_t bits; std::memcpy(&bits, stereo + i, 4);
    const auto b = encoded(bits); std::copy(b.begin(), b.end(), bytes.begin() + static_cast<std::ptrdiff_t>(i * 4));
  }
  // Bounds/finite-value validation precedes I/O; only I/O failures poison state.
  w.failed = true;
  w.writer.bytes(bytes.data(), count * 8);
  w.written += count; w.failed = false;
}
void FrozenTrackWriter::finish() {
  auto& w = *impl_;
  require(!w.finished && !w.failed, "frozen writer is closed or failed");
  require(w.written == w.frames, "frozen writer frame count incomplete");
  w.failed = true;
  const auto checksum = encoded(w.writer.crc.value ^ 0xffffffffU);
  w.writer.stream.write(checksum.data(), 4); w.writer.stream.close();
  require(bool(w.writer.stream), "frozen stream final write failed");
  std::filesystem::create_hard_link(w.staging.path / "audio.tmp", w.output);
  w.finished = true; w.failed = false;
}
}  // namespace daw
