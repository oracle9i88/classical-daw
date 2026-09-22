#include "daw/midi.hpp"
#include "daw/render.hpp"
#include "daw/realtime.hpp"
#include "daw/score.hpp"
#include "daw/score_midi.hpp"
#include "daw/timeline.hpp"
#include "daw/wav.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
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

bool readText(const std::filesystem::path& path, std::string* text) {
  if (text == nullptr) return false;
  std::ifstream input(path, std::ios::binary);
  if (!input) return false;
  *text = std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  return static_cast<bool>(input) || input.eof();
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
  score.parts[0].measures[0].notes[0].lyric = "C & D";
  if (!writeMusicXmlFile(score, musicxml_path.string(), &error)) return fail("MusicXML write: " + error);
  std::string musicxml_text;
  if (!readText(musicxml_path, &musicxml_text) || musicxml_text.find("<lyric><text>C &amp; D</text></lyric>") == std::string::npos) {
    return fail("MusicXML lyric escaping");
  }
  Score parsed_score;
  if (!readMusicXmlFile(musicxml_path.string(), &parsed_score, &error)) return fail("MusicXML read: " + error);
  if (parsed_score.parts.size() != 1 || parsed_score.parts[0].name != "Piano" ||
      parsed_score.parts[0].measures.size() != 1 || parsed_score.parts[0].measures[0].notes.size() != 4 ||
      parsed_score.parts[0].measures[0].notes[0].start != 0 ||
      parsed_score.parts[0].measures[0].notes[1].start != 960 ||
      parsed_score.parts[0].measures[0].notes[2].tie_stop != true ||
      parsed_score.parts[0].measures[0].notes[3].chord != true ||
      parsed_score.parts[0].measures[0].notes[0].lyric != "C & D") {
    return fail("MusicXML score round-trip");
  }
  if (parsed_score.time_signature.numerator != 4 || parsed_score.time_signature.denominator != 4 ||
      !closeEnough(parsed_score.bpm, 96.0)) {
    return fail("MusicXML score metadata round-trip");
  }

  const auto multi_voice_musicxml_path = temp / "classical_daw_multi_voice.musicxml";
  Score multi_voice_score = score;
  ScoreNote tuplet_note{0, 640, ScorePitch{'D', 0, 5}, false, false, false, false, 90};
  tuplet_note.voice = 2;
  tuplet_note.tuplet_actual = 3;
  tuplet_note.tuplet_normal = 2;
  multi_voice_score.parts[0].measures[0].notes.push_back(tuplet_note);
  if (!writeMusicXmlFile(multi_voice_score, multi_voice_musicxml_path.string(), &error)) {
    return fail("multi-voice MusicXML write: " + error);
  }
  Score parsed_multi_voice;
  if (!readMusicXmlFile(multi_voice_musicxml_path.string(), &parsed_multi_voice, &error)) {
    return fail("multi-voice MusicXML read: " + error);
  }
  bool found_tuplet = false;
  for (const ScoreNote& note : parsed_multi_voice.parts[0].measures[0].notes) {
    if (note.voice == 2) {
      found_tuplet = note.start == 0 && note.duration == 640 && note.tuplet_actual == 3 && note.tuplet_normal == 2;
    }
  }
  if (!found_tuplet) return fail("multi-voice and tuplet MusicXML round-trip");

  const auto multi_part_musicxml_path = temp / "classical_daw_multi_part.musicxml";
  Score multi_part_score = multi_voice_score;
  multi_part_score.parts.push_back(ScorePart{"P2", "Cello", {
      ScoreMeasure{1, 0, {
          ScoreNote{0, 1920, ScorePitch{'C', 0, 3}, false, false, false, false, 82},
      }},
  }});
  multi_part_score.parts[1].measures[0].notes[0].lyric = "bass";
  if (!writeMusicXmlFile(multi_part_score, multi_part_musicxml_path.string(), &error)) {
    return fail("multi-part MusicXML write: " + error);
  }
  if (!readMusicXmlFile(multi_part_musicxml_path.string(), &parsed_multi_voice, &error)) {
    return fail("multi-part MusicXML read: " + error);
  }
  if (parsed_multi_voice.parts.size() != 2 || parsed_multi_voice.parts[0].id != "P1" ||
      parsed_multi_voice.parts[0].name != "Piano" || parsed_multi_voice.parts[1].id != "P2" ||
      parsed_multi_voice.parts[1].name != "Cello" || parsed_multi_voice.parts[1].measures.size() != 1 ||
      parsed_multi_voice.parts[1].measures[0].start != 0 ||
      parsed_multi_voice.parts[1].measures[0].notes.size() != 1 ||
      parsed_multi_voice.parts[1].measures[0].notes[0].pitch.octave != 3 ||
      parsed_multi_voice.parts[1].measures[0].notes[0].lyric != "bass") {
    return fail("multi-part MusicXML score round-trip");
  }

  const auto score_midi_path = temp / "classical_daw_score_export.mid";
  MidiFile score_midi;
  if (!scoreToMidiFile(multi_voice_score, &score_midi, &error)) {
    return fail("score to MIDI conversion: " + error);
  }
  if (score_midi.tracks.size() != 1 || score_midi.tracks[0].notes.size() != 5 ||
      score_midi.tracks[0].notes[0].pitch != 60 || score_midi.tracks[0].notes[0].channel != 0 ||
      score_midi.tracks[0].notes[0].duration != 960 ||
      score_midi.tempo.changes().size() != 1 || !closeEnough(score_midi.tempo.changes()[0].bpm, 96.0)) {
    return fail("score to MIDI note/tempo mapping");
  }
  if (!writeScoreMidiFile(multi_voice_score, score_midi_path.string(), &error)) {
    return fail("score MIDI file write: " + error);
  }
  MidiFile exported_score_midi;
  if (!readMidiFile(score_midi_path.string(), &exported_score_midi, &error) ||
      exported_score_midi.tracks.size() != 1 || exported_score_midi.tracks[0].notes.size() != 5) {
    return fail("score MIDI file round-trip: " + error);
  }

  const auto multi_part_score_midi_path = temp / "classical_daw_multi_part_score_export.mid";
  MidiFile multi_part_score_midi;
  if (!scoreToMidiFile(multi_part_score, &multi_part_score_midi, &error)) {
    return fail("multi-part score to MIDI conversion: " + error);
  }
  if (multi_part_score_midi.format != 1 || multi_part_score_midi.tracks.size() != 2 ||
      multi_part_score_midi.tracks[0].name != "Piano" || multi_part_score_midi.tracks[1].name != "Cello" ||
      multi_part_score_midi.tracks[0].notes.size() != 5 || multi_part_score_midi.tracks[1].notes.size() != 1 ||
      multi_part_score_midi.tracks[0].notes[0].channel != 0 || multi_part_score_midi.tracks[1].notes[0].channel != 1 ||
      multi_part_score_midi.tracks[1].notes[0].pitch != 48 ||
      multi_part_score_midi.tempo.changes().size() != 1 ||
      !closeEnough(multi_part_score_midi.tempo.changes()[0].bpm, 96.0)) {
    return fail("multi-part score MIDI track/channel mapping");
  }
  if (!writeScoreMidiFile(multi_part_score, multi_part_score_midi_path.string(), &error)) {
    return fail("multi-part score MIDI file write: " + error);
  }
  MidiFile exported_multi_part_score_midi;
  if (!readMidiFile(multi_part_score_midi_path.string(), &exported_multi_part_score_midi, &error) ||
      exported_multi_part_score_midi.tracks.size() != 2 ||
      exported_multi_part_score_midi.tracks[0].name != "Piano" ||
      exported_multi_part_score_midi.tracks[1].name != "Cello" ||
      exported_multi_part_score_midi.tracks[0].notes.size() != 5 ||
      exported_multi_part_score_midi.tracks[1].notes.size() != 1 ||
      exported_multi_part_score_midi.tracks[1].notes[0].channel != 1) {
    return fail("multi-part score MIDI file round-trip: " + error);
  }
  const auto protected_midi_path = temp / "classical_daw_score_export_protected.mid";
  if (!writeText(protected_midi_path, "keep-existing-file")) return fail("protected MIDI fixture write");
  Score too_many_parts = score;
  too_many_parts.parts.clear();
  for (int index = 0; index < 17; ++index) {
    too_many_parts.parts.push_back(ScorePart{"P" + std::to_string(index + 1), "Part " + std::to_string(index + 1), {}});
  }
  if (writeScoreMidiFile(too_many_parts, protected_midi_path.string(), &error)) {
    return fail("score MIDI export accepted more than 16 parts");
  }
  std::string protected_contents;
  if (!readText(protected_midi_path, &protected_contents) || protected_contents != "keep-existing-file") {
    return fail("failed score MIDI conversion damaged destination");
  }
  Score zero_duration_score = multi_voice_score;
  zero_duration_score.parts[0].measures[0].notes[0].duration = 0;
  if (scoreToMidiFile(zero_duration_score, &score_midi, &error)) {
    return fail("zero-duration score MIDI note was accepted");
  }

  SpscRing<int, 2> ring;
  if (!ring.push(10) || !ring.push(20) || ring.push(30) || ring.approximateSize() != 2) {
    return fail("SPSC queue capacity");
  }
  int value = 0;
  if (!ring.pop(&value) || value != 10 || !ring.pop(&value) || value != 20 || ring.pop(&value)) {
    return fail("SPSC queue ordering");
  }

  BlockScheduler scheduler;
  if (!scheduler.enqueue({TransportCommandType::SeekSamples, 100, 120.0}) ||
      !scheduler.enqueue({TransportCommandType::SetTempo, 0, 90.0}) ||
      !scheduler.enqueue({TransportCommandType::Start, 0, 0.0})) {
    return fail("transport command enqueue");
  }
  scheduler.recordXrun();
  scheduler.processBlock(256);
  const TransportSnapshot running = scheduler.snapshot();
  if (!running.running || running.sample_position != 356 || !closeEnough(running.bpm, 90.0) || running.xrun_count != 1) {
    return fail("transport block scheduling");
  }
  if (!scheduler.enqueue({TransportCommandType::Stop, 0, 0.0})) return fail("transport stop enqueue");
  scheduler.processBlock(256);
  if (scheduler.snapshot().running || scheduler.snapshot().sample_position != 356) return fail("transport stop");

  static_assert(std::is_nothrow_destructible_v<InstrumentRenderer>);
  static_assert(noexcept(std::declval<InstrumentRenderer&>().prepare(std::declval<const InstrumentRenderConfig&>())));
  static_assert(noexcept(std::declval<InstrumentRenderer&>().reset()));
  static_assert(noexcept(std::declval<InstrumentRenderer&>().enqueue(std::declval<const VoiceEvent&>())));
  static_assert(noexcept(std::declval<InstrumentRenderer&>().render(nullptr, 0, 0, 0.0)));

  SineVoiceBank synth;
  InstrumentRenderer& renderer = synth;
  const InstrumentRenderConfig render_config{48000.0, 64, 2};
  if (!renderer.prepare(render_config) || !synth.prepared()) return fail("instrument prepare");
  if (synth.preparedConfig().max_frames_per_block != 64 || synth.preparedConfig().channels != 2) {
    return fail("instrument prepare config");
  }
  if (!renderer.enqueue({VoiceEventType::NoteOn, 69, 100})) return fail("sine voice enqueue");
  std::array<float, 128> audio{};
  // The renderer owns all voice/event storage. This callback-shaped call is
  // noexcept and performs no allocation, locking, or I/O by contract.
  renderer.render(audio.data(), 64, 2, 48000.0);
  float peak = 0.0F;
  for (const float sample : audio) peak = std::max(peak, std::abs(sample));
  if (peak <= 0.0F) return fail("sine voice render");
  if (!renderer.enqueue({VoiceEventType::NoteOff, 69, 0})) return fail("sine voice off enqueue");
  renderer.reset();
  if (synth.prepared()) return fail("instrument reset lifecycle");
  audio.fill(1.0F);
  renderer.render(audio.data(), 64, 2, 48000.0);
  for (const float sample : audio) {
    if (sample != 0.0F) return fail("instrument reset did not clear voices");
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
  std::filesystem::remove(multi_voice_musicxml_path);
  std::filesystem::remove(multi_part_musicxml_path);
  std::filesystem::remove(score_midi_path);
  std::filesystem::remove(multi_part_score_midi_path);
  std::filesystem::remove(protected_midi_path);
  std::filesystem::remove(invalid_musicxml_path);
  std::filesystem::remove(trailing_musicxml_path);
  std::filesystem::remove(prefix_musicxml_path);
  std::filesystem::remove(wav_path);
  std::cout << "classical-daw Alpha engine tests passed\n";
  return 0;
}
