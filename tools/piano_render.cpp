#include "pianoteq_au.hpp"
#include "daw/midi_sequence.hpp"
#include "daw/project.hpp"
#include "daw/score_midi.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;
namespace {
std::string jsonString(const std::string& value) {
  std::ostringstream result;
  result << '"';
  for (unsigned char byte : value) {
    if (byte == '"' || byte == '\\') result << '\\' << static_cast<char>(byte);
    else if (byte < 32) result << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(byte);
    else result << static_cast<char>(byte);
  }
  result << '"';
  return result.str();
}

std::vector<std::uint8_t> readState(const fs::path& path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  const auto size = input.tellg();
  if (!input || size <= 0 || size > 16 * 1024 * 1024) throw std::runtime_error("cannot read AU state (limit 16 MiB)");
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  input.seekg(0);
  input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!input) throw std::runtime_error("AU state read failed");
  return bytes;
}

void writeBytes(const fs::path& path, const void* bytes, std::size_t size) {
  std::ofstream output(path, std::ios::binary);
  if (!output) throw std::runtime_error("cannot create " + path.filename().string());
  output.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(size));
  output.close();
  if (!output) throw std::runtime_error("failed writing " + path.filename().string());
}

struct OutputGuard {
  fs::path directory;
  bool created = false, complete = false;
  ~OutputGuard() {
    if (!created || complete) return;
    std::error_code ignored;
    for (const auto* name : {"piano.wav", "piano.aupreset", "score.dawproj", "score.dawproj.tmp",
                             "performance.mid", "performance.mid.tmp", "report.json"}) {
      fs::remove(directory / name, ignored);
    }
    fs::remove(directory, ignored);  // Never recursively delete unexpected content.
  }
};
}

int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--list-presets") {
      daw::PianoteqAU piano;
      for (const auto& name : piano.factoryPresets()) std::cout << name << '\n';
      return 0;
    }
    if ((argc != 3 && argc != 5) ||
        (argc == 5 && std::string(argv[3]) != "--preset" && std::string(argv[3]) != "--state")) {
      std::cerr << "Usage: daw_piano_render INPUT.{mid,midi,dawproj,musicxml,xml} NEW_OUTPUT_DIRECTORY\n"
                   "                        [--preset FACTORY_NAME | --state LOCAL.aupreset]\n"
                   "       daw_piano_render --list-presets\n"
                   "Local Pianoteq 9 AU required. Stereo 48 kHz PCM16, 5-second release tail.\n"
                   "Default preset: NY Steinway D Classical. All parts play through one piano.\n";
      return 2;
    }
    OutputGuard output{fs::path(argv[2])};
    std::error_code ec;
    const auto status = fs::symlink_status(output.directory, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) throw std::runtime_error("cannot inspect output directory");
    if (fs::exists(status)) throw std::runtime_error("output directory must not exist");
    daw::Score score;
    daw::MidiFile midi;
    daw::MidiImportReport imported;
    std::string error;
    auto extension = fs::path(argv[1]).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    bool from_midi = false;
    if (extension == ".mid" || extension == ".midi") {
      from_midi = true;
      if (!daw::readMidiFile(argv[1], &midi, &error, &imported)) throw std::runtime_error("MIDI import: " + error);
      if (!daw::midiToScore(midi, &score, &error)) throw std::runtime_error("MIDI to project: " + error);
    } else {
      const bool loaded = extension == ".dawproj" ? daw::readProjectFile(argv[1], &score, &error) :
          (extension == ".musicxml" || extension == ".xml") ? daw::readMusicXmlFile(argv[1], &score, &error) : false;
      if (!loaded) throw std::runtime_error("score import (supported: mid/midi/dawproj/musicxml/xml): " + error);
      if (!daw::scoreToMidiFile(score, &midi, &error)) throw std::runtime_error("score to MIDI: " + error);
    }
    // Reject excessive/invalid timelines before loading third-party plugin code.
    (void)daw::makeMidiSampleSequence(midi);
    std::string name;
    std::vector<std::uint8_t> state;
    std::uint32_t version = 0;
    {
      daw::PianoteqAU setup;
      if (argc == 5 && std::string(argv[3]) == "--state") setup.restoreState(readState(argv[4]));
      else setup.selectFactoryPreset(argc == 5 ? argv[4] : "NY Steinway D Classical");
      name = setup.presetName();
      state = setup.state();
      version = setup.componentVersion();
    }
    // Bounce always consumes the serialized instrument state in a fresh AU.
    // On Pianoteq 9.2.2, direct factory selection and document-state loading
    // produce different audio despite equal exposed parameters/state. Using
    // one restoration path for both first bounce and reload removes that drift.
    daw::PianoteqAU piano;
    piano.restoreState(state);
    if (piano.state() != state) throw std::runtime_error("AU state changed when preparing the bounce");
    std::cerr << "Rendering local Pianoteq AU: " << name << '\n';
    daw::PianoRenderReport report;
    const auto audio = piano.render(midi, &report);
    std::ostringstream json;
    json.imbue(std::locale::classic());
    json << std::setprecision(17)
         << "{\n  \"bundle_version\": 1,\n  \"renderer\": \"Pianoteq 9 AU offline\","
         << "\n  \"component\": \"aumu/Pt9q/Mdrt\",\n  \"component_version\": " << version
         << ",\n  \"preset\": " << jsonString(name)
         << ",\n  \"license_status\": \"not_verified_by_host\","
         << "\n  \"project\": \"score.dawproj\",\n  \"midi\": \"performance.mid\","
         << "\n  \"instrument_state\": \"piano.aupreset\",\n  \"audio\": \"piano.wav\","
         << "\n  \"routing\": \"all parts to one fixed piano; preserve MIDI channels\","
         << "\n  \"sample_rate\": " << audio.sample_rate << ",\n  \"channels\": " << audio.channels
         << ",\n  \"frames\": " << audio.frameCount() << ",\n  \"tail_seconds\": 5,"
         << "\n  \"sent_messages\": " << report.sent_messages
         << ",\n  \"skipped_bank_program_messages\": " << report.skipped_instrument_selection
         << ",\n  \"clipped_samples\": " << report.clipped_samples
         << ",\n  \"peak_before_clipping\": " << report.peak << ",\n  \"rms_before_clipping\": " << report.rms
         << ",\n  \"last_second_rms\": " << report.last_second_rms
         << ",\n  \"source_is_midi\": " << (from_midi ? "true" : "false")
         << ",\n  \"source_ppq\": " << imported.source_ticks_per_quarter
         << ",\n  \"rounded_note_boundaries\": " << imported.rounded_note_boundaries
         << ",\n  \"rounded_tempo_events\": " << imported.rounded_tempo_events
         << ",\n  \"rounded_channel_events\": " << imported.rounded_channel_events
         << ",\n  \"rounded_meter_events\": " << imported.rounded_time_signature_events
         << ",\n  \"ignored_meta_events\": " << imported.ignored_meta_events
         << ",\n  \"ignored_sysex_events\": " << imported.ignored_sysex_events
         << ",\n  \"ambiguous_same_pitch_notes\": " << imported.overlapping_same_pitch_notes
         << "\n}\n";
    // Nothing is written until import, state capture and audio rendering succeed.
    if (!fs::create_directory(output.directory)) throw std::runtime_error("output directory must be new and creatable");
    output.created = true;
    if (!daw::writeProjectFile(score, (output.directory / "score.dawproj").string(), &error) ||
        !daw::writeMidiFile(midi, (output.directory / "performance.mid").string(), &error) ||
        !daw::writeWavPcm16(audio, (output.directory / "piano.wav").string(), &error)) throw std::runtime_error(error);
    writeBytes(output.directory / "piano.aupreset", state.data(), state.size());
    const auto text = json.str();
    writeBytes(output.directory / "report.json", text.data(), text.size());
    output.complete = true;
    std::cout << "Wrote piano bundle: " << output.directory << "\nPeak " << report.peak
              << "; clipped samples " << report.clipped_samples << "; skipped bank/program messages "
              << report.skipped_instrument_selection << '\n';
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "Piano render failed: " << exception.what() << '\n';
    return 1;
  }
}
