#include "daw/midi.hpp"
#include "daw/render.hpp"
#include "daw/score.hpp"
#include "daw/timeline.hpp"
#include "daw/wav.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

bool closeEnough(double a, double b, double epsilon = 1e-9) {
  return std::abs(a - b) <= epsilon;
}

int fail(const std::string& message) {
  std::cerr << "FAIL: " << message << '\n';
  return 1;
}

bool writeBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream output(path, std::ios::binary);
  if (!output) return false;
  output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(output);
}

bool writeText(const std::filesystem::path& path, const std::string& text) {
  std::ofstream output(path, std::ios::binary);
  if (!output) return false;
  output << text;
  return static_cast<bool>(output);
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

  // Regression fixture: one-byte channel messages must not consume the next
  // event's first byte. It also exercises running status and an ignored SysEx
  // plus text-meta event before the note.
  const auto boundary_path = temp / "classical_daw_midi_boundaries.mid";
  const std::vector<std::uint8_t> boundary_midi = {
      'M', 'T', 'h', 'd', 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x01, 0x03, 0xc0,
      'M', 'T', 'r', 'k', 0x00, 0x00, 0x00, 0x20,
      0x00, 0xc0, 0x05,             // Program Change (one data byte)
      0x00, 0x07,                   // running Program Change
      0x00, 0xd0, 0x40,             // Channel Pressure (one data byte)
      0x00, 0xf0, 0x03, 0x01, 0x02, 0xf7,  // ignored SysEx
      0x00, 0xff, 0x01, 0x01, 0x41,        // ignored text meta
      0x00, 0x90, 0x3c, 0x64,       // note on
      0x87, 0x40, 0x80, 0x3c, 0x00, // 960-tick note off
      0x00, 0xff, 0x2f, 0x00};
  if (!writeBytes(boundary_path, boundary_midi)) return fail("MIDI boundary fixture write");
  MidiFile boundary_file;
  if (!readMidiFile(boundary_path.string(), &boundary_file, &error)) {
    return fail("MIDI channel-message boundary read: " + error);
  }
  if (boundary_file.tracks.size() != 1 || boundary_file.tracks[0].notes.size() != 1 ||
      boundary_file.tracks[0].notes[0].start != 0 || boundary_file.tracks[0].notes[0].duration != 960) {
    return fail("MIDI channel-message boundary parsing");
  }

  const auto truncated_path = temp / "classical_daw_truncated_program_change.mid";
  const std::vector<std::uint8_t> truncated_midi = {
      'M', 'T', 'h', 'd', 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x01, 0x03, 0xc0,
      'M', 'T', 'r', 'k', 0x00, 0x00, 0x00, 0x02, 0x00, 0xc0};
  if (!writeBytes(truncated_path, truncated_midi)) return fail("MIDI truncated fixture write");
  if (readMidiFile(truncated_path.string(), &boundary_file, &error)) {
    return fail("truncated MIDI channel event was accepted");
  }

  const auto format_zero_path = temp / "classical_daw_format_zero.mid";
  MidiFile invalid_format_zero;
  invalid_format_zero.format = 0;
  invalid_format_zero.tracks = original.tracks;
  if (writeMidiFile(invalid_format_zero, format_zero_path.string(), &error)) {
    return fail("format 0 accepted multiple tracks");
  }

  const auto invalid_tempo_path = temp / "classical_daw_invalid_tempo.mid";
  MidiFile invalid_tempo;
  invalid_tempo.tempo = TempoMap(1.0);  // 60,000,000 microseconds does not fit 24 bits.
  invalid_tempo.tracks = {MidiTrack{"Piano", {}}};
  if (writeMidiFile(invalid_tempo, invalid_tempo_path.string(), &error)) {
    return fail("out-of-range MIDI tempo was accepted");
  }

  const auto overflow_path = temp / "classical_daw_overflow_note.mid";
  MidiFile overflow_note;
  overflow_note.tracks = {MidiTrack{"Piano", {{std::numeric_limits<Tick>::max(), 1, 60, 80, 0}}}};
  if (writeMidiFile(overflow_note, overflow_path.string(), &error)) {
    return fail("overflowing MIDI note duration was accepted");
  }

  const auto musicxml_path = temp / "classical_daw_score_roundtrip.musicxml";
  Score score;
  score.time_signature = {4, 4};
  score.bpm = 96.0;
  score.parts = {ScorePart{"P1", "Piano", {
      ScoreMeasure{1, 0, {
          ScoreNote{0, 960, ScorePitch{'C', 0, 4}, false, false, true, false, 100},
          ScoreNote{960, 960, ScorePitch{'E', 0, 4}, false, false, false, false, 100},
          ScoreNote{1920, 1920, ScorePitch{'G', 0, 4}, false, false, false, true, 100},
          ScoreNote{1920, 1920, ScorePitch{'C', 0, 5}, false, true, false, false, 100},
      }},
  }}};
  if (!writeMusicXmlFile(score, musicxml_path.string(), &error)) return fail("MusicXML write: " + error);
  Score parsed_score;
  if (!readMusicXmlFile(musicxml_path.string(), &parsed_score, &error)) return fail("MusicXML read: " + error);
  if (parsed_score.parts.size() != 1 || parsed_score.parts[0].name != "Piano" ||
      parsed_score.parts[0].measures.size() != 1 || parsed_score.parts[0].measures[0].notes.size() != 4 ||
      parsed_score.parts[0].measures[0].notes[0].start != 0 ||
      parsed_score.parts[0].measures[0].notes[1].start != 960 ||
      parsed_score.parts[0].measures[0].notes[2].tie_stop != true ||
      parsed_score.parts[0].measures[0].notes[3].chord != true) {
    return fail("MusicXML score round-trip");
  }
  if (parsed_score.time_signature.numerator != 4 || parsed_score.time_signature.denominator != 4 ||
      !closeEnough(parsed_score.bpm, 96.0)) {
    return fail("MusicXML score metadata round-trip");
  }

  const auto invalid_musicxml_path = temp / "classical_daw_invalid.musicxml";
  const std::string invalid_musicxml =
      "<score-partwise><part-list><score-part id=\"P1\"><part-name>Piano</part-name></score-part></part-list>"
      "<part id=\"P1\"><measure number=\"1\"><attributes><divisions>480</divisions></attributes>"
      "</measure></part></score-partwise>";
  if (!writeText(invalid_musicxml_path, invalid_musicxml)) return fail("invalid MusicXML fixture write");
  if (readMusicXmlFile(invalid_musicxml_path.string(), &parsed_score, &error)) {
    return fail("unsupported MusicXML divisions were accepted");
  }

  const auto trailing_musicxml_path = temp / "classical_daw_trailing.musicxml";
  if (!writeText(trailing_musicxml_path, "<score-partwise></score-partwise><garbage/>")) {
    return fail("trailing MusicXML fixture write");
  }
  if (readMusicXmlFile(trailing_musicxml_path.string(), &parsed_score, &error)) {
    return fail("trailing MusicXML data was accepted");
  }

  const auto prefix_musicxml_path = temp / "classical_daw_prefix.musicxml";
  if (!writeText(prefix_musicxml_path, "<garbage/><score-partwise></score-partwise>")) {
    return fail("prefix MusicXML fixture write");
  }
  if (readMusicXmlFile(prefix_musicxml_path.string(), &parsed_score, &error)) {
    return fail("prefix MusicXML data was accepted");
  }

  const AudioBuffer rendered = renderNotes(original.tracks[0], tempo, 48000.0, 0.05);
  if (rendered.sample_rate != 48000 || rendered.channels != 1 || rendered.frameCount() < 48000) {
    return fail("offline render shape");
  }
  if (!writeWavPcm16(rendered, wav_path.string(), &error)) return fail("WAV write: " + error);
  std::ifstream wav(wav_path, std::ios::binary | std::ios::ate);
  if (!wav || wav.tellg() <= 44) return fail("WAV file missing");

  std::filesystem::remove(midi_path);
  std::filesystem::remove(boundary_path);
  std::filesystem::remove(truncated_path);
  std::filesystem::remove(format_zero_path);
  std::filesystem::remove(invalid_tempo_path);
  std::filesystem::remove(overflow_path);
  std::filesystem::remove(musicxml_path);
  std::filesystem::remove(invalid_musicxml_path);
  std::filesystem::remove(trailing_musicxml_path);
  std::filesystem::remove(prefix_musicxml_path);
  std::filesystem::remove(wav_path);
  std::cout << "classical-daw Alpha engine tests passed\n";
  return 0;
}
