#pragma once

#include "daw/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace daw {

struct ScorePitch {
  char step = 'C';
  int alter = 0;
  int octave = 4;
};

struct ScoreNote {
  Tick start = 0;
  Tick duration = kTicksPerQuarter;
  ScorePitch pitch{};
  bool rest = false;
  bool chord = false;
  bool tie_start = false;
  bool tie_stop = false;
  std::uint8_t velocity = 100;
};

struct ScoreMeasure {
  int number = 1;
  Tick start = 0;
  std::vector<ScoreNote> notes;
};

struct ScorePart {
  std::string id = "P1";
  std::string name = "Part 1";
  std::vector<ScoreMeasure> measures;
};

// Internal score state uses the engine's fixed 960 PPQ tick domain. MusicXML
// is an interchange format at the boundary; it is not the realtime state.
struct Score {
  Tick divisions = kTicksPerQuarter;
  TimeSignature time_signature{};
  double bpm = 120.0;
  std::vector<ScorePart> parts;
};

// This first vertical slice intentionally supports one part and one voice.
// It preserves pitch, integer duration, rests, chords, ties, meter, and tempo.
bool writeMusicXmlFile(const Score& score, const std::string& path, std::string* error = nullptr);
bool readMusicXmlFile(const std::string& path, Score* score, std::string* error = nullptr);

}  // namespace daw

