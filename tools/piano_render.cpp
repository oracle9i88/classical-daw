#include "audio_unit_instrument.hpp"
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
                             "performance.mid", "performance.mid.tmp", "report.json", "instrument.wav", "instrument.aupreset"}) {
      fs::remove(directory / name, ignored);
    }
    fs::remove(directory, ignored);  // Never recursively delete unexpected content.
  }
};
}

int main(int argc, char** argv) {
  try {
    const bool list = argc >= 2 && std::string(argv[1]) == "--list-presets";
    daw::InstrumentKind kind = daw::InstrumentKind::Pianoteq9;
    std::string preset, state_path;
    bool selected_kind = false;
    const int option_start = list ? 2 : 3;
    if ((!list && argc < 3) || (argc - option_start) % 2 != 0) {
      std::cerr << "Usage: " << argv[0] << " INPUT NEW_OUTPUT_DIRECTORY [--preset NAME | --state FILE]"
#ifdef DAW_GENERIC_INSTRUMENT_CLI
                << " [--instrument pianoteq|swam-cello]"
#endif
                << "\n       " << argv[0] << " --list-presets [--instrument ID]\n";
      return 2;
    }
    for (int i = option_start; i < argc; i += 2) {
      const std::string option = argv[i], value = argv[i + 1];
      if (!list && option == "--preset" && preset.empty() && state_path.empty() && !value.empty()) preset = value;
      else if (!list && option == "--state" && state_path.empty() && preset.empty() && !value.empty()) state_path = value;
#ifdef DAW_GENERIC_INSTRUMENT_CLI
      else if (option == "--instrument" && !selected_kind) {
        if (value == "swam-cello") kind = daw::InstrumentKind::SwamCello3;
        else if (value != "pianoteq") throw std::invalid_argument("supported instruments: pianoteq, swam-cello");
        selected_kind = true;
      }
#endif
      else throw std::invalid_argument("unknown, duplicate or incompatible option: " + option);
    }
    (void)selected_kind;
    const auto& profile = daw::instrumentDescriptor(kind);
    if (list) {
      daw::AudioUnitInstrument instrument(kind);
      for (const auto& name : instrument.factoryPresets()) std::cout << name << '\n';
      return 0;
    }
    const std::string audio_name = kind == daw::InstrumentKind::Pianoteq9 ? "piano.wav" : "instrument.wav";
    const std::string state_name = kind == daw::InstrumentKind::Pianoteq9 ? "piano.aupreset" : "instrument.aupreset";
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
    const auto sequence = daw::makeMidiSampleSequence(midi);
    if (kind == daw::InstrumentKind::SwamCello3) daw::requireInitialExpression(sequence);
    std::string name;
    std::vector<std::uint8_t> state;
    std::uint32_t version = 0;
    {
      daw::AudioUnitInstrument setup(kind);
      if (!state_path.empty()) {
        state = readState(state_path);
        setup.restoreState(state);
      } else {
        setup.selectFactoryPreset(preset.empty() ? profile.default_preset : preset);
        state = setup.state();
      }
      name = setup.presetName();
      version = setup.componentVersion();
    }
    // Bounce always consumes the serialized instrument state in a fresh AU.
    // On Pianoteq 9.2.2, direct factory selection and document-state loading
    // produce different audio despite equal exposed parameters/state. Using
    // one restoration path for both first bounce and reload removes that drift.
    daw::AudioUnitInstrument piano(kind);
    piano.restoreState(state);
    const bool restored_bytes_equal = piano.state() == state;
    // SWAM refreshes a datetime field in its JUCE state on restoration. Retain
    // the exact source snapshot in the bundle instead of overwriting it with
    // transient metadata. Audio equivalence is covered by local integration tests.
    if (kind == daw::InstrumentKind::Pianoteq9 && !restored_bytes_equal) {
      throw std::runtime_error("AU state changed when preparing the bounce");
    }
    if (piano.presetName() != name) throw std::runtime_error("AU preset changed when preparing the bounce");
    std::cerr << "Rendering local " << profile.name << " AU: " << name << '\n';
    daw::InstrumentRenderReport report;
    const auto audio = piano.render(midi, &report);
    if (report.peak == 0.0 && std::any_of(midi.tracks.begin(), midi.tracks.end(),
        [](const daw::MidiTrack& track) { return !track.notes.empty(); })) {
      throw std::runtime_error("AU returned silence for sounding notes; check plugin startup, activation, MIDI mapping and expression");
    }
    std::ostringstream json;
    json.imbue(std::locale::classic());
    json << std::setprecision(17)
         << "{\n  \"bundle_version\": 1,\n  \"renderer\": " << jsonString(std::string(profile.name) + " AU offline")
         << ",\n  \"component\": " << jsonString(profile.component_id) << ",\n  \"component_version\": " << version
         << ",\n  \"preset\": " << jsonString(name)
         << ",\n  \"restored_state_bytes_equal\": " << (restored_bytes_equal ? "true" : "false")
         << ",\n  \"license_status\": \"not_verified_by_host\","
         << "\n  \"project\": \"score.dawproj\",\n  \"midi\": \"performance.mid\","
         << "\n  \"instrument_state\": " << jsonString(state_name) << ",\n  \"audio\": " << jsonString(audio_name) << ","
         << "\n  \"routing\": \"all parts to one fixed instrument; preserve MIDI channels\",\n  \"startup_seconds\": " << profile.startup_seconds << ","
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
        !daw::writeWavPcm16(audio, (output.directory / audio_name).string(), &error)) throw std::runtime_error(error);
    writeBytes(output.directory / state_name, state.data(), state.size());
    const auto text = json.str();
    writeBytes(output.directory / "report.json", text.data(), text.size());
    output.complete = true;
    std::cout << "Wrote instrument bundle: " << output.directory << "\nPeak " << report.peak
              << "; clipped samples " << report.clipped_samples << "; skipped bank/program messages "
              << report.skipped_instrument_selection << '\n';
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "Instrument render failed: " << exception.what() << '\n';
    return 1;
  }
}
