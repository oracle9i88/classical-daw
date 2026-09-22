#include "daw/midi.hpp"
#include "daw/render.hpp"
#include "daw/timeline.hpp"
#include "daw/wav.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

bool closeEnough(double a, double b, double epsilon = 1e-9) {
  return std::abs(a - b) <= epsilon;
}

int fail(const std::string& message) {
  std::cerr << "FAIL: " << message << '\n';
  return 1;
}

}  // namespace

int main() {
  using namespace daw;

  TempoMap tempo(120.0);
  tempo.addChange(960, 60.0);
  if (!closeEnough(tempo.tickToSeconds(960), 0.5)) return fail("tempo boundary");
  if (!closeEnough(tempo.tickToSeconds(1920), 1.5)) return fail("tempo integration");
  if (tempo.secondsToTick(1.5) != 1920) return fail("tempo inverse");

  Timeline timeline(48000.0, tempo);
  if (timeline.tickToSample(960) != 24000) return fail("tick to sample");
  if (timeline.sampleToTick(72000) != 1920) return fail("sample to tick");

  const auto temp = std::filesystem::temp_directory_path();
  const auto midi_path = temp / "classical_daw_alpha_test.mid";
  const auto wav_path = temp / "classical_daw_alpha_test.wav";

  MidiFile original;
  original.tempo = tempo;
  original.tracks = {
      MidiTrack{"Piano", {MidiNote{0, 960, 60, 96, 0}, MidiNote{960, 480, 64, 80, 0}}},
      MidiTrack{"Bass", {MidiNote{0, 1920, 36, 70, 1}}},
  };
  std::string error;
  if (!writeMidiFile(original, midi_path.string(), &error)) return fail("MIDI write: " + error);

  MidiFile parsed;
  if (!readMidiFile(midi_path.string(), &parsed, &error)) return fail("MIDI read: " + error);
  if (parsed.tracks.size() != 2 || parsed.tracks[0].name != "Piano" || parsed.tracks[1].name != "Bass") {
    return fail("MIDI track metadata round-trip");
  }
  if (parsed.tracks[0].notes.size() != 2 || parsed.tracks[0].notes[1].pitch != 64 ||
      parsed.tracks[1].notes[0].duration != 1920) {
    return fail("MIDI notes round-trip");
  }
  if (!closeEnough(parsed.tempo.tickToSeconds(1920), 1.5)) return fail("MIDI tempo round-trip");

  const AudioBuffer rendered = renderNotes(original.tracks[0], tempo, 48000.0, 0.05);
  if (rendered.sample_rate != 48000 || rendered.channels != 1 || rendered.frameCount() < 48000) {
    return fail("offline render shape");
  }
  if (!writeWavPcm16(rendered, wav_path.string(), &error)) return fail("WAV write: " + error);
  std::ifstream wav(wav_path, std::ios::binary | std::ios::ate);
  if (!wav || wav.tellg() <= 44) return fail("WAV file missing");

  std::filesystem::remove(midi_path);
  std::filesystem::remove(wav_path);
  std::cout << "classical-daw Alpha engine tests passed\n";
  return 0;
}
