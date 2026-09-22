#pragma once

#include "daw/types.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cmath>
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

enum class VoiceEventType : std::uint8_t { NoteOn, NoteOff };

struct VoiceEvent {
  VoiceEventType type = VoiceEventType::NoteOff;
  std::uint8_t pitch = 60;
  std::uint8_t velocity = 0;
};

// Fixed-size diagnostic voice used to prove the callback path with real
// samples. It is deliberately a sine bank, not a claim of orchestral quality.
class SineVoiceBank {
 public:
  static constexpr std::size_t kEventCapacity = 128;
  static constexpr std::size_t kVoiceCapacity = 32;

  bool enqueue(const VoiceEvent& event) noexcept { return events_.push(event); }

  void render(float* interleaved_output, std::uint32_t frame_count, std::uint32_t channels,
              double sample_rate) noexcept {
    if (interleaved_output == nullptr || channels == 0 || sample_rate <= 0.0) return;
    sample_rate_ = sample_rate;
    VoiceEvent event;
    while (events_.pop(&event)) apply(event);
    for (std::uint32_t frame = 0; frame < frame_count; ++frame) {
      float sample = 0.0F;
      for (Voice& voice : voices_) {
        if (!voice.active) continue;
        sample += voice.amplitude * static_cast<float>(std::sin(voice.phase));
        voice.phase += voice.phase_increment;
        if (voice.phase >= kTwoPi) voice.phase -= kTwoPi;
      }
      for (std::uint32_t channel = 0; channel < channels; ++channel) {
        interleaved_output[static_cast<std::size_t>(frame) * channels + channel] = sample;
      }
    }
  }

 private:
  struct Voice {
    bool active = false;
    std::uint8_t pitch = 0;
    float amplitude = 0.0F;
    double phase = 0.0;
    double phase_increment = 0.0;
  };

  static constexpr double kTwoPi = 6.28318530717958647692;

  void apply(const VoiceEvent& event) noexcept {
    if (event.type == VoiceEventType::NoteOff || event.velocity == 0) {
      for (Voice& voice : voices_) {
        if (voice.active && voice.pitch == event.pitch) voice.active = false;
      }
      return;
    }
    Voice* target = nullptr;
    for (Voice& voice : voices_) {
      if (voice.active && voice.pitch == event.pitch) {
        target = &voice;
        break;
      }
      if (target == nullptr && !voice.active) target = &voice;
    }
    if (target == nullptr) target = &voices_.front();
    target->active = true;
    target->pitch = event.pitch;
    target->amplitude = static_cast<float>(event.velocity) / 127.0F * 0.15F;
    target->phase = 0.0;
    target->phase_increment = kTwoPi * 440.0 * std::pow(2.0, (static_cast<double>(event.pitch) - 69.0) / 12.0) / sample_rate_;
  }

  SpscRing<VoiceEvent, kEventCapacity> events_;
  std::array<Voice, kVoiceCapacity> voices_{};
  double sample_rate_ = 48000.0;
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
