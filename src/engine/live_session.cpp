#include "daw/live_session.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <thread>

namespace daw {
namespace {
constexpr std::size_t no_slot = LiveSessionStream::max_notes;
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
struct LiveSessionStream::TrackPlan {
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
  bool audible = true;
  std::vector<std::size_t> guarded_onsets;
};

struct LiveSessionStream::Lane {
  struct Voice { bool fired=false, down=false, onset_locked=false; std::uint8_t channel=0,pitch=0; };
  std::vector<std::uint64_t> ids_;
  std::vector<TimedMidiEvent> fixed_events_;
  std::array<Voice,max_notes> voices_{};
  std::array<std::array<std::size_t,128>,16> key_owner_{};
  std::array<std::array<int,2>,16> controls_{};
  std::array<TimedMidiEvent,max_messages> block_{};
  std::size_t position_=0,cursor_=0;
  std::uint64_t suppressed_conflicts_=0;
  bool initialized_controls_=false,failed_=false;
  explicit Lane(const MidiSampleSequence& sequence) {
    for (const auto& e : sequence.events) if (noteOn(e)) ids_.push_back(e.note_id);
    std::sort(ids_.begin(), ids_.end());
    require(ids_.size() <= max_notes && (ids_.empty() || ids_.front() != 0) &&
        std::adjacent_find(ids_.begin(), ids_.end()) == ids_.end(), "invalid live performed-ID universe");
    fixed_events_ = fixedEvents(sequence);
    for(auto& channel:controls_) channel={-1,-1};
    for(auto& channel:key_owner_) channel.fill(no_slot);
  }
  std::unique_ptr<TrackPlan> prepare(MidiSampleSequence sequence,double gain,
      const std::vector<std::uint64_t>& guards) const;
  std::uint64_t rejected(const TrackPlan& plan) const noexcept {
    for(auto slot:plan.guarded_onsets) if(voices_[slot].onset_locked) return ids_[slot];
    return 0;
  }
  void emit(TimedMidiEvent event,std::size_t slot,std::size_t& count) noexcept;
  void switchPlan(const TrackPlan& next,std::size_t& count) noexcept;
  void catchUp(const TrackPlan& next,std::size_t& count) noexcept;
  TrackBlock process(const TrackPlan& plan,bool changed,std::size_t position,std::uint32_t frames) noexcept;
};
struct LiveSessionStream::Plan {
  std::vector<std::unique_ptr<TrackPlan>> tracks;
  std::size_t frames=0;
  std::uint64_t revision=0,ticket=0;
};
std::unique_ptr<LiveSessionStream::TrackPlan> LiveSessionStream::Lane::prepare(
    MidiSampleSequence sequence, double gain, const std::vector<std::uint64_t>& guards) const {
  require(sequence.sample_rate == 48000 && sequence.frames > sequence.end_frame &&
      sequence.frames <= 48000U * 60 * 30 && std::isfinite(gain) && gain >= 0 && gain <= 1,
      "invalid live plan bounds");
  require(sameEvents(fixed_events_, fixedEvents(sequence)), "live edits may only change notes, CC64/CC11 and gain");
  auto plan = std::make_unique<TrackPlan>();
  plan->notes.resize(ids_.size()); plan->events.reserve(sequence.events.size());
  plan->frames = sequence.frames; plan->gain = gain;
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
  for (const auto id : guards) {
    const auto found = std::lower_bound(ids_.begin(), ids_.end(), id);
    require(found != ids_.end() && *found == id, "unknown guarded performed ID");
    plan->guarded_onsets.push_back(static_cast<std::size_t>(found-ids_.begin()));
  }
  std::sort(plan->guarded_onsets.begin(), plan->guarded_onsets.end());
  plan->guarded_onsets.erase(std::unique(plan->guarded_onsets.begin(), plan->guarded_onsets.end()), plan->guarded_onsets.end());
  return plan;
}
void LiveSessionStream::Lane::emit(TimedMidiEvent event, std::size_t slot, std::size_t& count) noexcept {
  const int controller = lane(event);
  if (controller >= 0 && controls_[event.status&15][static_cast<std::size_t>(controller)] == event.data2) return;
  if (slot < ids_.size()) {
    auto& owner = key_owner_[event.status&15][event.data1];
    if (noteOn(event) && owner != no_slot && owner != slot) {
      // Retiming a held attack can create a collision absent from either score.
      // Preserve its voice; consume the conflicting attack for this pass only.
      voices_[slot].fired = true; voices_[slot].onset_locked = true; ++suppressed_conflicts_; return;
    }
    if (noteOff(event) && owner != slot) return;
  }
  if (count == block_.size()) { failed_ = true; return; }
  block_[count++] = event;
  if (slot < ids_.size()) {
    auto& voice = voices_[slot];
    if (noteOn(event)) {
      voice.fired = true; voice.down = true; voice.onset_locked = true;
      voice.channel = event.status & 15; voice.pitch = event.data1;
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
void LiveSessionStream::Lane::switchPlan(const TrackPlan& next, std::size_t& count) noexcept {
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
void LiveSessionStream::Lane::catchUp(const TrackPlan& next, std::size_t& count) noexcept {
  // After exact-boundary source events, newly moved notes spanning 'now' can
  // start with current controller state. Already fired/released
  // attacks are history: duration/onset/velocity edits never resurrect them.
  for (std::size_t i = 0; i < ids_.size(); ++i) {
    const auto& note = next.notes[i];
    if (!voices_[i].fired && note.present && note.on < position_ && note.off > position_)
      emit({0,static_cast<std::uint8_t>(0x90|note.channel),note.pitch,note.velocity,ids_[i]},i,count);
  }
}

LiveSessionStream::TrackBlock LiveSessionStream::Lane::process(const TrackPlan& plan,bool changed,
    std::size_t position,std::uint32_t frames) noexcept {
  position_=position;
  TrackBlock result; result.events=block_.data(); result.frame=position; result.frames=frames;
  result.gain=plan.gain; result.audible=plan.audible;
  std::size_t count=0;
  if(!initialized_controls_) {
    for(std::size_t channel=0;channel<16;++channel) {
      emit({0,static_cast<std::uint8_t>(0xb0|channel),64,0},no_slot,count);
      emit({0,static_cast<std::uint8_t>(0xb0|channel),11,127},no_slot,count);
    }
    initialized_controls_=true;
  }
  if(changed) {
    if(frames) switchPlan(plan,count);
    cursor_=static_cast<std::size_t>(std::lower_bound(plan.events.begin(),plan.events.end(),position,
        [](const auto& e,auto frame){return e.midi.frame<frame;})-plan.events.begin());
  }
  auto dispatch=[&] {
    const auto& scheduled=plan.events[cursor_++]; auto event=scheduled.midi;
    if(event.frame<position_) return;
    event.frame-=position_;
    if(scheduled.slot<ids_.size()) {
      const auto& voice=voices_[scheduled.slot];
      if(noteOn(event) && voice.fired) return;
      if(noteOff(event) && (!voice.down || voice.channel!=(event.status&15) || voice.pitch!=event.data1)) return;
    }
    emit(event,scheduled.slot,count);
  };
  while(frames && cursor_<plan.events.size() && plan.events[cursor_].midi.frame==position) dispatch();
  if(changed && frames) catchUp(plan,count);
  while(cursor_<plan.events.size() && plan.events[cursor_].midi.frame<position+frames) dispatch();
  result.count=count; return result;
}

LiveSessionStream::LiveSessionStream(std::vector<LiveTrackPlan> tracks,std::uint64_t revision) {
  static_assert(std::atomic<const Plan*>::is_always_lock_free);
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
  require(!tracks.empty() && tracks.size()<=max_tracks,"live session requires 1..64 tracks");
  for(const auto& track:tracks) {
    require(!track.track_id.empty() && track.track_id.size()<=1024 &&
        std::find(ids_.begin(),ids_.end(),track.track_id)==ids_.end(),"invalid live track identity");
    ids_.push_back(track.track_id);
    lanes_.push_back(std::make_unique<Lane>(track.sequence));
  }
  blocks_.resize(tracks.size());
  owned_[0]=prepare(std::move(tracks),revision); active_=owned_[0].get();
  submitted_revision_=revision; applied_.store(revision);
  render_end_=active_->frames; end_frame_.store(render_end_);
}
LiveSessionStream::~LiveSessionStream()=default;
std::unique_ptr<LiveSessionStream::Plan> LiveSessionStream::prepare(
    std::vector<LiveTrackPlan> tracks,std::uint64_t revision) const {
  require(tracks.size()==ids_.size(),"live topology change requires stopped transport");
  auto plan=std::make_unique<Plan>(); plan->revision=revision; plan->tracks.resize(ids_.size());
  for(auto& track:tracks) {
    const auto found=std::find(ids_.begin(),ids_.end(),track.track_id);
    require(found!=ids_.end(),"unknown live track identity");
    const auto slot=static_cast<std::size_t>(found-ids_.begin());
    require(!plan->tracks[slot],"duplicate live track identity");
    require(track.track_delay_us==0,"nonzero musical track_delay_us playback is not implemented");
    auto prepared=lanes_[slot]->prepare(std::move(track.sequence),track.gain,track.reject_started_onsets);
    prepared->audible=track.audible;
    plan->frames=std::max(plan->frames,prepared->frames);
    plan->tracks[slot]=std::move(prepared);
  }
  return plan;
}
std::uint64_t LiveSessionStream::suppressedConflictsAfterStop() const noexcept {
  std::uint64_t n=0; for(const auto& lane:lanes_) n+=lane->suppressed_conflicts_; return n;
}
void LiveSessionStream::collectRetired() noexcept {
  // ACK comes after retirement. Do not reclaim a just-retired candidate while
  // the callback still publishes its decision/identity.
  if (acknowledged_.load(std::memory_order_acquire) != submitted_ticket_) return;
  if (const auto* old = retired_.exchange(nullptr, std::memory_order_acq_rel))
    for (auto& slot : owned_) if (slot.get() == old) { slot.reset(); break; }
  submitted_revision_ = appliedRevision();
  submitted_plan_ = nullptr;
}
std::uint64_t LiveSessionStream::submit(std::vector<LiveTrackPlan> tracks, std::uint64_t revision) {
  collectRetired();
  require(acknowledged_.load(std::memory_order_acquire) == submitted_ticket_, "live update awaiting callback decision");
  require(!failed() && revision > submitted_revision_, "invalid or failed live revision");
  auto free = std::find_if(owned_.begin(), owned_.end(), [](const auto& slot) { return !slot; });
  require(free != owned_.end(), "live update pending; retry after the next audio block");
  auto next = prepare(std::move(tracks), revision); // all lanes before the only publication
  require(submitted_ticket_ != UINT64_MAX, "live request ticket exhausted");
  next->ticket = ++submitted_ticket_;
  *free = std::move(next); submitted_revision_ = revision;
  submitted_plan_ = free->get();
  requested_.store(submitted_ticket_,std::memory_order_release);
  publish_begin_frame_ = frame();
  pending_.store(free->get(), std::memory_order_release);
  publish_end_frame_ = frame();
  return submitted_ticket_;
}
LiveSessionStream::Receipt LiveSessionStream::receipt(std::uint64_t ticket) const {
  require(ticket != 0 && ticket == submitted_ticket_, "receipt requires latest live request ticket");
  Receipt result = acknowledged_.load(std::memory_order_acquire) != ticket ? Receipt{Decision::Pending,ticket} : receipt_;
  result.publish_begin_frame = publish_begin_frame_;
  result.publish_end_frame = publish_end_frame_;
  return result;
}
bool LiveSessionStream::cancelPending(std::uint64_t ticket) {
  if (receipt(ticket).decision != Decision::Pending) return false;
  const auto* expected = submitted_plan_;
  if (!pending_.compare_exchange_strong(expected,nullptr,std::memory_order_acq_rel)) return false;
  // CAS won ownership before the callback: it can never access this candidate.
  receipt_ = {Decision::Cancelled,ticket,submitted_revision_,0,frame()};
  retired_.store(submitted_plan_,std::memory_order_release);
  acknowledged_.store(ticket,std::memory_order_release);
  collectRetired();
  return true;
}
LiveSessionStream::Receipt LiveSessionStream::waitForDecision(
    std::uint64_t ticket, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now()+timeout;
  for (;;) {
    const auto result = receipt(ticket);
    if (result.decision != Decision::Pending) { collectRetired(); return result; }
    if (std::chrono::steady_clock::now() >= deadline && cancelPending(ticket)) return receipt(ticket);
    // If the callback already claimed the plan, never roll back a possibly
    // applied edit on a timeout. Its bounded host-only decision precedes AU DSP.
    std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
}

LiveSessionStream::Block LiveSessionStream::nextBlock(std::uint32_t frames) noexcept {
  Block result; result.tracks=blocks_.data(); result.count=blocks_.size(); result.frame=position_;
  if(failed() || !frames || frames>256) { failed_.store(true); result.count=0; return result; }
  const auto* candidate=pending_.exchange(nullptr,std::memory_order_acq_rel);
  const Plan* retire=nullptr;
  Receipt decision;
  bool changed=false;
  if(candidate) {
    std::size_t rejected_track=max_tracks; std::uint64_t rejected_id=0;
    // Crucial two-pass boundary: ALL admission before ANY lane emits/reconciles.
    for(std::size_t i=0;i<lanes_.size();++i)
      if((rejected_id=lanes_[i]->rejected(*candidate->tracks[i]))) { rejected_track=i; break; }
    if(rejected_id) {
      decision={Decision::RejectedStartedOnset,candidate->ticket,candidate->revision,rejected_id,position_};
      decision.track_slot=rejected_track; retire=candidate;
    } else {
      retire=active_; active_=candidate; changed=true;
      render_end_=std::max(render_end_,candidate->frames);
      decision={Decision::Applied,candidate->ticket,candidate->revision,0,position_};
    }
  }
  result.frames=static_cast<std::uint32_t>(std::min<std::size_t>(frames,render_end_>position_?render_end_-position_:0));
  result.revision=active_->revision;
  for(std::size_t i=0;i<lanes_.size();++i) {
    blocks_[i]=lanes_[i]->process(*active_->tracks[i],changed,position_,result.frames);
    blocks_[i].revision=active_->revision; // derived from the ONE whole-plan revision
    if(lanes_[i]->failed_) failed_.store(true,std::memory_order_release);
  }
  if(candidate) {
    if(changed) {
      ++update_count_; end_frame_.store(render_end_,std::memory_order_release);
      applied_frame_.store(position_,std::memory_order_release); applied_.store(active_->revision,std::memory_order_release);
    }
    // Every lane has stopped accessing old plan memory before this retirement.
    // ACK is still before external DSP, allowing waiters to commit safely.
    const auto ticket=candidate->ticket;
    retired_.store(retire,std::memory_order_release); receipt_=decision;
    acknowledged_.store(ticket,std::memory_order_release);
  }
  position_+=result.frames; published_frame_.store(position_,std::memory_order_release);
  return result;
}
}
