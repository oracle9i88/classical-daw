#include "daw/project.hpp"
#include "daw/score_midi.hpp"

#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "Usage: daw_midi_roundtrip INPUT_MIDI NEW_OUTPUT_DIRECTORY\n";
    return 2;
  }
  std::error_code ec;
  const std::filesystem::path directory(argv[2]);
  if (!std::filesystem::create_directory(directory, ec)) {
    std::cerr << "Output directory must not exist and must be creatable\n";
    return 2;
  }
  daw::MidiFile midi;
  daw::Score score, loaded;
  std::string error;
  const auto project = (directory / "roundtrip.dawproj").string();
  if (!daw::readMidiFile(argv[1], &midi, &error) ||
      !daw::writeMidiFile(midi, (directory / "direct.mid").string(), &error) ||
      !daw::midiToScore(midi, &score, &error) ||
      !daw::writeProjectFile(score, project, &error) ||
      !daw::readProjectFile(project, &loaded, &error) ||
      !daw::writeScoreMidiFile(loaded, (directory / "project.mid").string(), &error)) {
    std::cerr << "Round-trip failed: " << error << '\n';
    return 1;
  }
  std::cout << "Wrote direct.mid, roundtrip.dawproj, project.mid for independent comparison.\n"
            << "Score retains tempo and meter maps; this tool does not assert lossless notation.\n";
}
