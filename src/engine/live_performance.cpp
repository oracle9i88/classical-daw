#include "daw/live_performance.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

namespace daw {
namespace {
constexpr std::size_t no_slot = LivePerformanceStream::max_notes;
void require(bool ok, const char* why) { if (!ok) throw std::invalid_argument(why); }
bool noteOn(const TimedMidiEvent& e) { return (e.status & 0xf0) == 0x90 && e.data2; }
bool noteOff(const TimedMidiEvent& e) { return (e.status & 0xf0) == 0x80 || ((e.status & 0xf0) == 0x90 && !e.data2); }
int lane(const TimedMidiEvent& e) {
  if ((e.status & 0xf0) != 0xb0) return -1;
  return e.data1 == 64 ? 0 : (e.data1 == 11 ? 1 : -1);
}
std::vector<TimedMidiEvent> fixedEvents(const MidiSampleSequence& seq) {
  std::vector<TimedMidiEvent> result;
  for (const auto& e : seq.events)
    if (!noteOn(e) && !noteOff(e) && lane(e) < 0 && !e.terminal_reset) result.push_back(e);
  return result;
}
bool sameEvents(const std::vector<TimedMidiEvent>& a, const std::vector<TimedMidiEvent>& b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const auto& x, const auto& y) {
    return x.frame == y.frame && x.status == y.status && x.data1 == y.data1 && x.data2 == y.data2;
  });
}
}
struct LivePerformanceStream::Plan {
  struct Note {
    bool present = false;
    std::size_t on = 0, off = 0;
    std::uint8_t channel = 0, pitch = 0, velocity = 0, release = 0;
  };
  struct Event { TimedMidiEvent midi; std::size_t slot = no_slot; };
  std::vector<Note> notes;
  std::vector<Event> events;
  std::array<std::array<std::vector<TimedMidiEvent>, 2>, 16> lanes;
  std::size_t frames = 0;
  double gain = 1;
  std::uint64_t revision = 0;
};
LivePerformanceStream::LivePerformanceStream(MidiSampleSequence sequence, double gain, std::uint64_t revision) {
  static_assert(std::atomic<const Plan*>::is_always_lock_free);
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
  for (const auto& e : sequence.events) if (noteOn(e)) ids_.push_back(e.note_id);
  std::sort(ids_.begin(), ids_.end());
  require(ids_.size() <= max_notes && (ids_.empty() || ids_.front() != 0) &&
      std::adjacent_find(ids_.begin(), ids_.end()) == ids_.end(), "invalid live performed-ID universe");
  fixed_events_ = fixedEvents(sequence);
  owned_[0] = prepare(std::move(sequence), gain, revision);
  active_ = owned_[0].get(); submitted_revision_ = revision;
  applied_.store(revision); requested_.store(revision); render_end_ = active_->frames; end_frame_.store(render_end_);
  for (auto& channel : controls_) channel = {-1, -1}; // no message sent yet
  for (auto& channel : key_owner_) channel.fill(no_slot);
}
LivePerformanceStream::~LivePerformanceStream() = default;
std::unique_ptr<LivePerformanceStream::Plan> LivePerformanceStream::prepare(
    MidiSampleSequence sequence, double gain, std::uint64_t revision) const {
  require(sequence.sample_rate == 48000 && sequence.frames > sequence.end_frame &&
      sequence.frames <= 48000U * 60 * 30 && std::isfinite(gain) && gain >= 0 && gain <= 1,
      "invalid live plan bounds");
  require(sameEvents(fixed_events_, fixedEvents(sequence)), "live edits may only change notes, CC64/CC11 and gain");
  auto plan = std::make_unique<Plan>();
  plan->notes.resize(ids_.size()); plan->events.reserve(sequence.events.size());
  plan->frames = sequence.frames; plan->gain = gain; plan->revision = revision;
  std::vector<bool> released(ids_.size(), false);
  std::size_t previous = 0;
  for (std::size_t i = 0; i < sequence.events.size(); ++i) {
    const auto& e = sequence.events[i]; const auto type = e.status & 0xf0;
    require(e.frame < sequence.frames && e.frame >= previous && type >= 0x80 && type <= 0xe0 &&
        e.data1 < 128 && e.data2 < 128 && ((type != 0xc0 && type != 0xd0) || e.data2 == 0), "invalid live MIDI event");
    if (i >= 4096) require(e.frame - sequence.events[i-4096].frame >= 256, "live source event density exceeded");
    previous = e.frame;
    if (type == 0xb0) {
      require(!((e.data1 == 120 || e.data1 == 121 || e.data1 >= 123) && !e.terminal_reset), "live channel-mode changes unsupported");
      require(!((e.data1 == 66 || e.data1 == 69) && e.data2), "live sostenuto/hold-2 unsupported");
      require(!e.terminal_reset || (e.frame == sequence.end_frame && e.data2 == 0 &&
          (e.data1 == 64 || e.data1 == 66 || e.data1 == 69 || e.data1 == 123)), "invalid live terminal reset");
    } else require(!e.terminal_reset, "invalid live terminal flag");
    std::size_t slot = no_slot;
    if (noteOn(e) || noteOff(e)) {
      const auto found = std::lower_bound(ids_.begin(), ids_.end(), e.note_id);
      require(found != ids_.end() && *found == e.note_id, "new performed IDs require stopped transport");
      slot = static_cast<std::size_t>(found - ids_.begin()); auto& note = plan->notes[slot];
      if (noteOn(e)) {
        require(!note.present, "duplicate live attack");
        note = {true, e.frame, 0, static_cast<std::uint8_t>(e.status & 15), e.data1, e.data2, 0};
      } else {
        require(note.present && !released[slot] && e.frame > note.on && (e.status & 15) == note.channel && e.data1 == note.pitch,
            "unpaired live release");
        note.off = e.frame; note.release = e.data2; released[slot] = true;
      }
    } else require(e.note_id == 0, "controller carries note identity");
    const auto controller = lane(e);
    if (controller >= 0) plan->lanes[e.status & 15][static_cast<std::size_t>(controller)].push_back(e);
    plan->events.push_back({e, slot});
  }
  std::map<std::pair<int,int>, std::vector<std::pair<std::size_t,std::size_t>>> pitches;
  for (std::size_t i = 0; i < plan->notes.size(); ++i) if (plan->notes[i].present) {
    require(released[i], "missing live release"); const auto& n = plan->notes[i];
    require(n.off <= sequence.end_frame, "release beyond final reset boundary");
    pitches[{n.channel,n.pitch}].push_back({n.on,n.off});
  }
  for (auto& entry : pitches) {
    auto& notes = entry.second; std::sort(notes.begin(),notes.end());
    for (std::size_t i = 1; i < notes.size(); ++i) require(notes[i].first > notes[i-1].second, "ambiguous live same-pitch overlap");
  }
  return plan;
}
void LivePerformanceStream::collectRetired() noexcept {
  if (const auto* old = retired_.exchange(nullptr, std::memory_order_acq_rel))
    for (auto& slot : owned_) if (slot.get() == old) { slot.reset(); break; }
}
void LivePerformanceStream::submit(MidiSampleSequence sequence, double gain, std::uint64_t revision) {
  collectRetired();
  require(!failed() && revision > submitted_revision_, "invalid or failed live revision");
  auto free = std::find_if(owned_.begin(), owned_.end(), [](const auto& slot) { return !slot; });
  require(free != owned_.end(), "live update pending; retry after the next audio block");
  auto next = prepare(std::move(sequence), gain, revision); // no side effects on failure
  *free = std::move(next); submitted_revision_ = revision;
  requested_.store(revision,std::memory_order_release);
  pending_.store(free->get(), std::memory_order_release);
}
void LivePerformanceStream::emit(TimedMidiEvent event, std::size_t slot, std::size_t& count) noexcept {
  const int controller = lane(event);
  if (controller >= 0 && controls_[event.status&15][static_cast<std::size_t>(controller)] == event.data2) return;
  if (slot < ids_.size()) {
    auto& owner = key_owner_[event.status&15][event.data1];
    if (noteOn(event) && owner != no_slot && owner != slot) {
      // Retiming a held attack can create a collision absent from either score.
      // Preserve its voice; consume the conflicting attack for this pass only.
      voices_[slot].fired = true; ++suppressed_conflicts_; return;
    }
    if (noteOff(event) && owner != slot) return;
  }
  if (count == block_.size()) { failed_.store(true); return; }
  block_[count++] = event;
  if (slot < ids_.size()) {
    auto& voice = voices_[slot];
    if (noteOn(event)) {
      voice.fired = true; voice.down = true; voice.channel = event.status & 15; voice.pitch = event.data1;
      key_owner_[voice.channel][voice.pitch] = slot;
    } else if (noteOff(event)) {
      voice.down = false; key_owner_[event.status&15][event.data1] = no_slot;
    }
  }
  if (controller >= 0) controls_[event.status & 15][static_cast<std::size_t>(controller)] = event.data2;
  if (event.terminal_reset && event.data1 == 123) {
    for (auto& voice : voices_) if (voice.channel == (event.status & 15)) voice.down = false;
    key_owner_[event.status&15].fill(no_slot);
  }
}
void LivePerformanceStream::switchPlan(const Plan& next, std::size_t& count) noexcept {
  // Release only affected keys. Never panic/reset pedals to hide ownership bugs.
  for (std::size_t i = 0; i < ids_.size(); ++i) {
    auto& voice = voices_[i]; const auto& note = next.notes[i];
    if (!voice.down) continue;
    const bool same = note.present && note.channel == voice.channel && note.pitch == voice.pitch;
    if (same && note.off >= position_) continue; // preserve attack and exact-boundary release velocity
    emit({0,static_cast<std::uint8_t>(0x80|voice.channel),voice.pitch,0,ids_[i]},i,count);
    if (note.present && !same) voice.fired = false; // explicit repitch/route edit
  }
  // Chase strictly preceding values. Source messages at the boundary must
  // retain their order, even NoteOn followed by CC11 or several pedal changes.
  for (std::size_t channel = 0; channel < 16; ++channel) for (std::size_t lane_id = 0; lane_id < 2; ++lane_id) {
    const auto& events = next.lanes[channel][lane_id];
    auto past = std::lower_bound(events.begin(),events.end(),position_,[](const auto& e, auto frame) { return e.frame < frame; });
    const int value = past == events.begin() ? (lane_id == 0 ? 0 : 127) : std::prev(past)->data2;
    if (value != controls_[channel][lane_id]) emit({0,static_cast<std::uint8_t>(0xb0|channel),
        static_cast<std::uint8_t>(lane_id == 0 ? 64 : 11),static_cast<std::uint8_t>(value)},no_slot,count);
  }
}
void LivePerformanceStream::catchUp(const Plan& next, std::size_t& count) noexcept {
  // After exact-boundary source events, newly moved notes spanning 'now' can
  // start with current controller state. Already fired/released
  // attacks are history: duration/onset/velocity edits never resurrect them.
  for (std::size_t i = 0; i < ids_.size(); ++i) {
    const auto& note = next.notes[i];
    if (!voices_[i].fired && note.present && note.on < position_ && note.off > position_)
      emit({0,static_cast<std::uint8_t>(0x90|note.channel),note.pitch,note.velocity,ids_[i]},i,count);
  }
}
LivePerformanceStream::Block LivePerformanceStream::nextBlock(std::uint32_t frames) noexcept {
  Block result; result.events = block_.data(); result.frame = position_;
  if (failed() || !frames || frames > 256) { failed_.store(true); return result; }
  std::size_t count = 0;
  bool changed = false;
  if (!initialized_controls_) {
    for (std::size_t channel = 0; channel < 16; ++channel) {
      emit({0,static_cast<std::uint8_t>(0xb0|channel),64,0},no_slot,count);
      emit({0,static_cast<std::uint8_t>(0xb0|channel),11,127},no_slot,count);
    }
    initialized_controls_ = true;
  }
  if (const auto* next = pending_.exchange(nullptr,std::memory_order_acq_rel)) {
    // An already-ended pass may accept document revisions silently. Do not
    // synthesize an extra render block beyond its reserved/captured end.
    changed = position_ < render_end_ || position_ < next->frames;
    if (changed) switchPlan(*next,count);
    const auto* old = active_; active_ = next;
    cursor_ = static_cast<std::size_t>(std::lower_bound(next->events.begin(),next->events.end(),position_,
        [](const auto& e, auto frame) { return e.midi.frame < frame; }) - next->events.begin());
    // Never shorten a run's release-tail budget. Any still-down old key had
    // its old planned release + tail reserved before this boundary.
    render_end_ = std::max(render_end_,next->frames);
    end_frame_.store(render_end_,std::memory_order_release); ++update_count_;
    applied_frame_.store(position_,std::memory_order_release);
    applied_.store(next->revision,std::memory_order_release);
    // No access to the old plan after this release; its destructor is control-only.
    retired_.store(old,std::memory_order_release);
  }
  const auto remaining = render_end_ > position_ ? render_end_-position_ : 0;
  result.frames = static_cast<std::uint32_t>(std::min<std::size_t>(frames, remaining));
  auto dispatch = [&] {
    const auto& scheduled = active_->events[cursor_++]; auto event = scheduled.midi;
    if (event.frame < position_) return;
    event.frame -= position_;
    if (scheduled.slot < ids_.size()) {
      const auto& voice = voices_[scheduled.slot];
      if (noteOn(event) && voice.fired) return;
      if (noteOff(event) && (!voice.down || voice.channel != (event.status&15) || voice.pitch != event.data1)) return;
    }
    emit(event,scheduled.slot,count);
  };
  while (result.frames && cursor_ < active_->events.size() && active_->events[cursor_].midi.frame == position_) dispatch();
  if (changed) catchUp(*active_,count);
  while (cursor_ < active_->events.size() && active_->events[cursor_].midi.frame < position_+result.frames) dispatch();
  result.count = count; result.gain = active_->gain; result.revision = active_->revision;
  position_ += result.frames; published_frame_.store(position_,std::memory_order_release);
  return result;
}
}
