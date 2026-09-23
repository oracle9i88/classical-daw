#pragma once

#include "daw/midi_sequence.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <memory>

namespace daw {
// Single control producer / single audio consumer. Both must stop before
// destruction. Plans are created and destroyed ONLY on the control thread.
// Live edits keep the initial performed-ID namespace; adding IDs/take switching
// requires a new stopped transport. Notes may disappear and return in that set.
class LivePerformanceStream {
 public:
  static constexpr std::size_t max_notes = 4096;
  static constexpr std::size_t max_messages = 16384;
  struct Block {
    const TimedMidiEvent* events = nullptr;
    std::size_t count = 0, frame = 0;
    std::uint32_t frames = 0;
    double gain = 1;
    std::uint64_t revision = 0;
  };
  enum class Decision { Pending, Applied, RejectedStartedOnset, Cancelled };
  struct Receipt {
    Decision decision = Decision::Pending;
    std::uint64_t ticket = 0, revision = 0, note_id = 0;
    std::size_t frame = 0;
    // Control-thread observations immediately around the release publication,
    // excluding compile time and the later synchronous ACK wait.
    std::size_t publish_begin_frame = 0, publish_end_frame = 0;
  };
  LivePerformanceStream(MidiSampleSequence sequence, double gain, std::uint64_t revision = 0);
  ~LivePerformanceStream();
  LivePerformanceStream(const LivePerformanceStream&) = delete;
  LivePerformanceStream& operator=(const LivePerformanceStream&) = delete;
  // Control only. A full two-slot mailbox rejects rather than overwriting.
  // Guarded edits are transactions: wait for the callback's receipt BEFORE
  // accepting an editor command. An already-started guarded ID rejects the
  // whole plan, including gain/controllers. Empty guards retain the low-level
  // forced-plan API used by the key-ownership diagnostic probe.
  std::uint64_t submit(MidiSampleSequence sequence, double gain, std::uint64_t revision,
                       const std::vector<std::uint64_t>& reject_started_onsets = {});
  Receipt receipt(std::uint64_t ticket) const; // latest ticket, control only
  bool cancelPending(std::uint64_t ticket);   // false once callback has claimed it
  Receipt waitForDecision(std::uint64_t ticket,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(1000));
  void collectRetired() noexcept;
  Block nextBlock(std::uint32_t frames) noexcept;
  std::uint64_t appliedRevision() const noexcept { return applied_.load(std::memory_order_acquire); }
  std::size_t appliedFrame() const noexcept { return applied_frame_.load(std::memory_order_acquire); }
  std::size_t frame() const noexcept { return published_frame_.load(std::memory_order_acquire); }
  std::size_t endFrame() const noexcept { return end_frame_.load(std::memory_order_acquire); }
  bool failed() const noexcept { return failed_.load(std::memory_order_acquire); }
  bool hasPendingUpdate() const noexcept {
    return acknowledged_.load(std::memory_order_acquire) != requested_.load(std::memory_order_acquire);
  }
  std::uint64_t suppressedConflictsAfterStop() const noexcept { return suppressed_conflicts_; }
  std::uint64_t liveUpdateCountAfterStop() const noexcept { return update_count_; }
 private:
  struct Plan;
  struct Voice {
    bool fired = false, down = false;
    bool onset_locked = false; // sticky this pass: attacked OR consumed by conflict
    std::uint8_t channel = 0, pitch = 0;
  };
  std::unique_ptr<Plan> prepare(MidiSampleSequence sequence, double gain, std::uint64_t revision) const;
  void emit(TimedMidiEvent event, std::size_t slot, std::size_t& count) noexcept;
  void switchPlan(const Plan& next, std::size_t& count) noexcept;
  void catchUp(const Plan& next, std::size_t& count) noexcept;
  std::vector<std::uint64_t> ids_;
  std::vector<TimedMidiEvent> fixed_events_;
  std::array<std::unique_ptr<Plan>, 2> owned_;
  const Plan* active_ = nullptr; // audio thread only
  std::atomic<const Plan*> pending_{nullptr}, retired_{nullptr};
  std::array<Voice, max_notes> voices_{};
  std::array<std::array<std::size_t, 128>, 16> key_owner_{};
  std::array<std::array<int, 2>, 16> controls_{}; // CC64, CC11 actually sent
  std::array<TimedMidiEvent, max_messages> block_{};
  std::size_t position_ = 0, cursor_ = 0, render_end_ = 0;
  std::uint64_t suppressed_conflicts_ = 0, update_count_ = 0;
  bool initialized_controls_ = false;
  std::uint64_t submitted_revision_ = 0; // control thread only
  std::uint64_t submitted_ticket_ = 0;   // requests are independent of editor revisions
  const Plan* submitted_plan_ = nullptr; // control only, owned until ACK/reclamation
  std::size_t publish_begin_frame_ = 0, publish_end_frame_ = 0; // control only
  Receipt receipt_; // callback writes; control reads only after acquire ACK
  std::atomic<std::uint64_t> applied_{0};
  std::atomic<std::uint64_t> requested_{0}, acknowledged_{0}; // request tickets
  std::atomic<std::size_t> applied_frame_{0}, published_frame_{0}, end_frame_{0};
  std::atomic<bool> failed_{false};
};
}
