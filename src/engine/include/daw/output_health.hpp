#pragma once
#include <cmath>
#include <cstdint>

namespace daw {
// Control-thread observations only. No device API calls or clocks on the audio
// thread. A fault stays latched until a successful explicit start arms a new run.
struct OutputHealthSnapshot {
  bool readable = false, alive = false;
  std::uint32_t device = 0, default_device = 0, buffer_frames = 0;
  double sample_rate = 0;
  std::uint64_t rendered_frames = 0, callback_errors = 0;
};
enum class OutputFault { None, Stopped, Properties, DeviceLost, DeviceChanged,
                         DefaultChanged, FormatChanged, CallbackError, Stalled };
inline const char* outputFaultText(OutputFault fault) noexcept {
  switch (fault) {
    case OutputFault::None: return "healthy";
    case OutputFault::Stopped: return "output stopped";
    case OutputFault::Properties: return "output device properties unavailable";
    case OutputFault::DeviceLost: return "output device disconnected";
    case OutputFault::DeviceChanged: return "active output device changed";
    case OutputFault::DefaultChanged: return "system default output changed";
    case OutputFault::FormatChanged: return "output sample rate or buffer size changed";
    case OutputFault::CallbackError: return "output callback error";
    case OutputFault::Stalled: return "output callbacks stopped advancing";
  }
  return "unknown output fault";
}
class OutputHealthMonitor {
 public:
  static constexpr std::uint64_t kStallMilliseconds = 2000;
  void arm(const OutputHealthSnapshot& snapshot, bool follows_default, std::uint64_t now_ms) noexcept {
    baseline_ = snapshot; last_frames_ = snapshot.rendered_frames; last_progress_ = now_ms;
    follows_default_ = follows_default; fault_ = OutputFault::None;
    observe(snapshot, now_ms);
  }
  OutputFault observe(const OutputHealthSnapshot& s, std::uint64_t now_ms) noexcept {
    if (fault_ != OutputFault::None) return fault_;
    if (!s.readable || !s.buffer_frames || !std::isfinite(s.sample_rate) || s.sample_rate <= 0)
      return fault_ = OutputFault::Properties;
    if (!s.device || !s.alive) return fault_ = OutputFault::DeviceLost;
    if (s.device != baseline_.device) return fault_ = OutputFault::DeviceChanged;
    if (follows_default_ && s.default_device != baseline_.device) return fault_ = OutputFault::DefaultChanged;
    if (s.sample_rate != baseline_.sample_rate || s.buffer_frames != baseline_.buffer_frames)
      return fault_ = OutputFault::FormatChanged;
    if (s.callback_errors != baseline_.callback_errors) return fault_ = OutputFault::CallbackError;
    if (s.rendered_frames != last_frames_) { last_frames_ = s.rendered_frames; last_progress_ = now_ms; }
    if (now_ms >= last_progress_ && now_ms-last_progress_ >= kStallMilliseconds) return fault_ = OutputFault::Stalled;
    return fault_;
  }
  OutputFault fault() const noexcept { return fault_; }
 private:
  OutputHealthSnapshot baseline_;
  std::uint64_t last_frames_ = 0, last_progress_ = 0;
  bool follows_default_ = true;
  OutputFault fault_ = OutputFault::Stopped;
};
}
