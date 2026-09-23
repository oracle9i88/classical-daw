#include "audio_unit_instrument.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
}
// Opt-in local probe. Reads a short piano MIDI and its matching saved Pianoteq
// state, never opens an audio output device or writes plugin preferences.
int main(int argc, char** argv) {
  try {
    require(argc == 3, "usage: daw_au_chunk_probe PIANO.mid PIANO.aupreset");
    daw::MidiFile midi; std::string error;
    if (!daw::readMidiFile(argv[1], &midi, &error)) throw std::runtime_error(error);
    const auto size = std::filesystem::file_size(argv[2]);
    require(size > 0 && size <= 16U*1024U*1024U, "state size out of bounds");
    std::ifstream input(argv[2], std::ios::binary);
    const std::vector<std::uint8_t> state{std::istreambuf_iterator<char>(input), {}};
    require(state.size() == size, "state read failed");
    {
      daw::AudioUnitInstrument instrument(daw::InstrumentKind::Pianoteq9);
      instrument.restoreState(state);
      bool rejected = false;
      try { instrument.renderChunks(midi, {}); } catch (const std::invalid_argument&) { rejected = true; }
      require(rejected, "empty sink accepted");
      daw::InstrumentRenderReport report; report.peak = -1; report.sent_messages = 12345;
      unsigned calls = 0;
      rejected = false;
      try {
        instrument.renderChunks(midi, [&](std::size_t offset, const float*, std::uint32_t count) {
          require(offset == calls*256 && count == 256, "chunk clock discontinuity");
          if (++calls == 2) throw std::runtime_error("intentional sink failure");
        }, &report);
      } catch (const std::runtime_error& e) { rejected = std::string(e.what()) == "intentional sink failure"; }
      require(rejected && calls == 2 && report.peak == -1 && report.sent_messages == 12345,
              "sink failure did not abort without publishing report");
      rejected = false;
      try { instrument.renderChunks(midi, [](auto, auto, auto) {}); } catch (const std::exception&) { rejected = true; }
      require(rejected, "aborted AU was reused");
    }
    daw::AudioBuffer reference; daw::InstrumentRenderReport before, after;
    {
      daw::AudioUnitInstrument instrument(daw::InstrumentKind::Pianoteq9);
      instrument.restoreState(state); reference = instrument.render(midi, &before, 48000, 5, false);
    }
    double repeat_delta = 0;
    {
      daw::AudioUnitInstrument instrument(daw::InstrumentKind::Pianoteq9);
      instrument.restoreState(state);
      const auto repeated = instrument.render(midi, nullptr, 48000, 5, false);
      require(repeated.samples.size() == reference.samples.size(), "buffered repeat length changed");
      for (std::size_t i = 0; i < repeated.samples.size(); ++i)
        repeat_delta = std::max(repeat_delta, std::abs(static_cast<double>(repeated.samples[i])-reference.samples[i]));
    }
    std::size_t received = 0;
    double peak = 0, energy = 0, tail_energy = 0, chunk_delta = 0;
    std::uint64_t over_unity = 0;
    const auto tail_frames = std::min<std::size_t>(48000, reference.frameCount());
    {
      daw::AudioUnitInstrument instrument(daw::InstrumentKind::Pianoteq9);
      instrument.restoreState(state);
      instrument.renderChunks(midi, [&](std::size_t offset, const float* samples, std::uint32_t count) {
        require(offset == received && count && count <= 256 && count <= reference.frameCount()-received,
                "chunk timing/length mismatch");
        for (std::size_t i = 0; i < count*2; ++i) {
          const double value = samples[i];
          require(std::isfinite(value), "non-finite AU sample");
          peak = std::max(peak, std::abs(value)); energy += value*value;
          if (std::abs(value) > 1) ++over_unity;
          if (offset+i/2 >= reference.frameCount()-tail_frames) tail_energy += value*value;
          chunk_delta = std::max(chunk_delta, std::abs(value-reference.samples[offset*2+i]));
        }
        received += count;
      }, &after);
    }
    require(received == reference.frameCount() && after.peak > 0 && peak == after.peak &&
            std::abs(std::sqrt(energy/static_cast<double>(received*2))-after.rms) < 1e-12 &&
            std::abs(std::sqrt(tail_energy/static_cast<double>(tail_frames*2))-after.last_second_rms) < 1e-12 &&
            before.sent_messages == after.sent_messages && over_unity == after.over_unity_samples,
            "chunk diagnostics or final frame count differs");
    std::cout << "PASS Pianoteq continuous chunk clock/meter validation: frames=" << received
              << " messages=" << after.sent_messages << "; empty/throwing sink, report atomicity and consumed-instance checks passed\n";
    std::cout << "Separate AU instances (repeatability observation, not an equality gate): buffered_repeat_max_delta="
              << repeat_delta << " buffered_vs_chunk_max_delta=" << chunk_delta << '\n';
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
