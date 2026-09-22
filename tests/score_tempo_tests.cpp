#include "daw/history.hpp"
#include "daw/render.hpp"
#include "daw/score_midi.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using daw::Score;
using daw::TempoChange;
using daw::Tick;
constexpr double kRate = 12000.0;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void near(double actual, double expected, double tolerance, const std::string& reason) {
  require(std::abs(actual - expected) <= tolerance,
          reason + ": expected " + std::to_string(expected) + ", got " + std::to_string(actual));
}

bool sameDouble(double a, double b) {
  std::uint64_t first = 0;
  std::uint64_t second = 0;
  static_assert(sizeof(first) == sizeof(a));
  std::memcpy(&first, &a, sizeof(a));
  std::memcpy(&second, &b, sizeof(b));
  return first == second;
}

bool sameTempos(const std::vector<TempoChange>& a, const std::vector<TempoChange>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t index = 0; index < a.size(); ++index) {
    if (a[index].tick != b[index].tick || !sameDouble(a[index].bpm, b[index].bpm)) return false;
  }
  return true;
}

struct TempDirectory {
  std::filesystem::path path;
  TempDirectory() {
    path = std::filesystem::temp_directory_path() /
           ("classical_daw_score_tempo_" + std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()));
    require(std::filesystem::create_directory(path), "could not create isolated tempo test directory");
  }
  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

std::string readText(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  require(static_cast<bool>(file), "could not read test destination");
  return {(std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()};
}

void writeText(const std::filesystem::path& path, const std::string& text) {
  std::ofstream file(path, std::ios::binary);
  file << text;
  require(static_cast<bool>(file), "could not write test destination");
}

daw::ScoreNote note(Tick start, Tick duration) {
  daw::ScoreNote result;
  result.start = start;
  result.duration = duration;
  result.pitch = {'A', 0, 4};
  result.velocity = 90;
  return result;
}

Score scoreWithNotes(std::vector<daw::ScoreNote> notes = {note(0, 960)}) {
  Score result;
  result.bpm = 60.0;
  result.parts = {daw::ScorePart{"P1", "Tempo fixture", {daw::ScoreMeasure{1, 0, std::move(notes)}}}};
  return result;
}

bool sameScoreProbe(const Score& a, const Score& b) {
  if (!sameDouble(a.bpm, b.bpm) || !sameTempos(a.tempo_changes, b.tempo_changes) ||
      a.divisions != b.divisions || a.parts.size() != b.parts.size()) return false;
  for (std::size_t part = 0; part < a.parts.size(); ++part) {
    if (a.parts[part].name != b.parts[part].name ||
        a.parts[part].measures.size() != b.parts[part].measures.size()) return false;
    for (std::size_t measure = 0; measure < a.parts[part].measures.size(); ++measure) {
      const auto& left = a.parts[part].measures[measure].notes;
      const auto& right = b.parts[part].measures[measure].notes;
      if (left.size() != right.size()) return false;
      for (std::size_t index = 0; index < left.size(); ++index) {
        if (left[index].start != right[index].start || left[index].duration != right[index].duration ||
            left[index].pitch.step != right[index].pitch.step || left[index].velocity != right[index].velocity)
          return false;
      }
    }
  }
  return true;
}

double rms(const daw::AudioBuffer& audio, double start, double end) {
  const auto first = static_cast<std::size_t>(std::llround(start * kRate));
  const auto last = static_cast<std::size_t>(std::llround(end * kRate));
  require(first < last && last <= audio.samples.size(), "invalid audio test window");
  double sum = 0.0;
  for (auto index = first; index < last; ++index) {
    require(std::isfinite(audio.samples[index]), "tempo render produced invalid audio");
    sum += static_cast<double>(audio.samples[index]) * audio.samples[index];
  }
  return std::sqrt(sum / static_cast<double>(last - first));
}

void mathematicalTimelineAndSound() {
  auto score = scoreWithNotes({note(0, 480), note(960, 480), note(1920, 480)});
  score.tempo_changes = {{960, 120.0}, {1920, 30.0}};
  const auto map = daw::scoreTempoMap(score);
  const std::vector<std::pair<Tick, double>> expected = {
      {0, 0.0}, {480, 0.5}, {960, 1.0}, {1440, 1.25}, {1920, 1.5}, {2400, 2.5}, {2880, 3.5}};
  for (const auto& point : expected) {
    near(map.tickToSeconds(point.first), point.second, 1.0e-12, "piecewise musical time conversion");
    require(map.secondsToTick(point.second) == point.first, "inverse piecewise time conversion");
  }
  const auto audio = daw::renderScore(score, kRate, 0.1);
  require(audio.sample_rate == 12000 && audio.channels == 1 && audio.frameCount() == 31200,
          "60 -> 120 -> 30 BPM score should end at 2.5 s plus a 0.1 s tail");
  require(rms(audio, 0.1, 0.4) > 0.01, "first, one-second-per-quarter segment is silent");
  require(rms(audio, 0.6, 0.9) < 1.0e-7, "first rest was shortened by a later tempo");
  require(rms(audio, 1.05, 1.2) > 0.01, "fast segment's note onset or duration is incorrect");
  require(rms(audio, 1.3, 1.45) < 1.0e-7, "fast segment's note lasted beyond its release");
  require(rms(audio, 1.7, 2.4) > 0.01, "slow segment did not retain its longer sounding duration");
  require(rms(audio, 2.55, 2.59) < 1.0e-7, "slow segment failed to release at the correct time");

  // Editing the initial tempo does not rescale any authored later BPM values.
  score.bpm = 90.0;
  const auto edited = daw::scoreTempoMap(score);
  require(edited.changes().size() == 3 && edited.changes()[0].bpm == 90.0 &&
              edited.changes()[1].bpm == 120.0 && edited.changes()[2].bpm == 30.0,
          "the initial BPM must be authoritative without scaling later changes");
  const auto constant = daw::scoreTempoMap(scoreWithNotes());
  require(constant.changes().size() == 1 && constant.changes()[0].tick == 0 &&
              constant.changes()[0].bpm == 60.0,
          "empty later-tempo vector must retain legacy constant-tempo behavior");
}

void bridgeAndConductor(const std::filesystem::path& directory) {
  daw::MidiFile source;
  source.tempo = daw::TempoMap(91.25);
  source.tempo.addChange(480, 135.5);
  source.tempo.addChange(1200, 47.25);
  source.tempo.addChange(3600, 102.125);  // Retain a change after the final note.
  source.tracks = {{"Conductor", {}, {}},
                   {"Violin", {{0, 960, 69, 90, 0}}, {}},
                   {"Cello", {{1440, 1440, 45, 75, 1}}, {}}};
  const auto before_tempos = source.tempo.changes();
  Score score;
  std::string error;
  require(daw::midiToScore(source, &score, &error), "MIDI to score tempo import: " + error);
  require(score.parts.size() == 2 && score.parts[0].name == "Violin" && score.parts[1].name == "Cello",
          "metadata-only conductor should not become a note part");
  require(score.bpm == 91.25 && score.tempo_changes.size() == 3,
          "conductor tempo map was lost while omitting its empty part");
  for (std::size_t index = 0; index < score.tempo_changes.size(); ++index) {
    require(score.tempo_changes[index].tick == before_tempos[index + 1].tick &&
                sameDouble(score.tempo_changes[index].bpm, before_tempos[index + 1].bpm),
            "MIDI import changed a later tempo tick or BPM");
  }
  daw::MidiFile restored;
  require(daw::scoreToMidiFile(score, &restored, &error), "score to MIDI tempo export: " + error);
  require(sameTempos(before_tempos, restored.tempo.changes()), "in-memory tempo round-trip lost precision");
  require(sameTempos(source.tempo.changes(), before_tempos) && source.tracks.size() == 3 &&
              source.tracks[0].name == "Conductor",
          "tempo bridge changed the input MIDI file");

  const auto midi_path = directory / "tempo-roundtrip.mid";
  require(daw::writeScoreMidiFile(score, midi_path.string(), &error), "write tempo-bearing SMF: " + error);
  daw::MidiFile wire;
  require(daw::readMidiFile(midi_path.string(), &wire, &error), "read tempo-bearing SMF: " + error);
  require(wire.tempo.changes().size() == before_tempos.size(), "SMF omitted a later tempo event");
  for (std::size_t index = 0; index < before_tempos.size(); ++index) {
    const auto& actual = wire.tempo.changes()[index];
    const auto& expected = before_tempos[index];
    require(actual.tick == expected.tick, "SMF changed a tempo position");
    near(60000000.0 / actual.bpm, 60000000.0 / expected.bpm, 0.500001,
         "SMF tempo exceeded its half-microsecond rounding budget");
  }
}

void tiesAcrossTempoChanges() {
  auto first = note(0, 960);
  first.tie_start = true;
  auto last = note(960, 960);
  last.tie_stop = true;
  last.velocity = 25;
  auto tied = scoreWithNotes({first, last});
  tied.tempo_changes = {{960, 120.0}, {1920, 30.0}};
  auto sustained = scoreWithNotes({note(0, 1920)});
  sustained.tempo_changes = tied.tempo_changes;
  daw::MidiFile midi;
  std::string error;
  require(daw::scoreToMidiFile(tied, &midi, &error), "tie across tempo change: " + error);
  require(midi.tracks.size() == 1 && midi.tracks[0].notes.size() == 1 &&
              midi.tracks[0].notes[0].start == 0 && midi.tracks[0].notes[0].duration == 1920 &&
              midi.tracks[0].notes[0].velocity == 90,
          "tempo change split a tie into attacks or replaced its attack velocity");
  const auto tied_audio = daw::renderScore(tied, kRate, 0.1);
  const auto sustained_audio = daw::renderScore(sustained, kRate, 0.1);
  require(tied_audio.frameCount() == 19200, "tied note should last 1.5 s plus tail at the changed tempo");
  require(tied_audio.samples == sustained_audio.samples,
          "tied voice reattacked, changed phase, or changed gain at a tempo boundary");
  require(rms(tied_audio, 0.995, 1.005) > 0.01, "tempo-boundary tie developed an attack gap");
}

void historyPreservesCompleteMap() {
  auto first = scoreWithNotes();
  first.bpm = 71.0;
  first.tempo_changes = {{480, 82.0}, {1440, 93.0}};
  auto second = first;
  second.bpm = 104.0;
  second.tempo_changes = {{960, 115.0}};
  daw::ScoreHistory history(first);
  std::string error;
  require(history.commit(second, &error), "tempo edit history commit: " + error);
  auto output = scoreWithNotes();
  output.tempo_changes = {{17, 777.0}};  // Detect a manual-copy path leaving stale tempo state.
  require(history.undo(&output, &error) && sameScoreProbe(output, first),
          "undo did not restore the complete tempo map");
  require(history.redo(&output, &error) && sameScoreProbe(output, second),
          "redo did not restore the complete tempo map");
  output.tempo_changes = {{23, 888.0}};
  require(history.snapshot(&output, &error) && sameScoreProbe(output, second),
          "history snapshot retained the destination's stale tempo changes");
  require(history.reset(first, &error), "tempo history reset: " + error);
  require(history.snapshot(&output, &error) && sameScoreProbe(output, first),
          "history reset failed to retain the new baseline tempo map");
  require(!history.canUndo() && !history.canRedo(), "history reset retained an old tempo-edit branch");
}

void xmlOmissions(const std::filesystem::path& directory) {
  auto score = scoreWithNotes();
  score.bpm = 73.5;
  score.tempo_changes = {{480, 100.0}, {1920, 55.0}};
  const auto original = score;
  const auto path = directory / "tempo.musicxml";
  daw::MusicXmlExportReport report{77, 88, 99};
  std::string error;
  require(daw::writeMusicXmlFile(score, path.string(), &error, &report), "MusicXML tempo export: " + error);
  require(report.omitted_tempo_changes == 2 && report.omitted_midi_events == 0 &&
              report.omitted_note_midi_metadata == 0,
          "MusicXML must explicitly count omitted later tempo changes");
  Score imported;
  require(daw::readMusicXmlFile(path.string(), &imported, &error), "MusicXML initial tempo import: " + error);
  near(imported.bpm, 73.5, 1.0e-12, "MusicXML lost its supported initial tempo");
  require(imported.tempo_changes.empty(), "MusicXML fabricated unsupported later tempo changes");
  require(sameScoreProbe(score, original), "MusicXML export changed the source tempo map");
  score.tempo_changes.clear();
  require(daw::writeMusicXmlFile(score, path.string(), &error, &report) && report.omitted_tempo_changes == 0,
          "successful constant-tempo export did not clear stale omission diagnostics");

  score.bpm = 60000000.0 / 766667.0;
  require(daw::writeMusicXmlFile(score, path.string(), &error, &report) &&
              daw::readMusicXmlFile(path.string(), &imported, &error),
          "high-precision initial tempo XML round-trip: " + error);
  near(imported.bpm, score.bpm, 1.0e-12, "MusicXML rounded the initial BPM to display precision");

  score.bpm = 1.0e-6;
  require(daw::writeMusicXmlFile(score, path.string(), &error, &report) &&
              daw::readMusicXmlFile(path.string(), &imported, &error),
          "small positive initial tempo XML round-trip: " + error);
  near(imported.bpm, score.bpm, 1.0e-18, "MusicXML lost a small positive decimal BPM");
  const auto xml = readText(path);
  const auto sound = xml.find("<sound");
  const auto tempo = sound == std::string::npos ? std::string::npos : xml.find("tempo=\"", sound);
  require(tempo != std::string::npos, "MusicXML omitted its sound tempo attribute");
  const auto value_start = tempo + std::string("tempo=\"").size();
  const auto value_end = xml.find('"', value_start);
  require(value_end != std::string::npos, "MusicXML sound tempo attribute was not closed");
  require(xml.substr(value_start, value_end - value_start).find_first_of("eE") == std::string::npos,
          "MusicXML sound tempo must use a decimal, not scientific notation");
}

void invalidMaps(const std::filesystem::path& directory) {
  std::string error;
  const auto xml_path = directory / "protected.musicxml";
  const auto midi_path = directory / "protected.mid";
  const daw::MusicXmlExportReport sentinel{11, 22, 33};
  auto reject = [&](const Score& invalid, const std::string& reason) {
    const Score original = invalid;
    bool threw = false;
    try {
      (void)daw::scoreTempoMap(invalid);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    require(threw, "scoreTempoMap accepted " + reason);
    daw::MidiFile destination;
    destination.tempo = daw::TempoMap(79.0);
    destination.tempo.addChange(333, 89.0);
    destination.tracks = {{"untouched", {{17, 43, 42, 61, 4}}, {}}};
    const auto old_tempos = destination.tempo.changes();
    require(!daw::scoreToMidiFile(invalid, &destination, &error) && !error.empty(),
            "MIDI conversion accepted " + reason);
    require(sameTempos(destination.tempo.changes(), old_tempos) && destination.tracks.size() == 1 &&
                destination.tracks[0].name == "untouched" && destination.tracks[0].notes.size() == 1 &&
                destination.tracks[0].notes[0].pitch == 42 && destination.tracks[0].notes[0].start == 17,
            "failed tempo conversion changed destination MIDI: " + reason);
    writeText(xml_path, "protected-xml");
    auto report = sentinel;
    require(!daw::writeMusicXmlFile(invalid, xml_path.string(), &error, &report) && !error.empty(),
            "MusicXML export accepted " + reason);
    require(readText(xml_path) == "protected-xml" && report.omitted_midi_events == 11 &&
                report.omitted_note_midi_metadata == 22 && report.omitted_tempo_changes == 33,
            "failed MusicXML export damaged the file or omission report: " + reason);
    writeText(midi_path, "protected-midi");
    require(!daw::writeScoreMidiFile(invalid, midi_path.string(), &error) && readText(midi_path) == "protected-midi",
            "failed score MIDI export damaged the destination: " + reason);
    require(sameScoreProbe(invalid, original), "validation changed source score: " + reason);
  };
  for (const auto& changes : std::vector<std::vector<TempoChange>>{
           {{0, 120.0}}, {{-1, 120.0}}, {{960, 120.0}, {480, 90.0}}, {{480, 90.0}, {480, 120.0}},
           {{480, 0.0}}, {{480, -1.0}}, {{480, std::numeric_limits<double>::infinity()}},
           {{480, std::numeric_limits<double>::quiet_NaN()}}, {{480, 1000000.1}}}) {
    auto invalid = scoreWithNotes();
    invalid.tempo_changes = changes;
    reject(invalid, "invalid later tempo map");
  }
  for (double bpm : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                     std::numeric_limits<double>::quiet_NaN(), 1000000.1}) {
    auto invalid = scoreWithNotes();
    invalid.bpm = bpm;
    reject(invalid, "invalid initial BPM");
  }
  auto boundary = scoreWithNotes();
  boundary.bpm = 1000000.0;
  boundary.tempo_changes = {{1, 1000000.0}};
  require(daw::scoreTempoMap(boundary).changes().size() == 2, "inclusive BPM limit was incorrectly rejected");

  // Only this one bounded fixture exercises the million-change ceiling. Its
  // otherwise valid ordering ensures rejection is genuinely a resource bound.
  auto oversized = scoreWithNotes();
  oversized.tempo_changes.reserve(daw::kMaxScoreTempoChanges + 1);
  for (std::size_t index = 0; index <= daw::kMaxScoreTempoChanges; ++index) {
    oversized.tempo_changes.push_back({static_cast<Tick>(index + 1), 60.0});
  }
  bool rejected_count = false;
  try {
    (void)daw::scoreTempoMap(oversized);
  } catch (const std::invalid_argument&) {
    rejected_count = true;
  }
  require(rejected_count, "tempo map exceeded its documented change-count bound");

  for (bool initial : {false, true}) {
    daw::MidiFile invalid_midi;
    invalid_midi.tempo = daw::TempoMap(initial ? 1000000.1 : 60.0);
    if (!initial) invalid_midi.tempo.addChange(960, 1000000.1);
    invalid_midi.tracks = {{"source", {{0, 1920, 69, 90, 0}}, {}}};
    const auto original_tempos = invalid_midi.tempo.changes();
    auto destination = scoreWithNotes();
    destination.bpm = 81.0;
    destination.tempo_changes = {{111, 91.0}};
    const auto original_score = destination;
    require(!daw::midiToScore(invalid_midi, &destination, &error) && !error.empty(),
            "MIDI to score accepted tempo outside the Score limits");
    require(sameScoreProbe(destination, original_score) && sameTempos(invalid_midi.tempo.changes(), original_tempos),
            "failed MIDI tempo import changed its source or destination");
  }
}

}  // namespace

int main() {
  try {
    const TempDirectory directory;
    mathematicalTimelineAndSound();
    bridgeAndConductor(directory.path);
    tiesAcrossTempoChanges();
    historyPreservesCompleteMap();
    xmlOmissions(directory.path);
    invalidMaps(directory.path);
    std::cout << "classical-daw score tempo tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
