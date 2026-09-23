#include "daw/live_performance.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
thread_local bool in_callback = false;
thread_local std::size_t callback_allocations = 0, callback_releases = 0, total_releases = 0;

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

template <typename F> void rejects(F action, const char* message) {
  bool rejected = false;
  try { action(); } catch (const std::exception&) { rejected = true; }
  require(rejected, message);
}

struct Note {
  std::uint64_t id;
  std::size_t on, off;
  std::uint8_t pitch = 60, velocity = 90, channel = 0;
};

daw::TimedMidiEvent cc(std::size_t frame, std::uint8_t controller, std::uint8_t value,
                      std::uint8_t channel = 0) {
  return {frame, static_cast<std::uint8_t>(0xb0 | channel), controller, value};
}

daw::MidiSampleSequence plan(std::initializer_list<Note> notes, std::size_t end = 1024,
                            std::vector<daw::TimedMidiEvent> controls = {}) {
  daw::MidiSampleSequence sequence;
  sequence.frames = end + 128;
  sequence.end_frame = end;
  sequence.events = std::move(controls);
  std::array<bool, 16> channels{};
  channels[0] = true;  // Keep terminal resets even in a temporary empty version.
  for (const auto& event : sequence.events) channels[event.status & 15] = true;
  for (const auto& note : notes) {
    channels[note.channel] = true;
    sequence.events.push_back({note.on, static_cast<std::uint8_t>(0x90 | note.channel),
                               note.pitch, note.velocity, note.id});
    sequence.events.push_back({note.off, static_cast<std::uint8_t>(0x80 | note.channel),
                               note.pitch, 19, note.id});
  }
  for (std::size_t channel = 0; channel < channels.size(); ++channel) if (channels[channel]) {
    for (const std::uint8_t controller : {64, 66, 69, 123}) {
      auto event = cc(end, controller, 0, static_cast<std::uint8_t>(channel));
      event.terminal_reset = true;
      sequence.events.push_back(event);
    }
  }
  std::stable_sort(sequence.events.begin(), sequence.events.end(),
                   [](const auto& a, const auto& b) { return a.frame < b.frame; });
  return sequence;
}

bool attack(const daw::TimedMidiEvent& event) {
  return (event.status & 0xf0) == 0x90 && event.data2;
}
bool release(const daw::TimedMidiEvent& event) {
  return (event.status & 0xf0) == 0x80 || ((event.status & 0xf0) == 0x90 && !event.data2);
}

struct Recorder {
  daw::LivePerformanceStream stream;
  std::vector<daw::TimedMidiEvent> events;
  explicit Recorder(daw::MidiSampleSequence sequence) : stream(std::move(sequence), .5) {}
  daw::LivePerformanceStream::Block read(std::uint32_t frames = 128) {
    const auto allocations_before = callback_allocations, releases_before = callback_releases;
    in_callback = true;
    const auto result = stream.nextBlock(frames);
    in_callback = false;
    require(callback_allocations == allocations_before && callback_releases == releases_before,
            "nextBlock allocated or destroyed a plan on the callback thread");
    require(!stream.failed(), "valid live stream failed");
    require(result.frames <= frames, "block exceeded requested frame count");
    for (std::size_t i = 0; i < result.count; ++i) {
      auto event = result.events[i];
      require(event.frame < result.frames, "MIDI offset is outside audio block");
      if (i) require(event.frame >= result.events[i-1].frame, "MIDI offsets are unordered");
      event.frame += result.frame;
      events.push_back(event);
    }
    require(stream.frame() == result.frame + result.frames, "transport skipped or restarted a block");
    return result;
  }
  std::vector<daw::TimedMidiEvent> notes(std::uint64_t id, bool on) const {
    std::vector<daw::TimedMidiEvent> found;
    for (const auto& event : events)
      if (event.note_id == id && (on ? attack(event) : release(event))) found.push_back(event);
    return found;
  }
  void exactly(std::uint64_t id, std::size_t on, std::size_t off) const {
    const auto attacks = notes(id, true), releases = notes(id, false);
    require(attacks.size() == 1 && attacks[0].frame == on, "performed ID attacked twice or at wrong frame");
    require(releases.size() == 1 && releases[0].frame == off, "performed ID released twice or at wrong frame");
  }
};

void extendHeldNote() {
  Recorder r(plan({{1, 32, 192}}, 512));
  r.read();
  r.stream.submit(plan({{1, 32, 384}}, 512), .4, 1);
  const auto applied = r.read();
  require(applied.revision == 1 && applied.gain == .4 && applied.frame == 128 &&
          r.stream.appliedRevision() == 1 && r.stream.appliedFrame() == 128,
          "accepted revision was not applied at the next block boundary");
  require(r.notes(1, false).empty(), "old plan release cut an extended held note");
  r.read(256);
  r.exactly(1, 32, 384);
}

void shortenAndDelete() {
  {
    Recorder r(plan({{1, 32, 320}}));
    r.read();
    r.stream.submit(plan({{1, 32, 64}}), .5, 1);
    r.read(); r.read(256);
    r.exactly(1, 32, 128);
  }
  {
    Recorder r(plan({{1, 16, 400}}, 768, {cc(0,64,127)}));
    r.read();
    r.stream.submit(plan({}, 768, {cc(0,64,127)}), .5, 1);
    const auto before = r.events.size();
    r.read(); r.read(256);
    r.exactly(1, 16, 128);
    require(r.events.size() == before + 1 && release(r.events.back()),
            "deleting a held note reset pedal or panicked the channel");
  }
}

void chaseControllersAndBoundaries() {
  Recorder r(plan({{1,16,400}}, 768, {cc(0,64,0),cc(0,11,70)}));
  r.read();
  const auto before = r.events.size();
  r.stream.submit(plan({{1,16,400}}, 768,
      {cc(0,64,0),cc(0,11,70),cc(64,64,127),cc(64,11,95),cc(128,11,100),cc(256,64,0)}), .5, 1);
  r.read();
  unsigned pedal_chased = 0, expression_boundary = 0;
  int expression = -1;
  for (std::size_t i = before; i < r.events.size(); ++i) {
    const auto& e = r.events[i];
    require(e.frame == 128 && e.status == 0xb0, "controller chase changed note ownership or frame");
    if (e.data1 == 64 && e.data2 == 127) ++pedal_chased;
    if (e.data1 == 11) { expression = e.data2; if (e.data2 == 100) ++expression_boundary; }
  }
  require(pedal_chased == 1 && expression_boundary == 1 && expression == 100,
          "past controller state or exact-boundary controller was missing/duplicated");
  r.read();
  require(r.events.back().frame == 256 && r.events.back().data1 == 64 && r.events.back().data2 == 0,
          "next block controller was dispatched in the previous block or lost");
  r.read(); r.exactly(1,16,400);

  Recorder past_expression(plan({{1,0,400}},768,{cc(0,11,70)}));
  past_expression.read();
  past_expression.stream.submit(plan({{1,0,400}},768,{cc(0,11,70),cc(64,11,95)}),.5,1);
  past_expression.read();
  require(past_expression.events.back().frame == 128 && past_expression.events.back().data1 == 11 &&
          past_expression.events.back().data2 == 95, "past CC11 point was not chased at the next block");

  Recorder boundary(plan({{1,128,256}},512));
  boundary.read();
  require(boundary.notes(1,true).empty(), "attack at exclusive block end dispatched early");
  const auto boundary_before = boundary.events.size();
  boundary.stream.submit(plan({{1,128,256}},512,{cc(128,11,81)}),.5,1);
  boundary.read();
  require(boundary.events.size() == boundary_before + 2 && boundary.events[boundary_before].data1 == 11 &&
          attack(boundary.events[boundary_before + 1]) && boundary.notes(1,false).empty(),
          "same-frame source order or exclusive release boundary changed");
  boundary.read(); boundary.exactly(1,128,256);

  Recorder release_boundary(plan({{1,0,128}},512));
  release_boundary.read();
  release_boundary.stream.submit(plan({{1,0,128}},512),.5,1);
  release_boundary.read();
  release_boundary.exactly(1,0,128);
  require(release_boundary.notes(1,false)[0].data2 == 19,
          "unchanged source release at replacement boundary lost its velocity");
}

void exactBoundaryPreservesSourceOrderBeforeCatchUp() {
  Recorder r(plan({{1,128,256},{2,512,640,64}},768,{cc(0,11,70)}));
  r.read();
  auto replacement = plan({{1,128,256},{2,64,384,64}},768,
      {cc(0,11,70),cc(128,11,95),cc(128,64,127),cc(128,64,0),cc(128,64,90)});
  // The general fixture helper groups controller inputs before note inputs.
  // Deliberately author a different legal source order at the exact boundary:
  // attack, expression, pedal-down/up/down. Reconciliation cannot reorder it.
  const auto first = std::find_if(replacement.events.begin(),replacement.events.end(),
                                [](const auto& e){return e.frame == 128;});
  const auto note = std::find_if(first,replacement.events.end(),
                               [](const auto& e){return attack(e) && e.note_id == 1;});
  require(first != replacement.events.end() && note != replacement.events.end(),
          "source ordering regression fixture is incomplete");
  std::rotate(first,note,note+1);
  const auto before = r.events.size();
  r.stream.submit(std::move(replacement),.5,1);
  r.read();
  require(r.events.size() == before+6, "boundary source events or catch-up duplicated/lost");
  const auto* e = r.events.data()+before;
  require(attack(e[0]) && e[0].note_id == 1 &&
          e[1].status == 0xb0 && e[1].data1 == 11 && e[1].data2 == 95 &&
          e[2].status == 0xb0 && e[2].data1 == 64 && e[2].data2 == 127 &&
          e[3].status == 0xb0 && e[3].data1 == 64 && e[3].data2 == 0 &&
          e[4].status == 0xb0 && e[4].data1 == 64 && e[4].data2 == 90 &&
          attack(e[5]) && e[5].note_id == 2,
          "boundary chase reordered source attack/expression/pedal transitions or catch-up");
  for (std::size_t i = 0; i < 6; ++i)
    require(e[i].frame == 128, "same-frame source ordering changed the event frame");
  r.read(); r.read();
  r.exactly(1,128,256); r.exactly(2,128,384);
}

void onsetEditsPreserveAttackOwnership() {
  {
    Recorder r(plan({{1,16,400}},768));
    r.read();
    r.stream.submit(plan({{1,320,480}},768),.5,1);
    r.read(); r.read(256);
    r.exactly(1,16,480);
  }
  {
    Recorder r(plan({{1,16,64}},768));
    r.read();
    r.stream.submit(plan({{1,320,480}},768),.5,1);
    r.read(); r.read(256);
    r.exactly(1,16,64);
  }
  {
    Recorder r(plan({{1,512,640}},768));
    r.read(256);
    r.stream.submit(plan({{1,128,384}},768),.5,1);
    r.read(256);
    r.exactly(1,256,384);
  }
}

void mailboxBackpressureAndControlReclamation() {
  Recorder r(plan({{1,0,1000}},1200));
  r.stream.submit(plan({{1,0,900}},1200),.4,1);
  rejects([&]{r.stream.submit(plan({{1,0,800}},1200),.3,2);},
          "rapid submission replaced a pending accepted revision");
  require(r.stream.appliedRevision() == 0, "producer prematurely marked plan applied");
  require(r.read().revision == 1, "first queued revision was overwritten");
  const auto releases_before = total_releases;
  r.stream.collectRetired();
  require(total_releases > releases_before, "retired plan was not reclaimed on control thread");
  r.stream.submit(plan({{1,0,800}},1200),.3,2);
  const auto result = r.read();
  require(result.revision == 2 && result.frame == 128 && result.gain == .3,
          "mailbox did not accept next edit after retirement");
  rejects([&]{r.stream.submit(plan({{1,0,700}},1200),.2,2);},
          "nonmonotonic revision was accepted");
  r.stream.submit(plan({{1,0,700}},1200),.2,3); // submit itself may collect the retired plan.
  require(r.read().revision == 3 && r.stream.appliedFrame() == 256,
          "automatic retirement failed or rejected submission consumed a revision");
}

void terminalResetsFollowEditedEnd() {
  Recorder r(plan({{1,0,128}},256,{cc(0,64,127)}));
  r.read();
  r.stream.submit(plan({{1,0,512}},768,{cc(0,64,127)}),.5,1);
  r.read(); r.read(256); r.read(256);
  require(r.stream.endFrame() == 896, "new audio tail boundary was not published");
  require(std::none_of(r.events.begin(),r.events.end(),[](const auto& e){return e.terminal_reset;}),
          "old terminal reset cut the edited performance before its new end");
  r.read();
  r.exactly(1,0,512);
  std::vector<unsigned> resets;
  for (const auto& e : r.events) if (e.terminal_reset) {
    require(e.frame == 768 && e.data2 == 0, "terminal reset was not moved to edited end");
    resets.push_back(e.data1);
  }
  require(resets == std::vector<unsigned>({64,66,69,123}), "terminal reset duplicated or lost");
  require(r.read().frames == 0, "completed transport unexpectedly restarted");
}

void preserveActualKeyOwnerAcrossNonoverlappingPlans() {
  // Each new plan is individually nonoverlapping. Its edited future A onset
  // does not erase the A attack already sent to the plugin, so B must not
  // acquire the same channel/pitch and later release A's actual key.
  for (const bool catch_up : {true, false}) {
    Recorder r(plan({{1,0,512},{2,640,800}},1024));
    r.read(catch_up ? 256 : 128);
    r.stream.submit(catch_up ? plan({{1,400,600},{2,128,320}},1024) :
                              plan({{1,400,600},{2,200,300}},1024), .5, 1);
    r.read(256); r.read(256);
    r.exactly(1,0,600);
    require(r.notes(2,true).empty() && r.notes(2,false).empty(),
            "new plan's same-key attack/release stole an already sounding note");
    // There is no concurrent callback here; this diagnostic is documented as
    // after-stop because its production accessor deliberately is not atomic.
    require(r.stream.suppressedConflictsAfterStop() == 1,
            "suppressed same-key attack was not reported exactly once");
  }
}

void shorteningKeepsExistingAudioTail() {
  Recorder r(plan({{1,16,600}},768));
  r.read(256);
  r.stream.submit(plan({{1,16,64}},128),.5,1);
  require(r.read().frames == 128 && r.stream.endFrame() == 896,
          "shorter plan truncated the already sounding instrument tail");
  r.read(256); r.read(256);
  r.exactly(1,16,256);
  require(r.stream.frame() == 896 && r.read().frames == 0,
          "preserved tail did not terminate at the original render boundary");
}

void completedRunAppliesSameLengthRevisionSilently() {
  Recorder r(plan({{1,0,64}},128,{cc(0,11,70)}));
  r.read(256);
  const auto capture_capacity = r.stream.endFrame();
  require(capture_capacity == 256 && r.stream.frame() == capture_capacity,
          "completed-run fixture did not reach its reserved render/capture end");
  const auto before = r.events.size();
  for (std::uint64_t revision = 1; revision <= 2; ++revision) {
    r.stream.submit(plan({{1,0,64}},128,
        {cc(0,11,static_cast<std::uint8_t>(90+revision))}),.25,revision);
    const auto applied = r.read();
    require(applied.frames == 0 && applied.count == 0 && applied.frame == capture_capacity &&
            applied.revision == revision && applied.gain == .25 &&
            r.stream.appliedRevision() == revision && r.stream.appliedFrame() == capture_capacity,
            "completed run rendered a controller-chase block or failed to acknowledge revision");
    require(r.stream.frame() == capture_capacity && r.stream.endFrame() == capture_capacity &&
            r.events.size() == before && r.read().frames == 0,
            "same-length edit advanced beyond completed render/capture capacity");
  }
  r.exactly(1,0,64);
}

void rejectInvalidLivePlansAtomically() {
  Recorder r(plan({{1,0,128},{2,256,384}},768));
  rejects([&]{r.stream.submit(plan({{1,0,128},{3,256,384}},768),.5,1);},
          "live update expanded performed-ID namespace");
  rejects([&]{r.stream.submit(plan({{1,0,256},{2,256,384}},768),.5,1);},
          "coincident same-pitch release and attack accepted");
  rejects([&]{r.stream.submit(plan({{1,0,300},{2,256,384}},768),.5,1);},
          "same-pitch overlap accepted");
  auto invalid = plan({{1,0,128},{2,256,384}},768);
  invalid.events.erase(invalid.events.begin()+1); // Remove first note's release.
  rejects([&]{r.stream.submit(invalid,.5,1);}, "unpaired note update accepted");
  r.stream.submit(plan({{1,0,160},{2,256,384}},768),.5,1);
  require(r.read().revision == 1, "invalid update mutated publication or consumed revision");
  r.read(); r.read(); r.read();
  r.exactly(1,0,160); r.exactly(2,256,384);
}

void guardedOnsetDecisionsAreAtomic() {
  using Decision = daw::LivePerformanceStream::Decision;
  Recorder held(plan({{1,32,400}},768,{cc(0,64,127),cc(0,11,70)}));
  held.read();
  const auto before = held.events.size();
  const auto original_end = held.stream.endFrame();
  const auto candidate = plan({{1,320,600}},1024,{cc(0,64,0),cc(0,11,12)});
  const auto ticket = held.stream.submit(candidate,.125,1,{1});
  const auto pending = held.stream.receipt(ticket);
  require(pending.decision == Decision::Pending && pending.ticket == ticket &&
          pending.publish_begin_frame == 128 && pending.publish_end_frame == 128 &&
          held.stream.hasPendingUpdate() && held.stream.appliedRevision() == 0,
          "guarded publication was mistaken for callback acceptance");
  rejects([&]{held.stream.submit(candidate,.25,2,{1});},
          "pending guarded request was overwritten before its decision");
  const auto unchanged = held.read();
  const auto rejected = held.stream.waitForDecision(ticket,std::chrono::milliseconds(0));
  require(rejected.decision == Decision::RejectedStartedOnset && rejected.ticket == ticket &&
          rejected.revision == 1 && rejected.note_id == 1 && rejected.frame == 128 &&
          rejected.publish_begin_frame == 128 && rejected.publish_end_frame == 128,
          "held onset edit lacked its precise rejection receipt");
  require(unchanged.revision == 0 && unchanged.gain == .5 && held.stream.appliedRevision() == 0 &&
          held.stream.appliedFrame() == 0 && held.stream.endFrame() == original_end &&
          !held.stream.hasPendingUpdate() && held.events.size() == before &&
          held.stream.liveUpdateCountAfterStop() == 0,
          "guard rejection changed notes, controllers, gain, tail or applied revision");
  require(!held.stream.cancelPending(ticket), "accepted rejection could still be cancelled");

  // Rejection consumes a mailbox ticket, not an editor revision. The same
  // rejected revision can be retried, with no ABA from its old receipt.
  const auto retry = held.stream.submit(candidate,.125,1,{1,1});
  require(retry > ticket, "retry reused an old request identity");
  rejects([&]{(void)held.stream.receipt(ticket);}, "stale request receipt was accepted after retry");
  held.read();
  require(held.stream.waitForDecision(retry).decision == Decision::RejectedStartedOnset &&
          held.stream.appliedRevision() == 0 && held.events.size() == before,
          "repeated guard rejection consumed editor revision or emitted candidate controls");
  held.read();
  held.exactly(1,32,400);
  const auto accepted = held.stream.submit(plan({{1,32,400}},768,{cc(0,64,127),cc(0,11,70)}),.25,1);
  require(accepted > retry, "unguarded retry did not receive an independent ticket");
  const auto accepted_block = held.read();
  const auto applied = held.stream.waitForDecision(accepted);
  require(applied.decision == Decision::Applied && applied.revision == 1 && applied.note_id == 0 &&
          applied.frame == 512 && accepted_block.revision == 1 && accepted_block.gain == .25 &&
          applied.publish_begin_frame == 512 && applied.publish_end_frame == 512 &&
          held.stream.appliedRevision() == 1 && held.stream.liveUpdateCountAfterStop() == 1,
          "valid retry after rejection failed to accept the same editor revision");
  held.exactly(1,32,400);

  Recorder released(plan({{1,16,64}},768,{cc(0,11,70)}));
  released.read();
  const auto released_before = released.events.size();
  const auto released_ticket = released.stream.submit(plan({{1,320,480}},768,{cc(0,11,10)}),.1,1,{1});
  released.read();
  require(released.stream.waitForDecision(released_ticket).decision == Decision::RejectedStartedOnset &&
          released.stream.appliedRevision() == 0 && released.events.size() == released_before,
          "released onset guard checked only held voices and resurrected old history");
  released.read(256);
  released.exactly(1,16,64);

  Recorder future(plan({{1,512,640}},768,{cc(0,11,70)}));
  future.read();
  const auto future_ticket = future.stream.submit(plan({{1,256,384}},768,{cc(0,11,90)}),.25,1,{1});
  const auto future_block = future.read();
  const auto future_receipt = future.stream.waitForDecision(future_ticket);
  require(future_receipt.decision == Decision::Applied && future_receipt.frame == 128 &&
          future_receipt.publish_begin_frame == 128 && future_receipt.publish_end_frame == 128 &&
          future_block.revision == 1 && future_block.gain == .25 && future.notes(1,true).empty() &&
          future.events.back().frame == 128 && future.events.back().data1 == 11 && future.events.back().data2 == 90,
          "unstarted guarded onset edit was rejected or failed to apply atomically");
  future.read(256);
  future.exactly(1,256,384);
}

void guardedCancellationReclaimsOnlyUnclaimedPlans() {
  using Decision = daw::LivePerformanceStream::Decision;
  Recorder r(plan({{1,256,512}},768,{cc(0,11,70)}));
  rejects([&]{r.stream.submit(plan({{1,256,512}},768),.5,1,{99});},
          "unknown guarded identity was admitted");
  require(!r.stream.hasPendingUpdate(), "invalid guard published an unacknowledgeable request");
  const auto ticket = r.stream.submit(plan({{1,128,384}},768,{cc(0,11,5)}),.1,1,{1});
  const auto releases_before = total_releases;
  const auto receipt = r.stream.waitForDecision(ticket,std::chrono::milliseconds(0));
  require(receipt.decision == Decision::Cancelled && receipt.ticket == ticket &&
          receipt.revision == 1 && receipt.note_id == 0 && receipt.frame == 0 &&
          receipt.publish_begin_frame == 0 && receipt.publish_end_frame == 0 &&
          !r.stream.hasPendingUpdate() && r.stream.appliedRevision() == 0 && total_releases > releases_before,
          "zero-timeout unclaimed cancellation failed to reclaim on the control thread");
  require(!r.stream.cancelPending(ticket), "cancelled candidate was reclaimed twice");
  r.read(); r.read(); r.read(); r.read(); r.read();
  r.exactly(1,256,512);
  require(std::none_of(r.events.begin(),r.events.end(),[](const auto& event) {
    return event.status == 0xb0 && event.data1 == 11 && event.data2 == 5;
  }), "cancelled candidate later reached the callback");
  const auto retry = r.stream.submit(plan({{1,256,512}},768,{cc(0,11,70)}),.25,1);
  require(retry > ticket, "cancelled revision retry reused its mailbox ticket");
  r.read();
  require(r.stream.waitForDecision(retry,std::chrono::milliseconds(0)).decision == Decision::Applied &&
          !r.stream.cancelPending(retry), "already applied request was retroactively cancelled");
}

void guardedOnsetLockSurvivesRepitchAndSuppression() {
  using Decision = daw::LivePerformanceStream::Decision;
  {
    Recorder r(plan({{1,0,1000,60}},1200));
    r.read(256);
    // This low-level repitch is already over at the swap. It releases the
    // real C4 and clears replay ownership, but cannot erase the fact that ID 1
    // has consumed its attack in this transport pass.
    r.stream.submit(plan({{1,0,100,62}},1200),.5,1);
    r.read();
    r.exactly(1,0,256);
    const auto ticket = r.stream.submit(plan({{1,512,800,62}},1200),.5,2,{1});
    r.read();
    const auto decision = r.stream.waitForDecision(ticket);
    require(decision.decision == Decision::RejectedStartedOnset && decision.note_id == 1 &&
            r.stream.appliedRevision() == 1,
            "repitch/shortening reset allowed a previously played ID to bypass the onset guard");
    r.read(256); r.read(256);
    r.exactly(1,0,256);
  }
  {
    Recorder r(plan({{1,0,512},{2,640,800}},1024));
    r.read(256);
    r.stream.submit(plan({{1,400,600},{2,128,320}},1024),.5,1);
    r.read(256);
    require(r.stream.suppressedConflictsAfterStop() == 1 && r.notes(2,true).empty(),
            "guard suppression fixture did not consume an unplayed conflicting attack");
    const auto ticket = r.stream.submit(plan({{1,400,600},{2,700,900}},1024),.5,2,{2});
    r.read(256);
    const auto decision = r.stream.waitForDecision(ticket);
    require(decision.decision == Decision::RejectedStartedOnset && decision.note_id == 2 &&
            r.stream.appliedRevision() == 1 && r.notes(2,true).empty() && r.notes(2,false).empty(),
            "suppressed but consumed onset bypassed the conservative live-edit guard");
    r.exactly(1,0,600);
    require(r.stream.suppressedConflictsAfterStop() == 1,
            "rejected suppressed-ID edit created another conflict");
  }
}

void concurrentGuardRejectionAndRetry() {
  using Decision = daw::LivePerformanceStream::Decision;
  constexpr std::uint64_t revisions = 300;
  constexpr std::size_t musical_end = 48000U * 60 * 29;
  daw::LivePerformanceStream stream(plan({{1,0,musical_end-512}},musical_end,{cc(0,11,70)}),.5);
  std::atomic<bool> stop{false}, started{false};
  std::atomic<unsigned> failure{0};
  std::atomic<std::size_t> allocations{0}, releases{0}, attacks{0}, accepted_controls{0}, leaked_controls{0};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  std::thread audio([&] {
    std::size_t next_frame = 0;
    std::uint64_t previous_revision = 0;
    while (!stop.load(std::memory_order_acquire) && !failure.load(std::memory_order_relaxed)) {
      if (std::chrono::steady_clock::now() >= deadline) { failure.store(1); break; }
      in_callback = true;
      const auto block = stream.nextBlock(256);
      in_callback = false;
      if (stream.failed() || block.frame != next_frame || block.frames > 256 ||
          block.revision < previous_revision || block.revision > previous_revision+1 ||
          block.gain != (block.revision ? .25 : .5)) { failure.store(2); break; }
      previous_revision = block.revision;
      next_frame += block.frames;
      for (std::size_t i = 0; i < block.count; ++i) {
        const auto& event = block.events[i];
        if (event.frame >= block.frames) { failure.store(3); break; }
        if (attack(event) && event.note_id == 1) attacks.fetch_add(1);
        if (event.status == 0xb0 && event.data1 == 11 && event.data2 == 1) leaked_controls.fetch_add(1);
        if (event.status == 0xb0 && event.data1 == 11 && (event.data2 == 80 || event.data2 == 81))
          accepted_controls.fetch_add(1);
      }
      if (callback_allocations || callback_releases) { failure.store(4); break; }
      started.store(true,std::memory_order_release);
      std::this_thread::yield();
    }
    allocations.store(callback_allocations);
    releases.store(callback_releases);
  });
  bool completed = false;
  try {
    while (!started.load(std::memory_order_acquire) && !failure.load() &&
           std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    require(started.load() && !failure.load(), "guard concurrent fixture never attacked its held note");
    std::uint64_t prior_ticket = 0;
    for (std::uint64_t revision = 1; revision <= revisions; ++revision) {
      const auto rejected_ticket = stream.submit(plan({{1,256,musical_end-512}},musical_end,{cc(0,11,1)}),.1,revision,{1});
      const auto rejected = stream.waitForDecision(rejected_ticket);
      require(rejected_ticket > prior_ticket && rejected.decision == Decision::RejectedStartedOnset &&
              rejected.ticket == rejected_ticket && rejected.revision == revision && rejected.note_id == 1 &&
              stream.appliedRevision() == revision-1 && !stream.hasPendingUpdate(),
              "concurrent rejection mutated applied revision or returned an unrelated ACK");
      require(rejected.publish_begin_frame <= rejected.publish_end_frame &&
              rejected.frame >= rejected.publish_begin_frame && rejected.frame <= rejected.publish_end_frame+256,
              "concurrent rejection missed the next block after publication (excluding compile/ACK wait)");
      const auto retry_ticket = stream.submit(plan({{1,0,musical_end-512}},musical_end,
          {cc(0,11,static_cast<std::uint8_t>(80+revision%2))}),.25,revision);
      const auto accepted = stream.waitForDecision(retry_ticket);
      require(retry_ticket > rejected_ticket && accepted.decision == Decision::Applied &&
              accepted.ticket == retry_ticket && accepted.revision == revision &&
              stream.appliedRevision() == revision && !stream.hasPendingUpdate(),
              "concurrent retry failed to accept the rejected editor revision on a new ticket");
      require(accepted.publish_begin_frame <= accepted.publish_end_frame &&
              accepted.frame >= accepted.publish_begin_frame && accepted.frame <= accepted.publish_end_frame+256,
              "concurrent accepted retry missed the next block after publication (excluding compile/ACK wait)");
      prior_ticket = retry_ticket;
      if (failure.load() || std::chrono::steady_clock::now() >= deadline) break;
      completed = revision == revisions;
    }
  } catch (...) {
    stop.store(true,std::memory_order_release);
    audio.join();
    throw;
  }
  stop.store(true,std::memory_order_release);
  audio.join();
  stream.collectRetired();
  require(completed && failure.load() == 0 && allocations.load() == 0 && releases.load() == 0,
          "concurrent guard rejection/retry lost order, timed out or allocated on callback");
  require(attacks.load() == 1 && leaked_controls.load() == 0 && accepted_controls.load() == revisions &&
          stream.liveUpdateCountAfterStop() == revisions,
          "rejected candidate leaked an attack/controller or lost an accepted retry");
}

void concurrentPublicationAndReclamation() {
  constexpr std::uint64_t revisions = 1000;
  constexpr std::size_t musical_end = 48000U * 60 * 29;
  daw::LivePerformanceStream stream(plan({{1,0,musical_end-512}},musical_end),.5);
  std::array<std::atomic<unsigned>,revisions+1> seen{};
  for (auto& entry : seen) entry.store(0);
  std::atomic<bool> stop{false};
  std::atomic<unsigned> failure{0};
  std::atomic<std::uint64_t> observed_revision{0};
  std::atomic<std::size_t> allocations{0}, releases{0};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  std::thread audio([&] {
    std::uint64_t previous_revision = 0;
    std::size_t next_frame = 0;
    while (!stop.load(std::memory_order_acquire) && !failure.load(std::memory_order_relaxed)) {
      if (std::chrono::steady_clock::now() >= deadline) { failure.store(1); break; }
      in_callback = true;
      const auto block = stream.nextBlock(256);
      in_callback = false;
      // No assertions, throws, containers, logging or destruction on the audio
      // thread: failures and audit counts are returned to the control thread.
      if (stream.failed() || block.frame != next_frame || block.frames > 256) {
        failure.store(2); break;
      }
      next_frame += block.frames;
      if (block.revision < previous_revision || block.revision > revisions ||
          block.revision > previous_revision+1) { failure.store(3); break; }
      for (std::size_t i = 0; i < block.count; ++i) {
        if (block.events[i].frame >= block.frames ||
            (i && block.events[i].frame < block.events[i-1].frame)) {
          failure.store(4); break;
        }
      }
      if (block.revision != previous_revision) {
        seen[block.revision].fetch_add(1,std::memory_order_relaxed);
        previous_revision = block.revision;
        observed_revision.store(previous_revision,std::memory_order_release);
      }
      if (callback_allocations || callback_releases) { failure.store(5); break; }
      // Yield outside nextBlock so the producer can exercise publication and
      // reclamation, without depending on any particular scheduler timeslice.
      std::this_thread::yield();
    }
    allocations.store(callback_allocations);
    releases.store(callback_releases);
  });
  bool producer_completed = false;
  try {
    for (std::uint64_t revision = 1; revision <= revisions; ++revision) {
      const auto next = plan({{1,0,musical_end-512}},musical_end,
                            {cc(0,11,static_cast<std::uint8_t>(40+revision%80))});
      bool published = false;
      while (!published && !failure.load() && std::chrono::steady_clock::now() < deadline) {
        try { stream.submit(next,.5,revision); published = true; }
        catch (const std::invalid_argument&) {
          // Seeing appliedRevision alone does not wait for the complete
          // receipt/retirement ACK. Backpressure persists until that decision
          // is published and the control thread can reclaim its retired plan.
          std::this_thread::yield();
        }
      }
      if (!published) break;
      while (stream.appliedRevision() < revision && !failure.load() &&
             std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
      if (failure.load() || stream.appliedRevision() != revision) break;
      if (revision == revisions) {
        while (observed_revision.load(std::memory_order_acquire) != revisions && !failure.load() &&
               std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        producer_completed = observed_revision.load() == revisions;
      }
    }
  } catch (...) {
    stop.store(true,std::memory_order_release);
    audio.join();
    throw;
  }
  stop.store(true,std::memory_order_release);
  audio.join();
  stream.collectRetired();
  require(producer_completed && failure.load() == 0,
          "concurrent plan exchange timed out, lost order, or corrupted a callback block");
  require(allocations.load() == 0 && releases.load() == 0,
          "concurrent callback allocated or reclaimed a plan");
  for (std::uint64_t revision = 1; revision <= revisions; ++revision)
    require(seen[revision].load() == 1, "accepted concurrent revision was lost or applied twice");
  require(stream.appliedRevision() == revisions, "concurrent exchange ended on a stale revision");
}
} // namespace

void* operator new(std::size_t n) {
  if (in_callback) ++callback_allocations;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept {
  if (p) { ++total_releases; if (in_callback) ++callback_releases; }
  std::free(p);
}
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

int main() {
  try {
    extendHeldNote();
    shortenAndDelete();
    chaseControllersAndBoundaries();
    exactBoundaryPreservesSourceOrderBeforeCatchUp();
    onsetEditsPreserveAttackOwnership();
    mailboxBackpressureAndControlReclamation();
    terminalResetsFollowEditedEnd();
    preserveActualKeyOwnerAcrossNonoverlappingPlans();
    shorteningKeepsExistingAudioTail();
    completedRunAppliesSameLengthRevisionSilently();
    rejectInvalidLivePlansAtomically();
    guardedOnsetDecisionsAreAtomic();
    guardedCancellationReclaimsOnlyUnclaimedPlans();
    guardedOnsetLockSurvivesRepitchAndSuppression();
    concurrentGuardRejectionAndRetry();
    concurrentPublicationAndReclamation();
    require(callback_allocations == 0 && callback_releases == 0, "callback allocation audit failed");
    std::cout << "Live plan identity, hanging-note ownership, controller chase, block boundary, "
                 "guarded ACK/rejection/cancellation, backpressure, retirement and allocation tests passed\n";
    return 0;
  } catch (const std::exception& e) {
    in_callback = false;
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
}
