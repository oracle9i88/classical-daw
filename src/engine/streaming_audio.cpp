#include "daw/streaming_audio.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <thread>

namespace daw {
struct StreamingAudio::Impl {
  enum State : unsigned { Empty, Filling, Ready, Reading };
  struct Page {
    std::atomic<unsigned> state{Empty};
    std::size_t number = 0; // Access only while owning Filling or Reading.
    std::vector<float> samples;
  };
  std::vector<std::unique_ptr<FrozenTrackReader>> readers;
  std::array<Page, kPages> pages;
  std::size_t frames = 0, tracks = 0, held = kPages;
  std::atomic<std::size_t> requested{0};
  std::atomic<bool> stop{false}, error{false};
  std::thread worker;
  explicit Impl(std::vector<std::unique_ptr<FrozenTrackReader>> input) : readers(std::move(input)) {
    if (readers.empty() || readers.size() > kMaxTracks) throw std::invalid_argument("stream requires 1..64 readers");
    tracks = readers.size();
    for (const auto& reader : readers) {
      if (!reader || (frames && frames != reader->frameCount())) throw std::invalid_argument("stream readers must be aligned");
      frames = reader->frameCount();
    }
    for (auto& page : pages) page.samples.resize(kPageFrames * tracks * 2);
    for (std::size_t i = 0; i < kPages && i * kPageFrames < frames; ++i) load(pages[i], i);
    worker = std::thread([this] { run(); });
  }
  ~Impl() { stop.store(true, std::memory_order_relaxed); if (worker.joinable()) worker.join(); }
  void load(Page& page, std::size_t number) {
    const auto start = number * kPageFrames, count = std::min(kPageFrames, frames - start);
    std::array<float, kPageFrames * 2> scratch{};
    for (std::size_t track = 0; track < tracks; ++track) {
      readers[track]->readFrames(start, count, scratch.data());
      for (std::size_t f = 0; f < count; ++f) {
        page.samples[(f * tracks + track) * 2] = scratch[f * 2];
        page.samples[(f * tracks + track) * 2 + 1] = scratch[f * 2 + 1];
      }
    }
    page.number = number;
    page.state.store(Ready, std::memory_order_release);
  }
  void run() noexcept {
    try {
      while (!stop.load(std::memory_order_relaxed)) {
        const auto wanted = requested.load(std::memory_order_acquire);
        bool loaded = false;
        for (std::size_t next = wanted; next < wanted + kPages && next * kPageFrames < frames; ++next) {
          if (stop.load(std::memory_order_relaxed) || requested.load(std::memory_order_acquire) != wanted) break;
          bool present = false;
          // Only this thread writes page numbers. Consumer only changes states;
          // reading a Ready/Reading page number is therefore safe here.
          for (auto& page : pages) {
            const auto state = page.state.load(std::memory_order_acquire);
            if ((state == Ready || state == Reading) && page.number == next) { present = true; break; }
          }
          if (present) continue;
          for (auto& page : pages) {
            auto state = page.state.load(std::memory_order_acquire);
            if (state != Empty && !(state == Ready && (page.number < wanted || page.number >= wanted + kPages))) continue;
            if (!page.state.compare_exchange_strong(state, Filling, std::memory_order_acq_rel)) continue;
            load(page, next); loaded = true; break;
          }
        }
        if (!loaded) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    } catch (...) { error.store(true, std::memory_order_release); }
  }
  const float* frame(std::size_t position) noexcept {
    if (position >= frames || error.load(std::memory_order_acquire)) return nullptr;
    const auto number = position / kPageFrames;
    requested.store(number, std::memory_order_release);
    if (held < kPages && pages[held].number != number) {
      pages[held].state.store(Empty, std::memory_order_release); held = kPages;
    }
    if (held == kPages) {
      for (std::size_t i = 0; i < kPages; ++i) {
        unsigned expected = Ready;
        if (!pages[i].state.compare_exchange_strong(expected, Reading, std::memory_order_acq_rel)) continue;
        if (pages[i].number == number) { held = i; break; }
        pages[i].state.store(Ready, std::memory_order_release);
      }
    }
    return held == kPages ? nullptr : pages[held].samples.data() + (position % kPageFrames) * tracks * 2;
  }
};
static_assert(std::atomic<unsigned>::is_always_lock_free && std::atomic<std::size_t>::is_always_lock_free &&
              std::atomic<bool>::is_always_lock_free, "stream page handoff requires lock-free atomics");
StreamingAudio::StreamingAudio(std::vector<std::unique_ptr<FrozenTrackReader>> readers)
    : impl_(std::make_unique<Impl>(std::move(readers))) {}
StreamingAudio::~StreamingAudio() = default;
std::size_t StreamingAudio::trackCount() const noexcept { return impl_->tracks; }
std::size_t StreamingAudio::frameCount() const noexcept { return impl_->frames; }
std::size_t StreamingAudio::bufferBytes() const noexcept { return kPages * kPageFrames * impl_->tracks * 2 * sizeof(float); }
const float* StreamingAudio::frame(std::size_t position) noexcept { return impl_->frame(position); }
bool StreamingAudio::failed() const noexcept { return impl_->error.load(std::memory_order_acquire); }
}
