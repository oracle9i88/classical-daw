#pragma once

#include "daw/types.hpp"
#include "daw/midi_event.hpp"
#include "daw/tempo_map.hpp"

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
  std::uint16_t voice = 1;
  std::uint16_t staff = 1;
  std::uint16_t tuplet_actual = 0;
  std::uint16_t tuplet_normal = 0;
  // Optional single lyric syllable attached to this note.  The current
  // interchange slice keeps one <lyric><text> value per note; richer
  // syllabic/verse metadata can be added without changing note timing.
  std::string lyric;
  // Imported playback identity survives Score/project edits. -1 uses the
  // score exporter's default part channel; 0..15 retains an explicit channel.
  std::int16_t midi_channel = -1;
  // Clear ordinals when moving/repitching an imported note or changing its
  // endpoints, so newly authored event ordering is used at the edited boundary.
  std::uint64_t midi_on_order = 0;
  std::uint64_t midi_off_order = 0;
  std::uint8_t midi_release_velocity = 0;
  // Persistent notation-segment identity. Zero is an unassigned legacy/import
  // note. The MIDI bridge uses a tie's first segment as its notation anchor
  // when makePerformance builds the initial correspondence. Performed-note IDs
  // come from a separate allocator; the anchor is not a performed-note ID.
  std::uint64_t id = 0;
};

struct ScoreMeasure {
  int number = 1;
  Tick start = 0;
  std::vector<ScoreNote> notes;
  // Explicit notated extent, including trailing silence. Zero is the legacy
  // unspecified value: infer from the next measure or the active meter.
  Tick duration = 0;
  // MusicXML's printed token (e.g. "0", "X1", "01"). Empty uses number.
  // This never determines chronological order or playback time.
  std::string label;
};

struct ScorePart {
  std::string id = "P1";
  std::string name = "Part 1";
  std::vector<ScoreMeasure> measures;
  std::vector<MidiChannelEvent> midi_events{};
};

// Internal score state uses the engine's fixed 960 PPQ tick domain. MusicXML
// is an interchange format at the boundary; it is not the realtime state.
struct Score {
  Tick divisions = kTicksPerQuarter;
  TimeSignature time_signature{};
  double bpm = 120.0;
  std::vector<ScorePart> parts;
  // bpm is the authoritative tempo at tick zero. These later changes use
  // absolute 960-PPQ ticks, strictly increasing and greater than zero.
  // Empty preserves the legacy constant-tempo behavior. Values are steps,
  // not continuous ramps; changing bpm does not scale the later tempos.
  std::vector<TempoChange> tempo_changes{};
  // Initial time_signature is authoritative at zero. Later changes retain
  // all four SMF meter values at strictly increasing positive tick positions.
  std::vector<TimeSignatureChange> meter_changes{};
  // Zero denotes an unidentified legacy score; otherwise monotonic high water.
  std::uint64_t next_note_id = 0;
};

// Control thread, explicit opt-in migration. Assign only missing IDs, preserving
// existing identities and allocator high water. Strong exception guarantee.
void assignNoteIds(Score& score);
void validateNoteIds(const Score& score);

inline constexpr std::size_t kMaxScoreTempoChanges = 1'000'000;

// Validate and construct the complete playback map. All BPM values must be
// finite, positive and <= 1,000,000; at most kMaxScoreTempoChanges later
// entries are accepted. Invalid/duplicate/unsorted entries throw rather than
// being silently reordered or replacing the initial bpm.
TempoMap scoreTempoMap(const Score& score);

// This first vertical slice supports ordered parts with multiple voices/staves.
// Import normalizes positive decimal source divisions per part (leading changes
// at measure starts) exactly to 960 ticks. Decimal parsing uses bounded exact
// fractions (18 fractional places after trailing-zero removal; signed-64-bit
// mantissa); out-of-range values and rounding are rejected. Export uses 960 divisions.
// It preserves pitch, integer duration, rests, chords, ties, tuplets, meter,
// tempo, and MIDI velocity via the standard note dynamics percentage.
// Meter declarations at measure starts are retained across synchronized parts.
// Short/empty measures export explicit duration through forward moves;
// implicit input measures use their actual note/forward extent. Mid-measure or
// staff-specific time declarations and polymeter are explicitly unsupported.
// Step tempos retain their absolute ticks, including intra-measure changes.
// Import respects direction/sound offsets and simple numeric metronome marks;
// conditional tempos, metric modulation and cross-measure offsets are unsupported.
// Full engraving metadata remains outside this model.
struct MusicXmlExportReport {
  // The native project and SMF bridge retain these fields; the current
  // MusicXML notation slice omits them. Unchanged on failed export.
  std::uint64_t omitted_midi_events = 0;
  std::uint64_t omitted_note_midi_metadata = 0;
  // Retained for API compatibility. Successful XML export preserves the entire
  // step-tempo map (zero omissions); changes beyond stored measures fail.
  std::uint64_t omitted_tempo_changes = 0;
  // One for a nondefault initial clock setting, plus one per later event
  // with nondefault clocks or redundant n/d. XML retains canonical n/d changes
  // only. Nonstandard notation ratios (bb != 8) fail rather than alter timing.
  std::uint64_t omitted_meter_playback_metadata = 0;
};
bool writeMusicXmlFile(const Score& score, const std::string& path, std::string* error = nullptr,
                       MusicXmlExportReport* report = nullptr);
// Import repairs. Passing a report ASKS the reader to repair recoverable
// constructs instead of refusing the whole file, and to state exactly what it
// changed. Passing nullptr keeps the strict behavior every round-trip test
// relies on. A repair changes sounding music: it is an interpretation of an
// unrepresentable construct, never a claim of a lossless read.
struct MusicXmlImportReport {
  // Grace notes carry no written duration. Each borrows its notated type value
  // from the note it decorates, and that note keeps at least half of its own.
  std::uint64_t grace_notes_timed = 0;
  // Nothing in the voice had room to lend time; those graces are dropped.
  std::uint64_t grace_notes_dropped = 0;
  // Two different tempos declared at one tick; the later declaration wins.
  std::uint64_t conflicting_tempos_resolved = 0;
  // Verses beyond the first, and malformed lyric blocks, are dropped.
  std::uint64_t extra_lyrics_dropped = 0;
};
bool readMusicXmlFile(const std::string& path, Score* score, std::string* error = nullptr,
                      MusicXmlImportReport* report = nullptr);

// Make an imported score playable by the audition path. MIDI 1.0 cannot address
// two same-pitch notes on one channel at once, and the score-to-MIDI bridge only
// follows a tie whose segments join exactly; real engraving produces both all
// the time. Passing nullptr does nothing, so the engine keeps its strict view.
struct ScoreRepairReport {
  // A repeated note whose release landed exactly on the next attack. One tick
  // of silence is inserted; at 960 PPQ that is well under a millisecond.
  std::uint64_t repeats_separated = 0;
  // A genuine overlap: the earlier note was audibly shortened to make room.
  std::uint64_t overlaps_trimmed = 0;
  // Two attacks coincided, or nothing could be shortened; the later is silenced.
  std::uint64_t overlaps_silenced = 0;
  // A tie whose segments did not join exactly; it becomes a second attack.
  std::uint64_t broken_tie_chains_released = 0;
  // A note released at or before its own attack. It can never sound, so there
  // is no music to lose by dropping it; exporters emit these routinely.
  std::uint64_t dropped_silent_notes = 0;
  // A release with no attack to close. Nothing identifies what it referred to.
  std::uint64_t dropped_orphan_releases = 0;
};
void repairScoreForAudition(Score& score, ScoreRepairReport* report);

}  // namespace daw
