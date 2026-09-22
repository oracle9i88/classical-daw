#include "daw/score_midi.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace {
void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}
using Performance = std::vector<std::tuple<daw::Tick, daw::Tick, int, int>>;
Performance performance(const daw::MidiTrack& track) {
  Performance result;
  for (const auto& note : track.notes) result.emplace_back(note.start, note.end(), note.pitch, note.velocity);
  std::sort(result.begin(), result.end());
  return result;
}
}

int main() {
  const auto path = std::filesystem::temp_directory_path() / "classical_daw_musicxml_timing.musicxml";
  try {
    // A short tone is deliberately listed before a longer simultaneous one.
    // Later attacks overlap that held tone; both it and F cross a barline.
    daw::MidiFile source;
    source.tracks = {{"Polyphonic piano", {{0,960,67,61,0}, {0,4800,60,92,0},
                                          {240,600,64,44,0}, {3600,480,65,80,0}}}};
    daw::Score score, restored;
    std::string error;
    require(daw::midiToScore(source, &score, &error), error);
    require(daw::writeMusicXmlFile(score, path.string(), &error), error);
    std::ifstream in(path);
    const std::string xml((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    require(xml.find("<backup><duration>3600</duration></backup>") != std::string::npos,
            "held-tone overlap was not encoded with a backward cursor move");
    const auto first_note = xml.find("<note ");
    const auto first_chord = xml.find("<chord/>", first_note);
    require(xml.find("<duration>3840</duration>", first_note) < first_chord &&
            xml.find("<duration>960</duration>", first_chord) != std::string::npos,
            "shorter chord tone was placed before its longer base note");
    require(daw::readMusicXmlFile(path.string(), &restored, &error), error);
    daw::MidiFile result;
    require(daw::scoreToMidiFile(restored, &result, &error), error);
    require(result.tracks.size() == 1 && performance(result.tracks[0]) == performance(source.tracks[0]),
            "polyphonic MIDI/MusicXML round-trip altered note timing, pitch, or velocity");

    // A different voice finishes before the first voice. The following
    // measure starts at the furthest note end, never at the final XML cursor.
    const char* fixture =
      "<score-partwise><part-list><score-part id='P1'><part-name>Test</part-name>"
      "</score-part></part-list><part id='P1'><measure number='1'>"
      "<note><pitch><step>C</step><octave>4</octave></pitch><duration>5760</duration><voice>1</voice></note>"
      "<backup><duration>5760</duration></backup>"
      "<note><pitch><step>E</step><octave>4</octave></pitch><duration>960</duration><voice>2</voice></note>"
      "</measure><measure number='2'>"
      "<note><pitch><step>G</step><octave>4</octave></pitch><duration>960</duration></note>"
      "</measure></part></score-partwise>";
    { std::ofstream out(path); out << fixture; }
    require(daw::readMusicXmlFile(path.string(), &restored, &error), error);
    require(restored.parts[0].measures[1].start == 5760 &&
            restored.parts[0].measures[1].notes[0].start == 5760,
            "measure position ignored the longer preceding voice");
    std::filesystem::remove(path);
    std::cout << "MusicXML polyphonic timing tests passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::filesystem::remove(path);
    std::cerr << e.what() << '\n';
    return 1;
  }
}
