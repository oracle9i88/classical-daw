// Bounce the performance, not the score. Every onset, length, velocity and
// controller lane a performance carries exists only after that layer is
// applied, so this renders the compiled sequence rather than re-deriving
// events from notation. Offline: no output device is opened.
#include "audio_unit_instrument.hpp"
#include "daw/performance.hpp"
#include "daw/wav.hpp"
#include "daw/session.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>

namespace {
std::string jsonString(const std::string& value) {
  std::ostringstream out;out<<'"';
  for(unsigned char c:value) {
    if(c=='"'||c=='\\')out<<'\\'<<static_cast<char>(c);
    else if(c<32)out<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<static_cast<unsigned>(c);
    else out<<static_cast<char>(c);
  }
  out<<'"';return out.str();
}
}


int main(int argc, char** argv) {
  std::string stage = "arguments";
  try {
    std::size_t take = std::numeric_limits<std::size_t>::max();
    if (argc == 5 && std::string(argv[3]) == "--take") take = std::stoul(argv[4]);
    else if (argc != 3) throw std::runtime_error("usage: daw_performance_render DOCUMENT NEW_DIRECTORY [--take INDEX]");
    const std::filesystem::path root(argv[2]);
    if (std::filesystem::exists(root)) throw std::runtime_error("output directory already exists");

    stage = "load";
    auto document = daw::loadPerformanceDocument(argv[1]);
    if (take != std::numeric_limits<std::size_t>::max()) {
      if (take >= document.performances.size()) throw std::runtime_error("no such performance index");
      document.active = take;
    }
    const auto& performance = document.performances.at(document.active);

    stage = "compile";
    const auto sequence = daw::compilePerformance(document.score, performance);

    stage = "instrument";
    daw::AudioUnitInstrument piano(daw::InstrumentKind::Pianoteq9);
    piano.restoreState(document.piano_state);

    stage = "render";
    daw::InstrumentRenderReport report;
    auto audio = piano.renderPerformance(sequence, &report, false);
    // Preserve AU float headroom until the document gain is applied. Clipping
    // inside the plugin wrapper before attenuation would permanently distort it.
    const auto output_report = daw::applyMasterMix(audio, document.gain_db);

    stage = "write";
    std::filesystem::create_directories(root);
    std::string message;
    if (!daw::writeWavPcm16(audio, (root / "performance.wav").string(), &message)) {
      throw std::runtime_error("audio write failed: " + message);
    }
    std::ofstream json(root / "report.json");
    json << std::setprecision(17)
         << "{\n  \"format\": \"classical-daw-performance-bounce-1\",\n"
         << "  \"performance\": " << jsonString(performance.name) << ",\n"
         << "  \"take_index\": " << document.active << ",\n"
         << "  \"performed_notes\": " << performance.mapping.size() << ",\n"
         << "  \"performance_overrides\": " << performance.notes.size() << ",\n"
         << "  \"control_curves\": " << performance.curves.size() << ",\n"
         << "  \"output_gain_db\": " << document.gain_db << ",\n"
         << "  \"sent_messages\": " << report.sent_messages << ",\n"
         << "  \"frames\": " << audio.samples.size() / 2 << ",\n"
         << "  \"seconds\": " << static_cast<double>(audio.samples.size() / 2) / 48000 << ",\n"
         << "  \"peak\": " << output_report.peak << ",\n"
         << "  \"rms\": " << output_report.rms << ",\n"
         << "  \"clipped_samples\": " << output_report.over_unity_samples << ",\n"
         << "  \"output_devices_opened\": 0\n}\n";
    json.close();
    if (!json) throw std::runtime_error("report write failed");

    std::cout << "Bounced " << std::quoted(performance.name) << ": "
              << performance.notes.size() << " performance overrides and "
              << performance.curves.size() << " control curves are in this audio, "
              << report.sent_messages << " messages, gain " << document.gain_db << " dB, peak " << output_report.peak
              << ", clipped " << output_report.over_unity_samples << '\n';
    std::cout << "Wrote " << (root / "performance.wav").string() << '\n';
  } catch (const std::exception& e) {
    std::cerr << "FAIL stage=" << stage << " reason=" << e.what() << '\n';
    return 1;
  }
}
