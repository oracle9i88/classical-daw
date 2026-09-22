#include "daw/render.hpp"
#include "daw/score_midi.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

namespace {

int fail(const std::string& message) {
  std::cerr << "FAIL: " << message << '\n';
  return 1;
}

daw::ScoreNote note(daw::Tick start, daw::Tick duration, char step, int octave, std::uint8_t velocity = 100) {
  daw::ScoreNote value;
  value.start = start;
  value.duration = duration;
  value.pitch = {step, 0, octave};
  value.velocity = velocity;
  return value;
}

template <typename Function>
bool throws(Function&& function) {
  try {
    function();
  } catch (const std::exception&) {
    return true;
  }
  return false;
}

}  // namespace

int main() {
  using namespace daw;

  // At 60 BPM, one quarter note is exactly one second. The longest note ends
  // at tick 1920 (two seconds), so the 0.25-second tail makes 108000 frames.
  Score score;
  score.time_signature = {3, 4};
  score.bpm = 60.0;
  ScorePart piano;
  piano.id = "P1";
  piano.name = "Piano";
  piano.measures.push_back(ScoreMeasure{1, 0, {
      note(0, 960, 'C', 4, 100),
      note(960, 960, 'E', 4, 96),
  }});
  ScorePart strings;
  strings.id = "P2";
  strings.name = "Strings";
  strings.measures.push_back(ScoreMeasure{1, 0, {
      note(0, 1920, 'C', 3, 90),
  }});
  score.parts = {piano, strings};

  const AudioBuffer mixed = renderScore(score, 48000.0, 0.25);
  if (mixed.sample_rate != 48000 || mixed.channels != 1 || mixed.frameCount() != 108000) {
    return fail("score render shape or meter-aware duration");
  }
  float peak = 0.0F;
  bool heard = false;
  for (const float sample : mixed.samples) {
    if (!std::isfinite(sample) || sample < -1.0F || sample > 1.0F) {
      return fail("score render produced an invalid sample");
    }
    peak = std::max(peak, std::abs(sample));
    heard = heard || std::abs(sample) > 0.001F;
  }
  if (!heard || peak <= 0.0F) return fail("multi-part score render is silent");

  // The score path must agree with the strict score-to-MIDI bridge and the
  // existing per-track renderer. This checks that parts are summed, rather
  // than only the first part being rendered.
  MidiFile midi;
  std::string error;
  if (!scoreToMidiFile(score, &midi, &error) || midi.tracks.size() != 2) {
    return fail("score render conversion fixture: " + error);
  }
  const AudioBuffer first = renderNotes(midi.tracks[0], midi.tempo, 48000.0, 0.25);
  const AudioBuffer second = renderNotes(midi.tracks[1], midi.tempo, 48000.0, 0.25);
  for (std::size_t index = 0; index < mixed.samples.size(); ++index) {
    const float expected = std::clamp(first.samples[index] + second.samples[index], -1.0F, 1.0F);
    if (std::abs(mixed.samples[index] - expected) > 1.0e-6F) {
      return fail("score render did not sum all parts");
    }
  }

  // Conversion and render configuration errors throw before returning a
  // buffer; the input score itself remains unchanged after every failure.
  const Score unchanged = score;
  Score empty;
  if (!throws([&] { (void)renderScore(empty); })) return fail("empty score was accepted");
  if (!throws([&] { (void)renderScore(score, 0.0); })) return fail("invalid sample rate was accepted");
  if (!throws([&] { (void)renderScore(score, 48000.0, -0.01); })) return fail("invalid tail was accepted");
  Score invalid_note = score;
  invalid_note.parts[0].measures[0].notes[0].duration = 0;
  if (!throws([&] { (void)renderScore(invalid_note); })) return fail("invalid score note was accepted");
  if (score.parts.size() != unchanged.parts.size() || score.parts[0].measures[0].notes.size() !=
                                                      unchanged.parts[0].measures[0].notes.size() ||
      score.bpm != unchanged.bpm || score.time_signature.numerator != unchanged.time_signature.numerator) {
    return fail("failed score render mutated the caller score");
  }

  std::cout << "classical-daw score render tests passed\n";
  return 0;
}
