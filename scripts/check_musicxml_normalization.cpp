// Compare canonical notation after importing equivalent XML at different units.
// Build: c++ -std=c++17 -Isrc/engine/include scripts/check_musicxml_normalization.cpp \
//   build/libdaw_engine.a -o /tmp/check_musicxml_normalization
// Usage: check_musicxml_normalization BASELINE_XML VARIANT_XML NEW_OUTPUT_DIRECTORY
// This verifies fields supported by our XML writer, not arbitrary engraving metadata.
#include "daw/score_midi.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

std::string contents(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot read comparison output");
  return {std::istreambuf_iterator<char>(input), {}};
}
int main(int argc, char** argv) {
  try {
    if (argc != 4) throw std::runtime_error("Usage: check_musicxml_normalization BASELINE_XML VARIANT_XML NEW_OUTPUT_DIRECTORY");
    const std::filesystem::path output(argv[3]);
    if (!std::filesystem::create_directory(output)) throw std::runtime_error("output directory must not exist");
    daw::Score baseline, variant; std::string error;
    if (!daw::readMusicXmlFile(argv[1], &baseline, &error) || !daw::readMusicXmlFile(argv[2], &variant, &error))
      throw std::runtime_error(error);
    const auto first = output / "baseline-960.musicxml", second = output / "variant-960.musicxml";
    if (!daw::writeMusicXmlFile(baseline, first.string(), &error) || !daw::writeMusicXmlFile(variant, second.string(), &error))
      throw std::runtime_error(error);
    if (contents(first) != contents(second)) throw std::runtime_error("normalized XML differs; inspect output pair");
    if (!daw::writeScoreMidiFile(variant, (output / "variant.mid").string(), &error)) throw std::runtime_error(error);
    std::size_t measures = 0, note_fragments = 0;
    for (const auto& part : variant.parts) for (const auto& measure : part.measures) {
      ++measures; note_fragments += measure.notes.size();
    }
    std::cout << "PASS canonical XML identical parts=" << variant.parts.size() << " measures=" << measures
              << " note_fragments=" << note_fragments << " tempo_entries=" << variant.tempo_changes.size()+1 << '\n';
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
