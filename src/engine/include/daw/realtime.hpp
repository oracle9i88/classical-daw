#pragma once

#include "daw/types.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace daw {

// Single-producer/single-consumer bounded queue. The producer and consumer
// must each be confined to one thread. push/pop are allocation-free and use
// acquire/release ordering so the audio callback never needs a mutex.
template <typename T, std::size_t Capacity>
class SpscRing {
  static_assert(Capacity > 0, "SpscRing capacity must be positive");

 public:
  bool push(const T& value) noexcept {
    const std::uint64_t write = write_index_.load(std::memory_order_relaxed);
    const std::uint64_t read = read_index_.load(std::memory_order_acquire);
    if (write - read >= Capacity) return false;
    slots_[static_cast<std::size_t>(write % Capacity)] = value;
    write_index_.store(write + 1, std::memory_order_release);
    return true;
  }

  bool pop(T* value) noexcept {
    if (value == nullptr) return false;
    const std::uint64_t read = read_index_.load(std::memory_order_relaxed);
    const std::uint64_t write = write_index_.load(std::memory_order_acquire);
    if (read == write) return false;
    *value = slots_[static_cast<std::size_t>(read % Capacity)];
    read_index_.store(read + 1, std::memory_order_release);
    return true;
  }

  [[nodiscard]] std::size_t approximateSize() const noexcept {
    const auto write = write_index_.load(std::memory_order_acquire);
    const auto read = read_index_.load(std::memory_order_acquire);
    const auto count = write - read;
    return static_cast<std::size_t>(count > Capacity ? Capacity : count);
  }

 private:
  alignas(64) std::atomic<std::uint64_t> write_index_{0};
  alignas(64) std::atomic<std::uint64_t> read_index_{0};
  std::array<T, Capacity> slots_{};
};

enum class TransportCommandType : std::uint8_t { Start, Stop, SeekSamples, SetTempo };

struct TransportCommand {
  TransportCommandType type = TransportCommandType::Stop;
  SampleIndex sample_position = 0;
  double bpm = 120.0;
};

struct TransportSnapshot {
  SampleIndex sample_position = 0;
  double bpm = 120.0;
  bool running = false;
  std::uint64_t xrun_count = 0;
};

// Platform-neutral M0 transport. CoreAudio will call processBlock() from its
// callback; UI/device threads only enqueue commands and record xruns.
class BlockScheduler {
 public:
  static constexpr std::size_t kCommandCapacity = 256;

  bool enqueue(const TransportCommand& command) noexcept { return commands_.push(command); }
  void recordXrun() noexcept { xrun_count_.fetch_add(1, std::memory_order_relaxed); }

  // Applies all commands queued before this block and advances a running
  // transport by frame_count. No allocation, locks, or I/O occur here.
  void processBlock(std::uint32_t frame_count) noexcept {
    TransportCommand command;
    while (commands_.pop(&command)) apply(command);
    if (state_.running && frame_count <= std::numeric_limits<SampleIndex>::max() - state_.sample_position) {
      state_.sample_position += static_cast<SampleIndex>(frame_count);
    }
    state_.xrun_count = xrun_count_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] TransportSnapshot snapshot() const noexcept { return state_; }
  [[nodiscard]] std::size_t queuedCommandCount() const noexcept { return commands_.approximateSize(); }

 private:
  void apply(const TransportCommand& command) noexcept {
    switch (command.type) {
      case TransportCommandType::Start:
        state_.running = true;
        break;
      case TransportCommandType::Stop:
        state_.running = false;
        break;
      case TransportCommandType::SeekSamples:
        state_.sample_position = command.sample_position < 0 ? 0 : command.sample_position;
        break;
      case TransportCommandType::SetTempo:
        if (command.bpm > 0.0 && command.bpm <= 1000000.0) state_.bpm = command.bpm;
        break;
    }
  }

  SpscRing<TransportCommand, kCommandCapacity> commands_;
  TransportSnapshot state_;
  std::atomic<std::uint64_t> xrun_count_{0};
};

}  // namespace daw

