// Repairs an imported score so the audition path can play it. MIDI 1.0 cannot
// address two same-pitch notes on one channel at once, and the score-to-MIDI
// bridge only follows a tie whose segments join exactly. Real engraving
// produces both situations constantly, so an importer has to resolve them.
// These repairs change sounding music and are counted; the engine's own
// validation stays strict, and nothing here runs unless a caller asks for it.
#include "daw/score.hpp"

#include <algorithm>
#include <map>
#include <tuple>
#include <vector>

namespace daw {
namespace {

// Same arithmetic as the MIDI bridge, but out-of-range answers are reported
// rather than thrown: a score the bridge cannot convert at all is its problem.
long long soundingPitch(const ScorePitch& pitch) {
  int semitone = -1000;
  switch (pitch.step) {
    case 'C': semitone = 0; break;
    case 'D': semitone = 2; break;
    case 'E': semitone = 4; break;
    case 'F': semitone = 5; break;
    case 'G': semitone = 7; break;
    case 'A': semitone = 9; break;
    case 'B': semitone = 11; break;
    default: return -1;
  }
  const long long value = (static_cast<long long>(pitch.octave) + 1) * 12 + semitone + pitch.alter;
  return value < 0 || value > 127 ? -1 : value;
}

// One sounding note: a single attack plus any tie segments that continue it.
struct Chain {
  std::vector<ScoreNote*> segments;
  Tick start = 0;
  Tick end = 0;
  int channel = 0;
  long long pitch = 0;
};

void silence(const Chain& chain) {
  for (ScoreNote* segment : chain.segments) {
    segment->rest = true;
    // A rest carries neither a tie nor a chord marker: the project format
    // rejects both, and a silenced chord tone is simply a rest at that spot.
    segment->tie_start = false;
    segment->tie_stop = false;
    segment->chord = false;
    segment->lyric.clear();
  }
}

void release(const Chain& chain, ScoreRepairReport* report) {
  for (ScoreNote* segment : chain.segments) {
    segment->tie_start = false;
    segment->tie_stop = false;
  }
  ++report->broken_tie_chains_released;
}

}  // namespace

void repairScoreForAudition(Score& score, ScoreRepairReport* report) {
  if (report == nullptr) return;
  for (std::size_t part_index = 0; part_index < score.parts.size(); ++part_index) {
    ScorePart& part = score.parts[part_index];

    std::vector<ScoreNote*> ordered;
    for (ScoreMeasure& measure : part.measures) {
      for (ScoreNote& note : measure.notes) {
        if (!note.rest && note.duration > 0 && soundingPitch(note.pitch) >= 0) ordered.push_back(&note);
      }
    }
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const ScoreNote* left, const ScoreNote* right) { return left->start < right->start; });

    const auto channelOf = [&](const ScoreNote& note) {
      return note.midi_channel == -1 ? static_cast<int>(part_index % 16) : static_cast<int>(note.midi_channel);
    };
    // The bridge keeps a tie inside one staff/voice/pitch/channel, so a repair
    // that used a looser key could join two notes the bridge never would.
    using TieKey = std::tuple<std::uint16_t, std::uint16_t, long long, int>;
    const auto keyOf = [&](const ScoreNote& note) {
      return TieKey{note.staff, note.voice, soundingPitch(note.pitch), channelOf(note)};
    };

    // Pass one: build chains, releasing any tie that cannot join exactly. A
    // released tie becomes a second attack, which is audible but playable; a
    // broken chain is neither playable nor recoverable further down.
    std::vector<Chain> chains;
    std::map<TieKey, std::size_t> open;
    for (ScoreNote* note : ordered) {
      const TieKey key = keyOf(*note);
      auto held = open.find(key);
      if (note->tie_stop && held != open.end() && chains[held->second].end == note->start) {
        Chain& chain = chains[held->second];
        chain.segments.push_back(note);
        chain.end = note->start + note->duration;
        if (!note->tie_start) open.erase(held);
        continue;
      }
      // One repair per chain, not per flag: a stop that cannot join its chain
      // and the chain it abandons are the same problem counted once below.
      if (note->tie_stop) {
        note->tie_stop = false;
        if (held == open.end()) ++report->broken_tie_chains_released;
      }
      // This note is a fresh attack, so any chain still open on this key can
      // never receive its continuation.
      if (held != open.end()) {
        release(chains[held->second], report);
        open.erase(held);
      }
      chains.push_back({{note}, note->start, note->start + note->duration, channelOf(*note),
                        soundingPitch(note->pitch)});
      if (note->tie_start) open.emplace(key, chains.size() - 1U);
    }
    // A chain still open at the end never got its final stop. Its last segment
    // may itself carry a stop, so clearing only the starts would strand it.
    for (const auto& leftover : open) release(chains[leftover.second], report);

    // Releasing a tie above left the chains that carried it describing notes
    // that no longer sound as one. Rebuild from the flags that survived, so
    // the collision pass never reasons about a grouping that is already gone.
    chains.clear();
    open.clear();
    for (ScoreNote* note : ordered) {
      const TieKey key = keyOf(*note);
      const auto held = open.find(key);
      if (note->tie_stop && held != open.end()) {
        Chain& chain = chains[held->second];
        chain.segments.push_back(note);
        chain.end = note->start + note->duration;
        if (!note->tie_start) open.erase(held);
        continue;
      }
      chains.push_back({{note}, note->start, note->start + note->duration, channelOf(*note),
                        soundingPitch(note->pitch)});
      if (note->tie_start) open.emplace(key, chains.size() - 1U);
    }

    // Pass two: the audition groups sounding notes by channel and pitch only,
    // so chains from different voices collide there even when the notation is
    // unambiguous. A coincident end and attack also counts as a collision.
    std::map<std::pair<int, long long>, std::vector<std::size_t>> lanes;
    for (std::size_t index = 0; index < chains.size(); ++index) {
      lanes[{chains[index].channel, chains[index].pitch}].push_back(index);
    }
    for (auto& lane : lanes) {
      auto& indices = lane.second;
      std::stable_sort(indices.begin(), indices.end(), [&](std::size_t left, std::size_t right) {
        return chains[left].start < chains[right].start;
      });
      // Follow the sounding chain that reaches furthest, not merely the one
      // before this in start order. A short or silenced chain would otherwise
      // hide a long one still sounding underneath it, and the collision it
      // conceals is exactly what the audition refuses later.
      std::size_t frontier = indices.front();
      for (std::size_t i = 1; i < indices.size(); ++i) {
        Chain& previous = chains[frontier];
        Chain& current = chains[indices[i]];
        if (previous.end < current.start) {
          frontier = indices[i];
          continue;
        }
        if (current.start > previous.start) {
          ScoreNote* tail = previous.segments.back();
          // Leave one tick of silence: the audition rejects a release landing
          // on the next attack, not only one that passes it.
          const Tick wanted = current.start - 1 - tail->start;
          if (wanted > 0) {
            const bool merely_adjacent = previous.end == current.start;
            tail->duration = wanted;
            previous.end = tail->start + wanted;
            if (merely_adjacent) ++report->repeats_separated;
            else ++report->overlaps_trimmed;
            frontier = indices[i];
            continue;
          }
        }
        // The attacks coincide, or nothing is left to shorten. Keep whichever
        // reaches further, so the repair loses as little sounding music as it
        // can; nothing here can recover which voice the author meant.
        const bool current_reaches_further = current.end > previous.end;
        silence(current_reaches_further ? previous : current);
        (current_reaches_further ? previous : current).end =
            (current_reaches_further ? previous : current).start;
        ++report->overlaps_silenced;
        if (current_reaches_further) frontier = indices[i];
      }
    }
  }
}

}  // namespace daw
