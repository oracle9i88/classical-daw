#pragma once

#include "daw/live_session.hpp"
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
  using Block = LiveSessionStream::TrackBlock;
  using Decision = LiveSessionStream::Decision;
  using Receipt = LiveSessionStream::Receipt;
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
  std::uint64_t appliedRevision() const noexcept { return session_.appliedRevision(); }
  std::size_t appliedFrame() const noexcept { return session_.appliedFrame(); }
  std::size_t frame() const noexcept { return session_.frame(); }
  std::size_t endFrame() const noexcept { return session_.endFrame(); }
  bool failed() const noexcept { return session_.failed(); }
  bool hasPendingUpdate() const noexcept { return session_.hasPendingUpdate(); }
  std::uint64_t suppressedConflictsAfterStop() const noexcept { return session_.suppressedConflictsAfterStop(); }
  std::uint64_t liveUpdateCountAfterStop() const noexcept { return session_.liveUpdateCountAfterStop(); }
 private:
  LiveSessionStream session_;
};
}
