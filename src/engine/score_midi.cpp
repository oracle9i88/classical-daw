#include "daw/score_midi.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <utility>

namespace daw {
namespace {

int pitchClass(char step) {
  switch (step) {
    case 'C': return 0;
    case 'D': return 2;
    case 'E': return 4;
    case 'F': return 5;
    case 'G': return 7;
    case 'A': return 9;
    case 'B': return 11;
    default: throw std::invalid_argument("score pitch step is out of range");
  }
}

std::uint8_t toMidiPitch(const ScorePitch& pitch) {
  // Use a wide intermediate so even a hostile int octave/alter value cannot
  // wrap before the range check.
  const std::int64_t value = (static_cast<std::int64_t>(pitch.octave) + 1) * 12 +
                             static_cast<std::int64_t>(pitchClass(pitch.step)) +
                             static_cast<std::int64_t>(pitch.alter);
  if (value < 0 || value > 127) throw std::invalid_argument("score pitch is outside MIDI 0..127");
  return static_cast<std::uint8_t>(value);
}

void validateNoteTiming(const ScoreNote& note) {
  if (note.start < 0) throw std::invalid_argument("score note start cannot be negative");
  if (note.duration < 0) throw std::invalid_argument("score note duration cannot be negative");
  if (note.start > std::numeric_limits<Tick>::max() - note.duration) {
    throw std::invalid_argument("score note timing overflows tick range");
  }
  if (note.velocity > 127) throw std::invalid_argument("score note velocity is outside MIDI 0..127");
  if (note.voice == 0 || note.staff == 0) throw std::invalid_argument("score voice/staff numbers must be positive");
}

std::filesystem::path temporaryPath(const std::string& destination) {
  // A sibling path keeps the final rename on one filesystem. This is not used
  // until conversion has succeeded; an existing destination is therefore
  // preserved if the input score is invalid.
  return std::filesystem::path(destination).concat(".tmp");
}

}  // namespace

bool scoreToMidiFile(const Score& score, MidiFile* midi, std::string* error) {
  if (midi == nullptr) {
    if (error) *error = "MIDI output pointer is null";
    return false;
  }
  try {
    if (score.parts.size() != 1) throw std::invalid_argument("MIDI export requires exactly one score part");
    if (score.divisions != kTicksPerQuarter) {
      throw std::invalid_argument("score divisions must be 960 ticks per quarter");
    }
    if (!std::isfinite(score.bpm) || score.bpm <= 0.0) {
      throw std::invalid_argument("score tempo must be finite and positive");
    }

    const ScorePart& part = score.parts.front();
    MidiTrack track;
    track.name = part.name.empty() ? part.id : part.name;
    for (const ScoreMeasure& measure : part.measures) {
      if (measure.start < 0) throw std::invalid_argument("score measure start cannot be negative");
      for (const ScoreNote& note : measure.notes) {
        validateNoteTiming(note);
        if (note.rest) continue;
        track.notes.push_back({note.start, note.duration, toMidiPitch(note.pitch), note.velocity, 0});
      }
    }
    // Keep deterministic ordering for callers inspecting the in-memory
    // result. Equal-start notes are stable, so chords remain independent.
    std::stable_sort(track.notes.begin(), track.notes.end(), [](const MidiNote& left, const MidiNote& right) {
      return left.start < right.start;
    });

    MidiFile converted;
    converted.format = 1;
    converted.ticks_per_quarter = kTicksPerQuarter;
    converted.tempo = TempoMap(score.bpm);
    converted.tracks.push_back(std::move(track));
    *midi = std::move(converted);
    return true;
  } catch (const std::exception& exception) {
    if (error) *error = exception.what();
    return false;
  }
}

bool writeScoreMidiFile(const Score& score, const std::string& path, std::string* error) {
  if (path.empty()) {
    if (error) *error = "MIDI output path is empty";
    return false;
  }
  MidiFile converted;
  std::string conversion_error;
  if (!scoreToMidiFile(score, &converted, &conversion_error)) {
    if (error) *error = conversion_error;
    return false;
  }

  const std::filesystem::path destination(path);
  const std::filesystem::path temporary = temporaryPath(path);
  std::error_code cleanup_error;
  std::filesystem::remove(temporary, cleanup_error);
  std::string write_error;
  if (!writeMidiFile(converted, temporary.string(), &write_error)) {
    std::filesystem::remove(temporary, cleanup_error);
    if (error) *error = write_error;
    return false;
  }
  std::error_code rename_error;
  std::filesystem::rename(temporary, destination, rename_error);
  if (rename_error) {
    std::filesystem::remove(temporary, cleanup_error);
    if (error) *error = "failed to finalize MIDI output: " + rename_error.message();
    return false;
  }
  return true;
}

}  // namespace daw
