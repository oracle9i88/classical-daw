#include "daw/audio_limits.hpp"
#include "daw/wav.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template <class F> void rejects(F f) {
  bool failed = false; try { f(); } catch (const std::exception&) { failed = true; }
  require(failed, "invalid WAV operation accepted");
}
struct Temp {
  fs::path path = fs::temp_directory_path() / ("daw-wav-stream-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Temp() { require(fs::create_directory(path), "cannot reserve test directory"); }
  ~Temp() { std::error_code ec; fs::remove_all(path, ec); }
};
std::string read(const fs::path& path) { std::ifstream in(path, std::ios::binary); std::ostringstream out; out << in.rdbuf(); return out.str(); }
void write(const fs::path& path, const std::string& bytes) { std::ofstream out(path, std::ios::binary); out << bytes; }
std::uint32_t u32(const char* p) {
  std::uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(static_cast<unsigned char>(p[i])) << (8*i);
  return value;
}
void longFixture(const fs::path& path) {
  // Full two-hour PCM16 output, with only one chunk resident. Check RIFF length,
  // distant seeks, final partial chunk and exact PCM sample encoding on disk.
  constexpr auto frames = daw::kMaxStreamAudioFrames;
  std::array<float, 16384> block{};
  {
    daw::WavPcm16Writer writer(path.string(), frames);
    for (std::size_t start = 0; start < frames; start += 8192) {
      const auto count = std::min<std::size_t>(8192, frames - start);
      for (std::size_t i = 0; i < count; ++i) {
        block[i*2] = static_cast<float>(static_cast<int>((start+i)%997)-498)/1024.F;
        block[i*2+1] = -block[i*2];
      }
      writer.appendFrames(block.data(), count);
    }
    writer.finish();
  }
  require(fs::file_size(path) == 44 + frames*4, "long WAV size wrong");
  std::ifstream in(path, std::ios::binary); std::array<char, 44> header{};
  in.read(header.data(), header.size());
  require(u32(header.data()+4) == 36 + frames*4 && u32(header.data()+40) == frames*4, "long RIFF sizes wrong");
  for (const std::size_t frame : {std::size_t{0}, std::size_t{8191}, std::size_t{8192}, std::size_t{48000*60*90}, frames-1}) {
    std::array<char, 4> bytes{}; in.seekg(static_cast<std::streamoff>(44+frame*4)); in.read(bytes.data(), 4);
    const auto sample = static_cast<std::int16_t>(std::lrint(static_cast<float>(static_cast<int>(frame%997)-498)/1024.F*32767.F));
    require(in.good() && (u32(bytes.data()) & 65535) == static_cast<std::uint16_t>(sample) &&
            (u32(bytes.data()) >> 16) == static_cast<std::uint16_t>(-sample), "long PCM channel/frame mismatch");
  }
  std::cout << "PASS: 120-minute synthetic WAV, frames=" << frames << " bytes=" << fs::file_size(path) << " bounded chunk and distant PCM reads verified\n";
}
}
int main(int argc, char** argv) {
  try {
    Temp temp;
    if (argc == 2 && std::string(argv[1]) == "--long") { longFixture(temp.path/"long.wav"); return 0; }
    require(argc == 1, "usage: daw_wav_stream_writer_tests [--long]");
    const auto path = (temp.path/"stream.wav").string(), legacy = (temp.path/"legacy.wav").string();
    daw::AudioBuffer audio{48000, 2, {0.F, -0.F, 1.F, -1.F, .1234567F, -.1F}};
    for (int i = 0; i < 20000; ++i) audio.samples.push_back(static_cast<float>(i%129-64)/128.F);
    require(daw::writeWavPcm16(audio, legacy), "legacy fixture failed");
    {
      daw::WavPcm16Writer writer(path, audio.frameCount());
      require(!fs::exists(path), "partial output published");
      rejects([&] { daw::WavPcm16Writer other(path, 1); });
      rejects([&] { writer.finish(); });
      rejects([&] { writer.appendFrames(nullptr, 1); });
      rejects([&] { writer.appendFrames(audio.samples.data(), 8193); });
      std::array<float, 4> bad{.2F, .3F, .4F, 0};
      for (const float value : {1.01F, -1.01F, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        bad.back() = value; rejects([&] { writer.appendFrames(bad.data(), 2); });
      }
      writer.appendFrames(nullptr, 0);
      writer.appendFrames(audio.samples.data(), 8192);
      writer.appendFrames(audio.samples.data()+16384, audio.frameCount()-8192);
      rejects([&] { writer.appendFrames(audio.samples.data(), 1); });
      writer.finish(); rejects([&] { writer.finish(); }); rejects([&] { writer.appendFrames(nullptr, 0); });
    }
    require(read(path) == read(legacy), "streamed PCM differs from buffered encoding or invalid append changed data");
    require(!fs::exists(path+".writing"), "owned staging leaked");
    rejects([&] { daw::WavPcm16Writer writer(path, 1); });
    const auto partial = (temp.path/"partial.wav").string();
    { daw::WavPcm16Writer writer(partial, 3); writer.appendFrames(audio.samples.data(), 1); }
    require(!fs::exists(partial) && !fs::exists(partial+".writing"), "aborted output leaked");
    fs::create_directory(partial+".writing"); write(fs::path(partial+".writing")/"audio.tmp", "old crash");
    rejects([&] { daw::WavPcm16Writer writer(partial, 3); });
    require(read(fs::path(partial+".writing")/"audio.tmp") == "old crash", "stale staging modified");
    const auto competing = (temp.path/"competing.wav").string();
    {
      daw::WavPcm16Writer writer(competing, 3); writer.appendFrames(audio.samples.data(), 3);
      write(competing, "other owner"); rejects([&] { writer.finish(); });
      rejects([&] { writer.appendFrames(nullptr, 0); });
    }
    require(read(competing) == "other owner" && !fs::exists(competing+".writing"), "competing output not preserved");
    const auto link = temp.path/"symlink.wav"; fs::create_symlink(temp.path/"absent", link);
    rejects([&] { daw::WavPcm16Writer writer(link.string(), 3); });
    const auto invalid = (temp.path/"invalid.wav").string();
    rejects([&] { daw::WavPcm16Writer writer(invalid, 0); });
    rejects([&] { daw::WavPcm16Writer writer(invalid, daw::kMaxStreamAudioFrames+1); });
    rejects([&] { daw::WavPcm16Writer writer(invalid, std::numeric_limits<std::size_t>::max()); });
    require(!fs::exists(invalid+".writing"), "bad size created staging");
    std::cout << "WAV stream parity, finite/headroom checks, exact count, abort and publication protection passed\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
