#pragma once

#include "daw/types.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>
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
  std::uint64_t voice_event_drop_count = 0;
  std::uint64_t voice_event_late_count = 0;
};

enum class VoiceEventType : std::uint8_t { NoteOn, NoteOff };

struct VoiceEvent {
  VoiceEventType type = VoiceEventType::NoteOff;
  std::uint8_t pitch = 60;
  std::uint8_t velocity = 0;
  // Absolute transport sample at which this event becomes due. Keeping this
  // field last preserves aggregate initialization used by the original
  // immediate-event API; omitted values default to sample zero.
  SampleIndex sample_position = 0;
};

// Configuration supplied once, on the control thread, before an instrument
// is handed to the audio callback.  A renderer must retain all state it needs
// in its own fixed-size storage; prepare/reset are therefore also noexcept and
// may not allocate or take locks.
struct InstrumentRenderConfig {
  double sample_rate = 48000.0;
  std::uint32_t max_frames_per_block = 0;
  std::uint32_t channels = 0;
};

// The real-time instrument contract.  The owner creates and prepares an
// implementation outside the callback, queues events from the producer side,
// then calls render() from exactly one audio thread.  Implementations must not
// allocate, block, perform I/O, or throw from any of these methods.  reset() is
// called only after the callback has stopped and makes the instance reusable.
class InstrumentRenderer {
 public:
  virtual ~InstrumentRenderer() = default;

  InstrumentRenderer() = default;
  InstrumentRenderer(const InstrumentRenderer&) = delete;
  InstrumentRenderer& operator=(const InstrumentRenderer&) = delete;

  virtual bool prepare(const InstrumentRenderConfig& config) noexcept = 0;
  virtual void reset() noexcept = 0;
  virtual bool enqueue(const VoiceEvent& event) noexcept = 0;
  virtual void render(float* interleaved_output, std::uint32_t frame_count,
                     std::uint32_t channels, double sample_rate) noexcept = 0;
};

// Fixed-size diagnostic voice used to prove the callback path with real
// samples. It is deliberately a sine bank, not a claim of orchestral quality.
class SineVoiceBank : public InstrumentRenderer {
 public:
  static constexpr std::size_t kEventCapacity = 128;
  static constexpr std::size_t kVoiceCapacity = 32;

  bool prepare(const InstrumentRenderConfig& config) noexcept override {
    if (config.sample_rate <= 0.0 || !std::isfinite(config.sample_rate) ||
        config.max_frames_per_block == 0 || config.channels == 0) {
      return false;
    }
    reset();
    sample_rate_ = config.sample_rate;
    prepared_config_ = config;
    prepared_ = true;
    return true;
  }

  void reset() noexcept override {
    VoiceEvent pending;
    while (events_.pop(&pending)) {
    }
    for (Voice& voice : voices_) voice = Voice{};
    prepared_ = false;
    prepared_config_ = InstrumentRenderConfig{};
    sample_rate_ = 48000.0;
  }

  bool enqueue(const VoiceEvent& event) noexcept override { return events_.push(event); }

  [[nodiscard]] bool prepared() const noexcept { return prepared_; }
  [[nodiscard]] InstrumentRenderConfig preparedConfig() const noexcept { return prepared_config_; }

  void render(float* interleaved_output, std::uint32_t frame_count, std::uint32_t channels,
              double sample_rate) noexcept override {
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
  InstrumentRenderConfig prepared_config_{};
  bool prepared_ = false;
};

// Platform-neutral transport and block-boundary event dispatcher. CoreAudio
// calls processBlock() from its callback; UI/device threads only enqueue
// commands/events and record xruns. Voice events are timestamped in absolute
// transport samples. The callback never allocates, locks, or performs I/O.
class BlockScheduler {
 public:
  static constexpr std::size_t kCommandCapacity = 256;
  static constexpr std::size_t kVoiceEventCapacity = 256;

  BlockScheduler() noexcept { publishSnapshot(); }

  bool enqueue(const TransportCommand& command) noexcept { return commands_.push(command); }
  bool enqueueVoiceEvent(const VoiceEvent& event) noexcept {
    if (!voice_events_.push(event)) {
      voice_event_drop_count_.fetch_add(1U, std::memory_order_relaxed);
      return false;
    }
    return true;
  }

  // The renderer is owned and prepared by the control thread before the
  // audio callback starts. It must remain valid until the callback has
  // stopped. Passing nullptr deliberately parks queued events for a later
  // attachment instead of silently discarding them.
  void setInstrumentRenderer(InstrumentRenderer* renderer) noexcept { renderer_ = renderer; }
  [[nodiscard]] InstrumentRenderer* instrumentRenderer() const noexcept { return renderer_; }
  void recordXrun() noexcept { xrun_count_.fetch_add(1, std::memory_order_relaxed); }

  // Applies all commands queued before this block, dispatches events due in
  // the block, and advances a running transport by frame_count. An event at
  // exactly the block end is retained for the next block so it is not fired
  // one block early. No allocation, locks, or I/O occur here.
  void processBlock(std::uint32_t frame_count) noexcept {
    TransportCommand command;
    while (commands_.pop(&command)) apply(command);
    const SampleIndex block_start = state_.sample_position;
    SampleIndex block_end = block_start;
    if (state_.running && frame_count <= std::numeric_limits<SampleIndex>::max() - block_start) {
      block_end += static_cast<SampleIndex>(frame_count);
    }
    ingestVoiceEvents();
    dispatchVoiceEvents(block_start, block_end, frame_count);
    if (state_.running && frame_count <= std::numeric_limits<SampleIndex>::max() - state_.sample_position) {
      state_.sample_position += static_cast<SampleIndex>(frame_count);
    }
    state_.xrun_count = xrun_count_.load(std::memory_order_relaxed);
    state_.voice_event_drop_count = voice_event_drop_count_.load(std::memory_order_relaxed);
    state_.voice_event_late_count = voice_event_late_count_.load(std::memory_order_relaxed);
    publishSnapshot();
  }

  // The audio thread owns state_. Readers use a sequence-published copy so
  // snapshot() never races with processBlock() and never touches a mutex.
  [[nodiscard]] TransportSnapshot snapshot() const noexcept {
    for (;;) {
      const std::uint64_t before = snapshot_sequence_.load(std::memory_order_acquire);
      if ((before & 1U) != 0U) continue;
      TransportSnapshot result;
      result.sample_position = snapshot_sample_position_.load(std::memory_order_relaxed);
      result.bpm = bitsToDouble(snapshot_bpm_bits_.load(std::memory_order_relaxed));
      result.running = snapshot_running_.load(std::memory_order_relaxed);
      result.xrun_count = snapshot_xrun_count_.load(std::memory_order_relaxed);
      result.voice_event_drop_count = snapshot_voice_event_drop_count_.load(std::memory_order_relaxed);
      result.voice_event_late_count = snapshot_voice_event_late_count_.load(std::memory_order_relaxed);
      const std::uint64_t after = snapshot_sequence_.load(std::memory_order_acquire);
      if (before == after) return result;
    }
  }
  [[nodiscard]] std::size_t queuedCommandCount() const noexcept { return commands_.approximateSize(); }
  [[nodiscard]] std::size_t queuedVoiceEventCount() const noexcept { return voice_events_.approximateSize(); }
  [[nodiscard]] std::size_t pendingVoiceEventCount() const noexcept { return pending_voice_event_count_; }

 private:
  static std::uint64_t doubleToBits(double value) noexcept {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
  }

  static double bitsToDouble(std::uint64_t bits) noexcept {
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }

  void publishSnapshot() noexcept {
    snapshot_sequence_.fetch_add(1U, std::memory_order_release);
    snapshot_sample_position_.store(state_.sample_position, std::memory_order_relaxed);
    snapshot_bpm_bits_.store(doubleToBits(state_.bpm), std::memory_order_relaxed);
    snapshot_running_.store(state_.running, std::memory_order_relaxed);
    snapshot_xrun_count_.store(state_.xrun_count, std::memory_order_relaxed);
    snapshot_voice_event_drop_count_.store(state_.voice_event_drop_count, std::memory_order_relaxed);
    snapshot_voice_event_late_count_.store(state_.voice_event_late_count, std::memory_order_relaxed);
    snapshot_sequence_.fetch_add(1U, std::memory_order_release);
  }

  void ingestVoiceEvents() noexcept {
    VoiceEvent event;
    while (voice_events_.pop(&event)) {
      if (pending_voice_event_count_ >= kVoiceEventCapacity) {
        voice_event_drop_count_.fetch_add(1U, std::memory_order_relaxed);
        continue;
      }
      std::size_t insert_at = pending_voice_event_count_;
      while (insert_at > 0 && pending_voice_events_[insert_at - 1U].sample_position > event.sample_position) {
        pending_voice_events_[insert_at] = pending_voice_events_[insert_at - 1U];
        --insert_at;
      }
      pending_voice_events_[insert_at] = event;
      ++pending_voice_event_count_;
    }
  }

  void dispatchVoiceEvents(SampleIndex block_start, SampleIndex block_end,
                           std::uint32_t frame_count) noexcept {
    // A stopped/zero-frame callback has no interval. Fire events at the
    // current transport position once, while retaining future events.
    const bool zero_length_block = !state_.running || frame_count == 0;
    while (pending_voice_event_count_ > 0) {
      const VoiceEvent& next = pending_voice_events_.front();
      const bool due = zero_length_block ? next.sample_position <= block_start
                                         : next.sample_position < block_end;
      if (!due) break;

      const VoiceEvent event = next;
      for (std::size_t index = 1; index < pending_voice_event_count_; ++index) {
        pending_voice_events_[index - 1U] = pending_voice_events_[index];
      }
      --pending_voice_event_count_;

      if (event.sample_position < block_start) {
        voice_event_late_count_.fetch_add(1U, std::memory_order_relaxed);
      }
      if (renderer_ != nullptr && !renderer_->enqueue(event)) {
        voice_event_drop_count_.fetch_add(1U, std::memory_order_relaxed);
      }
    }
  }

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
  SpscRing<VoiceEvent, kVoiceEventCapacity> voice_events_;
  std::array<VoiceEvent, kVoiceEventCapacity> pending_voice_events_{};
  std::size_t pending_voice_event_count_ = 0;
  InstrumentRenderer* renderer_ = nullptr;
  TransportSnapshot state_;
  std::atomic<std::uint64_t> xrun_count_{0};
  std::atomic<std::uint64_t> voice_event_drop_count_{0};
  std::atomic<std::uint64_t> voice_event_late_count_{0};
  std::atomic<std::uint64_t> snapshot_sequence_{0};
  std::atomic<SampleIndex> snapshot_sample_position_{0};
  std::atomic<std::uint64_t> snapshot_bpm_bits_{0};
  std::atomic<bool> snapshot_running_{false};
  std::atomic<std::uint64_t> snapshot_xrun_count_{0};
  std::atomic<std::uint64_t> snapshot_voice_event_drop_count_{0};
  std::atomic<std::uint64_t> snapshot_voice_event_late_count_{0};
};

}  // namespace daw
