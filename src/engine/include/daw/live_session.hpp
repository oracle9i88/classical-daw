#pragma once
#include "daw/midi_sequence.hpp"
#include "daw/performance.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>

namespace daw {
struct LiveTrackPlan {
  std::string track_id;
  MidiSampleSequence sequence;
  double gain = 1;
  std::vector<std::uint64_t> reject_started_onsets;
  bool audible = true;
  std::int64_t track_delay_us = 0;
};

// One producer, one audio consumer, one whole-session publication and ACK.
// Lane ledgers have no mailboxes, owned plans, revisions or clocks of their own.
// Topology/ID namespaces fixed for this stopped transport; all plans are built
// and destroyed on the control thread. Stop BOTH threads before destruction.
class LiveSessionStream {
 public:
  static constexpr std::size_t max_tracks = 64, max_notes = kMaxAuditionAttacks, max_messages = 16384;
  struct TrackBlock {
    const TimedMidiEvent* events = nullptr;
    std::size_t count = 0, frame = 0;
    std::uint32_t frames = 0;
    double gain = 1;
    std::uint64_t revision = 0;
    bool audible = true;
  };
  struct Block {
    const TrackBlock* tracks = nullptr;
    std::size_t count = 0, frame = 0;
    std::uint32_t frames = 0;
    std::uint64_t revision = 0;
  };
  enum class Decision { Pending, Applied, RejectedStartedOnset, Cancelled };
  struct Receipt {
    Decision decision = Decision::Pending;
    std::uint64_t ticket = 0, revision = 0, note_id = 0;
    std::size_t frame = 0, publish_begin_frame = 0, publish_end_frame = 0;
    // Immutable slot -> stable trackId(); no string allocation in callback.
    std::size_t track_slot = max_tracks;
  };
  explicit LiveSessionStream(std::vector<LiveTrackPlan> tracks, std::uint64_t revision = 0);
  ~LiveSessionStream();
  LiveSessionStream(const LiveSessionStream&) = delete;
  LiveSessionStream& operator=(const LiveSessionStream&) = delete;
  std::uint64_t submit(std::vector<LiveTrackPlan> tracks, std::uint64_t revision);
  Receipt receipt(std::uint64_t ticket) const;
  bool cancelPending(std::uint64_t ticket);
  Receipt waitForDecision(std::uint64_t ticket,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(1000));
  void collectRetired() noexcept;
  Block nextBlock(std::uint32_t frames) noexcept;
  const std::string& trackId(std::size_t slot) const { return ids_.at(slot); }
  std::size_t trackCount() const noexcept { return ids_.size(); }
  std::uint64_t appliedRevision() const noexcept { return applied_.load(std::memory_order_acquire); }
  std::size_t appliedFrame() const noexcept { return applied_frame_.load(std::memory_order_acquire); }
  std::size_t frame() const noexcept { return published_frame_.load(std::memory_order_acquire); }
  std::size_t endFrame() const noexcept { return end_frame_.load(std::memory_order_acquire); }
  bool failed() const noexcept { return failed_.load(std::memory_order_acquire); }
  bool hasPendingUpdate() const noexcept {
    return acknowledged_.load(std::memory_order_acquire) != requested_.load(std::memory_order_acquire);
  }
  std::uint64_t suppressedConflictsAfterStop() const noexcept;
  std::uint64_t liveUpdateCountAfterStop() const noexcept { return update_count_; }
 private:
  struct TrackPlan;
  struct Lane;
  struct Plan;
  std::unique_ptr<Plan> prepare(std::vector<LiveTrackPlan> tracks, std::uint64_t revision) const;
  std::vector<std::string> ids_;
  std::vector<std::unique_ptr<Lane>> lanes_;
  std::vector<TrackBlock> blocks_;
  std::array<std::unique_ptr<Plan>, 2> owned_;
  const Plan* active_ = nullptr; // audio only
  std::atomic<const Plan*> pending_{nullptr}, retired_{nullptr};
  std::size_t position_ = 0, render_end_ = 0;
  std::uint64_t update_count_ = 0;
  std::uint64_t submitted_revision_ = 0, submitted_ticket_ = 0; // control only
  const Plan* submitted_plan_ = nullptr;
  std::size_t publish_begin_frame_ = 0, publish_end_frame_ = 0;
  Receipt receipt_; // control reads only after acquire ACK
  std::atomic<std::uint64_t> applied_{0}, requested_{0}, acknowledged_{0};
  std::atomic<std::size_t> applied_frame_{0}, published_frame_{0}, end_frame_{0};
  std::atomic<bool> failed_{false};
};
}
