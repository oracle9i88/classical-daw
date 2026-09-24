#pragma once
#include "daw/score.hpp"
#include "daw/midi_sequence.hpp"
#include <optional>
#include <functional>
#include <memory>
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

// Turn the controller messages a score already carries into an editable lane.
// A curve OWNS its lane: adopting one replaces those messages at playback, and
// the lane is then sampled every ten milliseconds and interpolated, so the
// timing a MIDI file specified to the sample is quantised. That is a real cost
// and the reason this is a command rather than something import does quietly.
// The step shape survives: each change is written as the old value held until
// one millisecond before it, then the new value. CC64 reads as released and
// CC11 as full before the first message, matching the offline renderer.
// Reports how many messages became how many points, and the worst shift any
// message suffers from the ten-millisecond grid.
struct CurveAdoptionReport {
  std::uint64_t source_messages = 0;
  std::uint64_t points = 0;
  double worst_shift_seconds = 0;
};
ControlCurve curveFromScoreMessages(const Score& score, std::uint8_t channel, std::uint8_t controller,
                                    std::uint64_t curve_id, CurveAdoptionReport* report = nullptr);
struct PerformanceDocument {
  Score score;
  std::vector<Performance> performances;
  std::size_t active = 0;
  std::vector<std::uint8_t> piano_state;
  double gain_db = -12;
};
// How many attacks one audition can hold. This bounds a per-track voice ledger
// in the audio thread and nothing the callback does per block: block work is
// bounded separately, by the event-density rule below. A Beethoven sonata
// movement runs to eight thousand notes, so the old four thousand shut out the
// repertoire this is for while saving sixty kilobytes.
inline constexpr std::size_t kMaxAuditionAttacks = 16384;
// At most this many events may fall inside one 256-frame block. That is the
// realtime scratch capacity and is deliberately NOT the total-note limit.
inline constexpr std::size_t kMaxEventsPerRealtimeSlice = 4096;

// First vertical slice: one piano part, fixed 48 kHz.
// Performance edits leave Score unchanged. Offsets never quantize written notes. All native
// channel messages are retained; AU fixed-preset filtering is a host concern.
MidiSampleSequence compilePerformance(const Score& score, const Performance& performance);
// Validated override lists; absence means zero offset. Compare stored seconds,
// including sub-sample edits, rather than rounded scheduled frame numbers.
std::vector<std::uint64_t> changedPerformanceOnsets(const std::vector<NotePerformance>& before,
                                                  const std::vector<NotePerformance>& after);
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
  // The same score played twice is the whole point, and until now a document
  // could only hold two readings if it was born with them. Copies the active
  // performance, names it, and selects it. Selecting is not an edit; making
  // one is, so it is on the same history as everything else.
  bool copyTake(const std::string& name);
  bool set(const NotePerformance& value);
  bool setPitch(std::uint64_t notation_id, ScorePitch pitch);
  bool setCurvePoint(std::uint64_t curve_id, std::uint64_t point_id, double value);
  // Create/replace one explicit CC lane, or remove it, on the same undo stack.
  // A lane replaces imported messages for that channel/controller at playback.
  bool putCurve(ControlCurve curve);
  bool removeCurve(std::uint64_t curve_id);
  bool setGain(double db);
  // Shaping a passage is one musical act, so it is one command and one undo.
  // The value ramps linearly with each note's position between the two times,
  // by where the note is heard now rather than where it was written, and only
  // the named field moves: a crescendo does not discard timing already given.
  enum class Shape { OnsetMilliseconds, DurationScale, Velocity };
  std::size_t shapeRange(double from_seconds, double to_seconds, Shape field,
                         double from_value, double to_value);
  bool undo();
  bool redo();
  std::uint64_t revision() const noexcept { return revision_; }
  // Optional control-thread admission, after validation and before command
  // acceptance. Throwing must leave the playing plan unchanged; the document/
  // history roll back. A guarded live sink waits for the audio-thread decision
  // before returning (rejected/cancelled candidates never become active).
  // After acceptance no fallible work remains. Never called from the callback.
  using CommitAdmission = std::function<void(const PerformanceDocument&, std::uint64_t)>;
  void setCommitAdmission(CommitAdmission admission) { admission_ = std::move(admission); }
 private:
  enum class Kind { Note, Pitch, Curve, Gain, CurveLane, NoteRange, TakeAdd };
  struct Change {
    Kind kind = Kind::Note; std::size_t take = 0; std::uint64_t id = 0, point = 0;
    std::optional<NotePerformance> before, after;
    ScorePitch pitch_before{}, pitch_after{};
    double value_before = 0, value_after = 0;
    std::shared_ptr<const ControlCurve> curve_before, curve_after;
    // A range edit carries every override it writes and every one it replaced.
    // Held by handle so a Change stays nothrow-copyable and history mutation
    // after a successful apply still cannot fail.
    std::shared_ptr<const std::vector<NotePerformance>> notes_before, notes_after;
    // A new reading, held by handle for the same reason.
    std::shared_ptr<const Performance> take_added;
  };
  bool commit(Change change);
  void apply(const Change& change, bool forward);
  PerformanceDocument document_;
  std::vector<Change> history_;
  std::size_t cursor_ = 0;
  std::uint64_t revision_ = 0;
  CommitAdmission admission_;
};
}
