#pragma once
#include "daw/audio_output_source.hpp"
#include "daw/performance.hpp"
#include "daw/live_session.hpp"
#include <memory>
#include <stdexcept>

namespace daw {
// Control-thread facade for the existing CLI. Ensemble playback uses the ONE
// shared session runtime; legacy piano retains its verified range/preroll path.
template <class PianoAudition, class EnsembleAudition>
class BasicDocumentAudition final : public AudioOutputSource {
public:
  BasicDocumentAudition(const PerformanceDocument &d, bool silent = false, bool capture = false,
                        std::uint64_t revision = 0) {
    if (d.routes.empty())
      piano_ = std::make_unique<PianoAudition>(d, silent, capture, revision);
    else {
      if (capture)
        throw std::invalid_argument("ensemble capture must use a dedicated probe");
      ensemble_ = std::make_unique<EnsembleAudition>(d, silent, revision);
    }
  }
  bool acceptsFormat(double r, std::uint32_t c) const noexcept override {
    return r == 48000 && c == 2;
  }
  void render(float *out, std::uint32_t n) noexcept override {
    if (piano_)
      piano_->render(out, n);
    else
      ensemble_->render(out, n);
  }
  void prepareRange(std::size_t from, std::size_t until = 0) {
    if (piano_)
      piano_->prepareRange(from, until);
    else if (from || until)
      throw std::invalid_argument(
          "ensemble range/seek requires stopped graph preroll integration; use play from zero");
  }
  void start() noexcept {
    if (piano_)
      piano_->start();
    else
      ensemble_->start();
  }
  LiveSessionStream::Receipt submit(const PerformanceDocument &d, std::uint64_t revision) {
    return piano_ ? piano_->submit(d, revision) : ensemble_->submit(d, revision);
  }
  bool done() const noexcept { return piano_ ? piano_->done() : ensemble_->done(); }
  bool failed() const noexcept { return piano_ ? piano_->failed() : ensemble_->failed(); }
  std::size_t frame() const noexcept { return piano_ ? piano_->frame() : ensemble_->frame(); }
  double latency() const noexcept { return piano_ ? piano_->latency() : ensemble_->latency(); }
  std::uint64_t appliedRevision() const noexcept {
    return piano_ ? piano_->appliedRevision() : ensemble_->appliedRevision();
  }
  std::size_t appliedFrame() const noexcept {
    return piano_ ? piano_->appliedFrame() : ensemble_->appliedFrame();
  }
  std::uint64_t suppressedConflictsAfterStop() const noexcept {
    return piano_ ? piano_->suppressedConflictsAfterStop()
                  : ensemble_->suppressedConflictsAfterStop();
  }
  std::string statusText() const { return ensemble_ ? ensemble_->statusText() : std::string{}; }

private:
  std::unique_ptr<PianoAudition> piano_;
  std::unique_ptr<EnsembleAudition> ensemble_;
};
} // namespace daw
