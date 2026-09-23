#include "daw/render.hpp"
#include "daw/score_midi.hpp"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace {
using daw::Tick;
void require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
struct TempDirectory {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("daw_xml_tempos_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  TempDirectory() { require(std::filesystem::create_directory(path), "create test directory"); }
  ~TempDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
void write(const std::filesystem::path& path, const std::string& value) {
  std::ofstream out(path); out << value; require(bool(out), "write fixture");
}
std::string read(const std::filesystem::path& path) {
  std::ifstream in(path); return {std::istreambuf_iterator<char>(in), {}};
}
std::string measure(const std::string& events, int number = 1, bool partial = false) {
  return "<measure number=\"" + std::to_string(number) + "\"" + (partial ? " implicit=\"yes\"" : "") +
      "><attributes><divisions>960</divisions></attributes>" + events + "</measure>";
}
std::string document(const std::string& first, const std::string& second = {}) {
  return "<score-partwise version=\"4.0\"><part-list><score-part id=\"P1\"><part-name>First</part-name></score-part>" +
      (second.empty() ? std::string{} : "<score-part id=\"P2\"><part-name>Second</part-name></score-part>") +
      "</part-list><part id=\"P1\">" + first + "</part>" +
      (second.empty() ? std::string{} : "<part id=\"P2\">" + second + "</part>") + "</score-partwise>";
}
std::string note(Tick duration, int voice = 1) {
  return "<note><pitch><step>A</step><octave>4</octave></pitch><duration>" + std::to_string(duration) +
      "</duration><voice>" + std::to_string(voice) + "</voice></note>";
}
std::string direction(const std::string& sound, const std::string& offset = {}) {
  return "<direction><direction-type><words>Tempo</words></direction-type>" + offset + sound + "</direction>";
}
std::string offset(const std::string& ticks, const std::string& flag = "yes") {
  return "<offset" + (flag.empty() ? std::string{} : " sound=\"" + flag + "\"") + ">" + ticks + "</offset>";
}
daw::Score load(const std::filesystem::path& path, const std::string& xml) {
  write(path, xml); daw::Score result; std::string error;
  require(daw::readMusicXmlFile(path.string(), &result, &error), "import: " + error);
  return result;
}
void tempos(const daw::Score& score, double initial, std::vector<daw::TempoChange> expected) {
  require(score.bpm == initial && score.tempo_changes.size() == expected.size(), "tempo count/initial mismatch");
  for (std::size_t i = 0; i < expected.size(); ++i) {
    require(score.tempo_changes[i].tick == expected[i].tick && score.tempo_changes[i].bpm == expected[i].bpm,
            "tempo position or BPM mismatch at " + std::to_string(i));
  }
}
void cursorAndOffsets(const std::filesystem::path& path) {
  const auto xml = document(measure(
      "<sound tempo=\"60\"/>" + note(1920) +
      direction("<sound tempo=\"120\"/>", offset("-960.000")) +
      "<backup><duration>1920</duration></backup>" + note(480, 2) +
      direction("<sound tempo=\"80\"/>", offset("720.5", "")) + // visual only: tick 480
      direction("<sound tempo=\"90\"><offset sound=\"no\">960</offset></sound>", offset("-99.5")) +
      "<forward><duration>1440</duration></forward>" + "<sound tempo=\"30\"/>"));
  const auto score = load(path, xml);
  tempos(score, 60, {{480,80}, {960,120}, {1440,90}, {1920,30}});
  require(score.parts[0].measures[0].notes.size() == 2 && score.parts[0].measures[0].notes[0].duration == 1920 &&
              score.parts[0].measures[0].notes[1].start == 0, "directions moved the voice cursor");
  auto later = load(path, document(measure(note(960) + "<sound tempo=\"60\"/>")));
  tempos(later, 120, {{960,60}}); // Never backdate a first tempo that occurs after the first note.
  auto boundary = load(path, document(measure(note(960), 1, true) +
      measure(direction("<sound tempo=\"72\"/>") + note(480), 2, true)));
  tempos(boundary, 120, {{960,72}});
  require(boundary.parts[0].measures[1].start == 960, "pickup shifted following tempo");
}
void metronomesAndParts(const std::filesystem::path& path) {
  const auto metronome = [](const std::string& content, const std::string& extra = {}) {
    return "<direction><direction-type><metronome>" + content + "</metronome></direction-type>" + extra + "</direction>";
  };
  const auto mark = metronome("<beat-unit>quarter</beat-unit><beat-unit-dot/><per-minute>60</per-minute>");
  auto score = load(path, document(measure(mark + note(960) +
      metronome("<beat-unit>half</beat-unit><per-minute>60</per-minute>")),
      measure("<sound tempo=\"90\"/>" + note(960) + "<sound tempo=\"120\"/>")));
  tempos(score, 90, {{960,120}}); // Dotted-quarter 60 = quarter 90; duplicate part marks coalesce.
  score = load(path, document(measure(note(1920)), measure(note(960) + "<sound tempo=\"77\"/>")));
  tempos(score, 120, {{960,77}}); // A sparse declaration in another part is global.
  score = load(path, document(measure(metronome("<beat-unit>quarter</beat-unit><per-minute>Allegro</per-minute>",
      "<sound tempo=\"123.25\"/>") + note(960))));
  tempos(score, 123.25, {}); // The explicit sound controls playback, display text does not.
  score = load(path, document(measure(metronome("<beat-unit>eighth</beat-unit><beat-unit-dot/><beat-unit-dot/><per-minute>80</per-minute>") + note(960))));
  tempos(score, 70, {});
}
void exportAndRender(const std::filesystem::path& path) {
  daw::Score source; source.bpm = 60;
  daw::ScoreNote held; held.pitch = {'A',0,4}; held.start = 0; held.duration = 1920; held.velocity = 90;
  source.parts = {{"P1", "Held", {{1,0,{held},1920}}}};
  source.tempo_changes = {{480,120}, {960,30}, {1920,75}};
  const auto before = daw::renderScore(source, 12000, 0.1);
  require(before.frameCount() == 34200, "known held-note duration should be 2.75 s + 0.1 s tail");
  std::string error; daw::MusicXmlExportReport report;
  require(daw::writeMusicXmlFile(source, path.string(), &error, &report), "write held note: " + error);
  auto restored = load(path, read(path));
  tempos(restored, 60, source.tempo_changes);
  require(report.omitted_tempo_changes == 0 && restored.parts[0].measures[0].notes.size() == 1 &&
              restored.parts[0].measures[0].duration == 1920, "tempo export split held note or enlarged partial bar");
  const auto after = daw::renderScore(restored, 12000, 0.1);
  require(before.samples == after.samples, "tempo XML round-trip changed held-note samples or reattacked it");

  // First part ends early; only the longer part can carry the complete global map.
  source.parts[0].measures[0].duration = 960;
  source.parts[0].measures[0].notes[0].duration = 960;
  held.start = 960; held.duration = 960;
  source.parts.push_back({"P2", "Longer", {{1,0,{},960}, {2,960,{held},960}}});
  source.tempo_changes = {{480,60}, {960,60000000.0/766667.0}, {1440,0.000001}, {1920,75}};
  require(daw::writeMusicXmlFile(source, path.string(), &error, &report), "write sparse parts: " + error);
  restored = load(path, read(path));
  tempos(restored, 60, source.tempo_changes); // Preserve redundant marks, precision and final-end mark.
  require(restored.parts[0].measures.size() == 1 && restored.parts[1].measures.size() == 2, "tempo export padded shorter part");
  const auto bytes = read(path);
  source.tempo_changes.push_back({1921,100}); report.omitted_tempo_changes = 77;
  require(!daw::writeMusicXmlFile(source, path.string(), &error, &report) && !error.empty(), "accepted tempo beyond score extent");
  require(read(path) == bytes && report.omitted_tempo_changes == 77, "failed export changed destination or diagnostics");
}
void rejects(const std::filesystem::path& path) {
  const std::vector<std::string> bad{
      document(measure("<sound tempo=\"0\"/>")), document(measure("<sound tempo=\"-1\"/>")),
      document(measure("<sound tempo=\"1000000.1\"/>")), document(measure("<sound tempo=\"NaN\"/>")),
      document(measure("<sound tempo=\"1e2\"/>")), document(measure("<sound tempo=\"\"/>")),
      document(measure("<sound tempo=\"120\" time-only=\"2\"/>")),
      document(measure(direction("<sound tempo=\"90\"/>", offset("-1")))),
      document(measure(direction("<sound tempo=\"90\"/>", offset("3841")))),
      document(measure(direction("<sound tempo=\"90\"/>", offset("0.5")))),
      document(measure(direction("<sound tempo=\"90\"/>", offset("0", "maybe")))),
      document(measure(direction("<sound tempo=\"90\"/>", offset("0") + offset("0")))),
      document(measure(direction("<sound tempo=\"90\"><offset>0.5</offset></sound>"))),
      document(measure(note(1) + direction("<sound tempo=\"90\"/>", offset("9223372036854775807")))),
      document(measure(direction("<sound tempo=\"90\"/>", offset("9223372036854775808")))),
      document(measure(note(960) + direction("<sound tempo=\"90\"/>", offset("1")),1,true)),
      document(measure("<sound tempo=\"90\"/>"), measure("<sound tempo=\"91\"/>")),
      document(measure(note(960) + "<sound tempo=\"90\"/>"), measure(note(960) + "<sound tempo=\"91\"/>")),
      document(measure("<direction><direction-type><metronome><beat-unit>quarter</beat-unit><per-minute>80-90</per-minute></metronome></direction-type></direction>")),
      document(measure("<direction><direction-type><metronome><beat-unit>quarter</beat-unit><beat-unit>half</beat-unit></metronome></direction-type></direction>")),
      document(measure("<sound tempo=\"90\"><offset>1</sound></offset>"))
  };
  daw::Score destination; destination.bpm = 73; destination.tempo_changes = {{123,47}};
  destination.parts = {{"sentinel", "Keep me", {{1,0,{},960}}}};
  for (std::size_t i = 0; i < bad.size(); ++i) {
    write(path, bad[i]); std::string error;
    require(!daw::readMusicXmlFile(path.string(), &destination, &error) && !error.empty(), "accepted bad fixture " + std::to_string(i));
    tempos(destination, 73, {{123,47}});
    require(destination.parts.size() == 1 && destination.parts[0].id == "sentinel", "failed read modified destination");
  }
}
}
int main() {
  try {
    TempDirectory directory; const auto path = directory.path / "tempo.musicxml";
    cursorAndOffsets(path); metronomesAndParts(path); exportAndRender(path); rejects(path);
    std::cout << "MusicXML tempo timing, parts, rendering and failure tests passed\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
