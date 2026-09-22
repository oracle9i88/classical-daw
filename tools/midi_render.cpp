#include "daw/midi.hpp"
#include "daw/render.hpp"
#include "daw/wav.hpp"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {

bool parseSampleRate(const std::string& text, std::uint32_t* rate) {
  std::uint32_t parsed = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
      parsed < 8000 || parsed > 192000) {
    return false;
  }
  *rate = parsed;
  return true;
}

std::string reportJson(const daw::AudioBuffer& audio, const daw::MidiImportReport& imported,
                       const daw::MidiRenderReport& rendered) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(17)
      << "{\n  \"renderer\": \"sine_diagnostic\","
      << "\n  \"sample_rate\": " << audio.sample_rate
      << ",\n  \"channels\": " << audio.channels
      << ",\n  \"frames\": " << audio.frameCount()
      << ",\n  \"duration_seconds\": "
      << static_cast<double>(audio.frameCount()) / static_cast<double>(audio.sample_rate)
      << ",\n  \"import_diagnostics\": {"
      << "\n    \"source_ticks_per_quarter\": " << imported.source_ticks_per_quarter
      << ",\n    \"rounded_note_boundaries\": " << imported.rounded_note_boundaries
      << ",\n    \"rounded_tempo_events\": " << imported.rounded_tempo_events
      << ",\n    \"ignored_channel_events\": " << imported.ignored_channel_events
      << ",\n    \"preserved_channel_events\": " << imported.preserved_channel_events
      << ",\n    \"rounded_channel_events\": " << imported.rounded_channel_events
      << ",\n    \"ignored_meta_events\": " << imported.ignored_meta_events
      << ",\n    \"ignored_sysex_events\": " << imported.ignored_sysex_events
      << ",\n    \"ignored_time_signature_events\": " << imported.ignored_time_signature_events
      << ",\n    \"preserved_time_signature_events\": " << imported.preserved_time_signature_events
      << ",\n    \"rounded_time_signature_events\": " << imported.rounded_time_signature_events
      << ",\n    \"coalesced_time_signature_events\": " << imported.coalesced_time_signature_events
      << ",\n    \"overlapping_same_pitch_notes\": " << imported.overlapping_same_pitch_notes
      << "\n  },\n  \"render_diagnostics\": {"
      << "\n    \"interpreted_channel_events\": " << rendered.interpreted_channel_events
      << ",\n    \"unsupported_channel_events\": " << rendered.unsupported_channel_events
      << ",\n    \"voices_released_at_end\": " << rendered.voices_released_at_end
      << ",\n    \"ignored_release_velocities\": " << rendered.ignored_release_velocities
      << ",\n    \"clipped_samples\": " << rendered.clipped_samples
      << "\n  }\n}\n";
  return out.str();
}

// Only these files in a directory created by this invocation belong to us.
// A nonempty directory is deliberately never removed recursively on failure.
struct OutputGuard {
  explicit OutputGuard(const std::filesystem::path& path)
      : directory(path), wav(path / "diagnostic.wav"), report(path / "report.json") {}

  ~OutputGuard() {
    if (!created || complete) return;
    std::error_code ignored;
    if (wav_started) std::filesystem::remove(wav, ignored);
    if (report_started) std::filesystem::remove(report, ignored);
    std::filesystem::remove(directory, ignored);
  }

  std::filesystem::path directory;
  std::filesystem::path wav;
  std::filesystem::path report;
  bool created = false;
  bool wav_started = false;
  bool report_started = false;
  bool complete = false;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3 || argc > 4) {
    std::cerr << "Usage: daw_midi_render input.mid NEW_OUTPUT_DIRECTORY [sample_rate]\n"
              << "sample_rate must be an integer in 8000..192000 (default 48000).\n";
    return 2;
  }
  std::uint32_t sample_rate = 48000;
  if (argc == 4 && !parseSampleRate(argv[3], &sample_rate)) {
    std::cerr << "Sample rate must be an integer in 8000..192000.\n";
    return 2;
  }

  std::cerr << "Sine-only diagnostic renderer; not an orchestral sampler.\n";
  try {
    OutputGuard output{std::filesystem::path(argv[2])};
    std::error_code status_error;
    const auto status = std::filesystem::symlink_status(output.directory, status_error);
    if (status_error && status_error != std::errc::no_such_file_or_directory) {
      throw std::runtime_error("cannot inspect output directory: " + status_error.message());
    }
    if (std::filesystem::exists(status)) {
      throw std::runtime_error("output directory must not exist");
    }

    daw::MidiFile midi;
    daw::MidiImportReport imported;
    std::string error;
    if (!daw::readMidiFile(argv[1], &midi, &error, &imported)) {
      throw std::runtime_error("MIDI import failed: " + error);
    }
    daw::MidiRenderReport rendered;
    const daw::AudioBuffer audio = daw::renderMidiFile(midi, static_cast<double>(sample_rate), 0.1, &rendered);
    const std::string json = reportJson(audio, imported, rendered);
    std::cerr << "Interpreted channel events: " << rendered.interpreted_channel_events
              << "; unsupported channel events: " << rendered.unsupported_channel_events
              << "; ignored release velocities: " << rendered.ignored_release_velocities
              << "; clipped samples: " << rendered.clipped_samples
              << "; voices released at end: " << rendered.voices_released_at_end << '\n';

    // All parsing and rendering finish before the first filesystem mutation.
    // create_directory also catches an output path created after the check.
    std::error_code create_error;
    if (!std::filesystem::create_directory(output.directory, create_error)) {
      throw std::runtime_error("output directory must not exist and must be creatable" +
                               (create_error ? ": " + create_error.message() : std::string{}));
    }
    output.created = true;
    output.wav_started = true;
    if (!daw::writeWavPcm16(audio, output.wav.string(), &error)) {
      throw std::runtime_error("WAV write failed: " + error);
    }
    output.report_started = true;
    std::ofstream report(output.report, std::ios::binary | std::ios::trunc);
    if (!report) throw std::runtime_error("cannot open output report");
    report << json;
    report.flush();
    if (!report) throw std::runtime_error("failed while writing output report");
    report.close();
    if (!report) throw std::runtime_error("failed while closing output report");
    output.complete = true;
    std::cout << "Wrote " << output.wav << " and " << output.report << '\n';
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "MIDI render failed: " << exception.what() << '\n';
    return 1;
  }
}
