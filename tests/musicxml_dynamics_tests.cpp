#include "daw/score.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {

int fail(const std::string& message) {
  std::cerr << "FAIL: " << message << '\n';
  return 1;
}

std::string readText(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {(std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()};
}

void writeText(const std::filesystem::path& path, const std::string& text) {
  std::ofstream file(path, std::ios::binary);
  file << text;
}

std::string fixture(const std::string& attributes) {
  return "<score-partwise version=\"4.0\"><part-list>"
         "<score-part id=\"P1\"><part-name>Piano</part-name></score-part></part-list>"
         "<part id=\"P1\"><measure number=\"1\"><note" + attributes + ">"
         "<pitch><step>C</step><octave>4</octave></pitch><duration>960</duration>"
         "</note></measure></part></score-partwise>";
}

daw::Score singleNoteScore() {
  daw::Score score;
  score.parts = {daw::ScorePart{"P1", "Piano", {daw::ScoreMeasure{1, 0, {daw::ScoreNote{}}}}}};
  return score;
}

}  // namespace

int main() {
  using namespace daw;
  const auto path = std::filesystem::temp_directory_path() / "classical_daw_musicxml_dynamics.musicxml";
  std::string error;
  Score original = singleNoteScore();
  auto& notes = original.parts[0].measures[0].notes;
  notes.clear();
  // Every MIDI velocity must survive the decimal conversion, including zero.
  for (int velocity = 0; velocity <= 127; ++velocity) {
    ScoreNote note;
    note.start = velocity;
    note.duration = 1;
    note.velocity = static_cast<std::uint8_t>(velocity);
    notes.push_back(note);
  }
  if (!writeMusicXmlFile(original, path.string(), &error)) return fail("write velocities: " + error);
  const std::string xml = readText(path);
  if (xml.find("<note dynamics=\"100\">") == std::string::npos || xml.find("<velocity>") != std::string::npos) {
    return fail("writer did not use the standard dynamics percentage attribute");
  }
  Score parsed;
  if (!readMusicXmlFile(path.string(), &parsed, &error) || parsed.parts[0].measures[0].notes.size() != notes.size()) {
    return fail("read velocities: " + error);
  }
  for (std::size_t index = 0; index < notes.size(); ++index) {
    if (parsed.parts[0].measures[0].notes[index].velocity != notes[index].velocity) {
      return fail("velocity round-trip changed " + std::to_string(index));
    }
  }

  const std::vector<std::pair<std::string, int>> standard_values = {
      {"", 100}, {" dynamics=\"0\"", 0}, {" dynamics=\"50\"", 45},
      {" dynamics=\"100\"", 90}, {" dynamics=\"120\"", 108},
      {" dynamics=\"141.11111111111111\"", 127}, {" dynamics=\"1\"", 1},
      {" dynamics=\"101\"", 91}, {" dynamics = '  +100.0  '", 90},
      {" dynamics='50.'", 45}, {" dynamics='.5'", 0},
      {" data-dynamics=\"50\"", 100}, {" id=\"dynamics='50'\"", 100},
      {" data-dynamics=\"50\" dynamics='120'", 108}};
  for (const auto& example : standard_values) {
    writeText(path, fixture(example.first));
    if (!readMusicXmlFile(path.string(), &parsed, &error) ||
        parsed.parts[0].measures[0].notes[0].velocity != example.second) {
      return fail("standard dynamics conversion " + example.first + ": " + error);
    }
  }

  Score protected_score = singleNoteScore();
  protected_score.bpm = 71.0;
  protected_score.parts[0].name = "untouched";
  protected_score.parts[0].measures[0].notes[0].velocity = 42;
  const std::vector<std::string> invalid_values = {
      "", " ", "-1", "nan", "NaN", "inf", "-inf", "142", "141.2", "9999999999999999999999999999999999999999",
      "100junk", "100 50", "1e2", "0x64", ".", "+", "1.2.3"};
  for (const std::string& value : invalid_values) {
    parsed = protected_score;
    error.clear();
    writeText(path, fixture(" dynamics=\"" + value + "\""));
    if (readMusicXmlFile(path.string(), &parsed, &error) || error.find("dynamics") == std::string::npos ||
        parsed.bpm != 71.0 || parsed.parts.size() != 1 || parsed.parts[0].name != "untouched" ||
        parsed.parts[0].measures.size() != 1 || parsed.parts[0].measures[0].notes.size() != 1 ||
        parsed.parts[0].measures[0].notes[0].velocity != 42) {
      return fail("invalid dynamics accepted or changed output: " + value + ": " + error);
    }
  }
  for (const int velocity : {128, 255}) {
    Score invalid = singleNoteScore();
    invalid.parts[0].measures[0].notes[0].velocity = static_cast<std::uint8_t>(velocity);
    writeText(path, "existing-score");
    error.clear();
    if (writeMusicXmlFile(invalid, path.string(), &error) || error.find("velocity") == std::string::npos ||
        readText(path) != "existing-score") {
      return fail("invalid velocity damaged destination or was accepted");
    }
  }

  // Ties precede voice; time-modification precedes staff in the note schema.
  Score structured = singleNoteScore();
  auto& note = structured.parts[0].measures[0].notes[0];
  note.velocity = 83;
  note.tie_start = true;
  note.tie_stop = true;
  note.voice = 2;
  note.staff = 2;
  note.tuplet_actual = 3;
  note.tuplet_normal = 2;
  if (!writeMusicXmlFile(structured, path.string(), &error)) return fail("structured note write: " + error);
  const std::string ordered = readText(path);
  const auto duration = ordered.find("<duration>");
  const auto stop = ordered.find("<tie type=\"stop\"");
  const auto start = ordered.find("<tie type=\"start\"");
  const auto voice = ordered.find("<voice>");
  const auto modification = ordered.find("<time-modification>");
  const auto staff = ordered.find("<staff>");
  const auto notations = ordered.find("<notations>");
  if (!(duration < stop && stop < start && start < voice && voice < modification && modification < staff && staff < notations)) {
    return fail("note children are outside MusicXML schema order");
  }
  if (!readMusicXmlFile(path.string(), &parsed, &error)) return fail("structured note read: " + error);
  const auto& restored = parsed.parts[0].measures[0].notes[0];
  if (restored.velocity != 83 || !restored.tie_start || !restored.tie_stop || restored.staff != 2 || restored.voice != 2 ||
      restored.tuplet_actual != 3 || restored.tuplet_normal != 2) {
    return fail("structured note round-trip lost notation or velocity");
  }
  std::filesystem::remove(path);
  std::cout << "classical-daw MusicXML dynamics tests passed\n";
  return 0;
}
