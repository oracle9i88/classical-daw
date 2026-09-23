#pragma once
#include "daw/score.hpp"
#include "daw/midi_sequence.hpp"
#include <optional>
#include <string>
#include <vector>

namespace daw {
struct NotePerformance {
  std::uint64_t note_id = 0; // Performed-note ID, NOT a notation element ID.
  double onset_seconds = 0; // Offset from notated onset AFTER tempo conversion.
  double duration_scale = 1;
  int velocity = -1; // -1 inherits notated/imported velocity; otherwise 1..127.
};
struct PerformedNoteMapping {
  std::uint64_t id = 0;
  std::vector<std::uint64_t> notation_ids;
};
struct CurvePoint { std::uint64_t id = 0; double seconds = 0, value = 0; };
struct ControlCurve {
  std::uint64_t id = 0;
  std::uint8_t channel = 0, controller = 11;
  std::vector<CurvePoint> points;
};
struct Performance {
  std::string name;
  std::uint64_t next_note_id = 1;
  std::vector<PerformedNoteMapping> mapping;
  std::vector<NotePerformance> notes;
  std::vector<ControlCurve> curves;
};
// Explicit initial correspondence, including multiple tied notation segments.
// The relation is stored, not inferred again on reopen. This slice executes
// ordinary attacks/ties; ornament one-to-many expansion is not implemented yet.
Performance makePerformance(const Score& score, const std::string& name);
struct PerformanceDocument {
  Score score;
  std::vector<Performance> performances;
  std::size_t active = 0;
  std::vector<std::uint8_t> piano_state;
  double gain_db = -12;
};
// First vertical slice: one piano part, up to 4096 attacks, fixed 48 kHz.
// Performance edits leave Score unchanged. Offsets never quantize written notes. All native
// channel messages are retained; AU fixed-preset filtering is a host concern.
MidiSampleSequence compilePerformance(const Score& score, const Performance& performance);
void validatePerformanceDocument(const PerformanceDocument& document);
void savePerformanceDocument(const PerformanceDocument& document, const std::string& new_directory);
PerformanceDocument loadPerformanceDocument(const std::string& directory);

// The document's ONE history stores addressed notation/performance/curve/gain
// edits, not score snapshots. It owns no ScoreHistory or SessionMixState stack.
// Selection itself is not an edit. New edits truncate redo; 128 commands max.
class WorkEditor {
 public:
  explicit WorkEditor(PerformanceDocument document);
  const PerformanceDocument& document() const noexcept { return document_; }
  void select(std::size_t take);
  bool set(const NotePerformance& value);
  bool setPitch(std::uint64_t notation_id, ScorePitch pitch);
  bool setCurvePoint(std::uint64_t curve_id, std::uint64_t point_id, double value);
  bool setGain(double db);
  bool undo();
  bool redo();
  std::uint64_t revision() const noexcept { return revision_; }
 private:
  enum class Kind { Note, Pitch, Curve, Gain };
  struct Change {
    Kind kind = Kind::Note; std::size_t take = 0; std::uint64_t id = 0, point = 0;
    std::optional<NotePerformance> before, after;
    ScorePitch pitch_before{}, pitch_after{};
    double value_before = 0, value_after = 0;
  };
  bool commit(Change change);
  void apply(const Change& change, bool forward);
  PerformanceDocument document_;
  std::vector<Change> history_;
  std::size_t cursor_ = 0;
  std::uint64_t revision_ = 0;
};
}
