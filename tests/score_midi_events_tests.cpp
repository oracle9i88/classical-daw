#include "daw/score_midi.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

bool equalEvents(const std::vector<daw::MidiChannelEvent>& left,
                 const std::vector<daw::MidiChannelEvent>& right) {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    const auto& a = left[index];
    const auto& b = right[index];
    if (std::tie(a.tick, a.type, a.channel, a.data1, a.data2, a.order) !=
        std::tie(b.tick, b.type, b.channel, b.data1, b.data2, b.order)) return false;
  }
  return true;
}

bool equalNotes(std::vector<daw::MidiNote> left, std::vector<daw::MidiNote> right) {
  if (left.size() != right.size()) return false;
  const auto fields = [](const daw::MidiNote& note) {
    return std::tie(note.start, note.duration, note.pitch, note.velocity, note.channel,
                    note.release_velocity, note.on_order, note.off_order);
  };
  const auto less = [&fields](const daw::MidiNote& a, const daw::MidiNote& b) { return fields(a) < fields(b); };
  std::sort(left.begin(), left.end(), less);
  std::sort(right.begin(), right.end(), less);
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (fields(left[index]) != fields(right[index])) return false;
  }
  return true;
}

daw::Score sentinelScore() {
  daw::Score score;
  daw::ScoreNote note;
  note.start = 123;
  note.duration = 456;
  note.midi_channel = 4;
  note.midi_on_order = 17;
  note.midi_off_order = 19;
  note.midi_release_velocity = 37;
  score.parts = {daw::ScorePart{"sentinel", "untouched score", {{1, 0, {note}}},
                                 {{321, daw::MidiChannelEventType::ControlChange, 4, 64, 127, 18}}}};
  return score;
}

bool isSentinel(const daw::Score& score) {
  if (score.parts.size() != 1 || score.parts[0].name != "untouched score" ||
      score.parts[0].measures.size() != 1 || score.parts[0].measures[0].notes.size() != 1) return false;
  const daw::ScoreNote& note = score.parts[0].measures[0].notes[0];
  return note.start == 123 && note.duration == 456 && note.midi_channel == 4 &&
         note.midi_on_order == 17 && note.midi_off_order == 19 && note.midi_release_velocity == 37 &&
         equalEvents(score.parts[0].midi_events, sentinelScore().parts[0].midi_events);
}

daw::MidiFile sentinelMidi() {
  daw::MidiFile midi;
  midi.tracks = {daw::MidiTrack{"untouched MIDI", {{123, 456, 67, 81, 4, 37, 17, 19}},
                               {{321, daw::MidiChannelEventType::ControlChange, 4, 64, 127, 18}}}};
  return midi;
}

bool isSentinel(const daw::MidiFile& midi) {
  const daw::MidiFile sentinel = sentinelMidi();
  return midi.tracks.size() == 1 && midi.tracks[0].name == "untouched MIDI" &&
         equalNotes(midi.tracks[0].notes, sentinel.tracks[0].notes) &&
         equalEvents(midi.tracks[0].channel_events, sentinel.tracks[0].channel_events);
}

void testEventOnlyTracks() {
  using namespace daw;
  MidiFile original;
  // Late control data must survive without allocating millions of empty bars.
  const std::vector<MidiChannelEvent> controls = {
      {0, MidiChannelEventType::ProgramChange, 11, 42, 0, 1},
      {std::numeric_limits<Tick>::max(), MidiChannelEventType::ControlChange, 11, 64, 0, 2}};
  original.tracks = {MidiTrack{"Conductor", {}, {}}, MidiTrack{"Controls", {}, controls},
                     MidiTrack{"Notes", {{0, 960, 60, 92, 11, 35, 3, 4}}, {}},
                     MidiTrack{"Trailing empty", {}, {}}};
  Score score;
  MidiFile result;
  std::string error;
  require(midiToScore(original, &score, &error), "event-only import: " + error);
  require(score.parts.size() == 2 && score.parts[0].name == "Controls" &&
              score.parts[0].measures.size() == 1 && score.parts[0].measures[0].number == 1 &&
              score.parts[0].measures[0].start == 0 && score.parts[0].measures[0].notes.empty(),
          "event-only track was dropped or allocated a controller-length measure grid");
  require(equalEvents(score.parts[0].midi_events, controls), "score changed event-only track data");
  require(scoreToMidiFile(score, &result, &error), "event-only export: " + error);
  require(result.tracks.size() == 2 && result.tracks[0].notes.empty() &&
              equalEvents(result.tracks[0].channel_events, controls) &&
              equalNotes(result.tracks[1].notes, original.tracks[2].notes),
          "event-only score round-trip lost events or shifted note channels after omitting conductor");

  original.tracks = {MidiTrack{"Controls alone", {}, controls}};
  require(midiToScore(original, &score, &error) && score.parts.size() == 1 &&
              scoreToMidiFile(score, &result, &error) && equalEvents(result.tracks[0].channel_events, controls),
          "MIDI containing only channel events failed: " + error);
}

void testMultichannelAndTies() {
  using namespace daw;
  MidiFile original;
  original.format = 0;
  original.tracks = {MidiTrack{"Multichannel", {
      {3360, 8640, 60, 83, 2, 67, 5, 14},
      {3360, 960, 60, 101, 9, 31, 6, 8},
      {0, 480, 64, 76, 15, 22, 2, 4}}, {
      {0, MidiChannelEventType::ProgramChange, 2, 40, 0, 1},
      {0, MidiChannelEventType::ControlChange, 15, 64, 127, 3},
      {3360, MidiChannelEventType::PitchBend, 2, 1, 65, 7},
      {4320, MidiChannelEventType::PolyPressure, 9, 60, 70, 9},
      {4320, MidiChannelEventType::ChannelPressure, 2, 49, 0, 10},
      {4320, MidiChannelEventType::ControlChange, 15, 64, 0, 11}}}};
  Score score;
  MidiFile result;
  std::string error;
  require(midiToScore(original, &score, &error), "Type 0 multichannel import: " + error);
  require(score.parts.size() == 1 && score.parts[0].measures.size() == 4,
          "multichannel import lost its track or measure layout");
  std::size_t long_segments = 0;
  for (const ScoreMeasure& measure : score.parts[0].measures) {
    for (const ScoreNote& note : measure.notes) {
      require(note.midi_channel == static_cast<int>(note.voice) - 1, "score lost an explicit input MIDI channel");
      if (note.midi_channel != 2) continue;
      ++long_segments;
      require(note.midi_on_order == (note.tie_stop ? 0U : 5U) &&
                  note.midi_off_order == (note.tie_start ? 0U : 14U) &&
                  note.midi_release_velocity == (note.tie_start ? 0 : 67),
              "split tie copied source note events onto intermediate segments");
    }
  }
  require(long_segments == 4, "fixture did not exercise all tied note segments");
  require(scoreToMidiFile(score, &result, &error), "Type 0 multichannel export: " + error);
  require(result.tracks.size() == 1 && equalNotes(result.tracks[0].notes, original.tracks[0].notes) &&
              equalEvents(result.tracks[0].channel_events, original.tracks[0].channel_events),
          "Type 0 multichannel round-trip changed channels, release velocities, or source order");

  // Two tied notes can share staff, voice and pitch when their routes differ.
  // Use different first/last metadata to prove merge selects the right ends.
  ScoreNote a;
  a.duration = 960;
  a.tie_start = true;
  a.midi_channel = 3;
  a.midi_on_order = 11;
  a.midi_off_order = 12;
  a.midi_release_velocity = 1;
  ScoreNote b = a;
  b.start = 960;
  b.tie_start = false;
  b.tie_stop = true;
  b.velocity = 20;
  b.midi_on_order = 99;
  b.midi_off_order = 17;
  b.midi_release_velocity = 43;
  ScoreNote c = a;
  c.midi_channel = 7;
  c.midi_on_order = 13;
  ScoreNote d = b;
  d.midi_channel = 7;
  d.midi_off_order = 18;
  score.parts = {ScorePart{"P1", "Routed ties", {{1, 0, {a, c, b, d}}}, {}}};
  require(scoreToMidiFile(score, &result, &error), "parallel routed ties: " + error);
  require(equalNotes(result.tracks[0].notes, {{0, 1920, 60, 100, 3, 43, 11, 17},
                                              {0, 1920, 60, 100, 7, 43, 13, 18}}),
          "tie merge crossed output channels or lost first-on/last-off metadata");
  score.parts[0].measures[0].notes = {a, d};
  result = sentinelMidi();
  require(!scoreToMidiFile(score, &result, &error) && error.find("tie") != std::string::npos && isSentinel(result),
          "tie crossing MIDI channels was accepted or changed output");
}

void testMalformedMetadata() {
  using namespace daw;
  const std::vector<MidiChannelEvent> malformed = {
      {-1, MidiChannelEventType::ControlChange, 0, 64, 127, 1},
      {0, MidiChannelEventType::ControlChange, 16, 64, 127, 1},
      {0, MidiChannelEventType::ControlChange, 0, 128, 127, 1},
      {0, MidiChannelEventType::ControlChange, 0, 64, 128, 1},
      {0, MidiChannelEventType::ProgramChange, 0, 40, 1, 1},
      {0, MidiChannelEventType::ChannelPressure, 0, 40, 1, 1},
      {0, static_cast<MidiChannelEventType>(0x90), 0, 60, 100, 1}};
  std::string error;
  for (const MidiChannelEvent& event : malformed) {
    MidiFile source = sentinelMidi();
    // The earlier valid track ensures a partial conversion cannot leak out.
    source.tracks.push_back(MidiTrack{"Malformed events", {}, {event}});
    Score score = sentinelScore();
    require(!midiToScore(source, &score, &error) && error.find("event") != std::string::npos && isSentinel(score),
            "invalid imported channel event was accepted or changed score");
    Score invalid = sentinelScore();
    invalid.parts.push_back(ScorePart{"P2", "Malformed events", {}, {event}});
    MidiFile result = sentinelMidi();
    require(!scoreToMidiFile(invalid, &result, &error) && error.find("event") != std::string::npos && isSentinel(result),
            "invalid score channel event was accepted or changed MIDI");
  }
  for (const int channel : {-2, 16}) {
    Score invalid = sentinelScore();
    invalid.parts[0].measures[0].notes[0].midi_channel = static_cast<std::int16_t>(channel);
    MidiFile result = sentinelMidi();
    require(!scoreToMidiFile(invalid, &result, &error) && isSentinel(result),
            "invalid score note channel was accepted or changed MIDI");
  }
  Score invalid = sentinelScore();
  invalid.parts[0].measures[0].notes[0].midi_release_velocity = 128;
  MidiFile result = sentinelMidi();
  require(!scoreToMidiFile(invalid, &result, &error) && isSentinel(result),
          "invalid score release velocity was accepted or changed MIDI");
  MidiFile source = sentinelMidi();
  source.tracks[0].notes[0].release_velocity = 128;
  Score score = sentinelScore();
  require(!midiToScore(source, &score, &error) && isSentinel(score),
          "invalid MIDI release velocity was accepted or changed score");
}

void testManyPartsAndDefaultChannels() {
  using namespace daw;
  MidiFile original;
  for (std::size_t index = 0; index < 20; ++index) {
    const auto channel = static_cast<std::uint8_t>(index % 16);
    original.tracks.push_back(MidiTrack{"Track " + std::to_string(index + 1),
        {{0, 480, 60, 90, channel, 30, 2, 3}}, {{0, MidiChannelEventType::ProgramChange, channel, 40, 0, 1}}});
  }
  std::string error;
  Score score;
  MidiFile result;
  require(midiToScore(original, &score, &error) && scoreToMidiFile(score, &result, &error),
          "more than 16 explicitly routed imported tracks failed: " + error);
  require(result.tracks.size() == original.tracks.size(), "large arrangement lost tracks");
  for (std::size_t index = 0; index < original.tracks.size(); ++index) {
    require(equalNotes(result.tracks[index].notes, original.tracks[index].notes) &&
                equalEvents(result.tracks[index].channel_events, original.tracks[index].channel_events),
            "large arrangement changed explicit routing");
  }
  for (const std::size_t index : {std::size_t{0}, std::size_t{16}}) {
    Score unrouted = score;
    unrouted.parts[index].measures[0].notes[0].midi_channel = -1;
    result = sentinelMidi();
    require(!scoreToMidiFile(unrouted, &result, &error) && isSentinel(result),
            "large arrangement with an implicit route was accepted or changed output");
  }
  for (ScorePart& part : score.parts) part.measures[0].notes.clear();
  require(scoreToMidiFile(score, &result, &error) && result.tracks.size() == 20 &&
              result.tracks.back().notes.empty() && result.tracks.back().channel_events[0].channel == 3,
          "more than 16 event-only routed parts failed: " + error);
  Score bare;
  bare.parts.resize(17);
  result = sentinelMidi();
  require(!scoreToMidiFile(bare, &result, &error) && isSentinel(result),
          "legacy unallocated bare part guard changed");

  Score authored;
  authored.parts = {ScorePart{"P1", "Default 0", {{1, 0, {ScoreNote{}}}}, {}},
                    ScorePart{"P2", "Default 1", {{1, 0, {ScoreNote{}}}}, {}}};
  require(scoreToMidiFile(authored, &result, &error) && result.tracks[0].notes[0].channel == 0 &&
              result.tracks[1].notes[0].channel == 1,
          "newly authored notes lost the default part-index channel mapping");
  authored.parts[1].measures[0].notes[0].midi_channel = 9;
  require(scoreToMidiFile(authored, &result, &error) && result.tracks[1].notes[0].channel == 9,
          "explicit note channel did not override the default part route");
}

}  // namespace

int main() {
  try {
    testEventOnlyTracks();
    testMultichannelAndTies();
    testMalformedMetadata();
    testManyPartsAndDefaultChannels();
    std::cout << "classical-daw score MIDI event tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
