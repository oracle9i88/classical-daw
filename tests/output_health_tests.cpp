#include "daw/output_health.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
}
int main() {
  try {
    using F = daw::OutputFault;
    const daw::OutputHealthSnapshot baseline{true, true, 42, 42, 512, 48000, 1000, 0};
    daw::OutputHealthMonitor monitor;
    require(monitor.observe(baseline, 0) == F::Stopped, "unarmed monitor healthy");
    monitor.arm(baseline, true, 100);
    require(monitor.observe(baseline, 2099) == F::None, "stall before deadline");
    auto progress = baseline; progress.rendered_frames += 1;
    require(monitor.observe(progress, 2100) == F::None, "progress failed to reset deadline");
    require(monitor.observe(progress, 4099) == F::None, "reset deadline wrong");
    require(monitor.observe(progress, 4100) == F::Stalled, "missing callback stall");
    progress.rendered_frames += 1;
    require(monitor.observe(progress, 4101) == F::Stalled, "fault automatically resumed");
    auto check = [&](daw::OutputHealthSnapshot bad, F expected) {
      monitor.arm(baseline, true, 100);
      require(monitor.observe(bad, 101) == expected, "wrong device fault");
      require(monitor.observe(baseline, 102) == expected, "fault was not latched");
      monitor.arm(baseline, true, 103);
      require(monitor.fault() == F::None, "explicit rearm failed");
    };
    auto s = baseline; s.readable = false; check(s, F::Properties);
    s = baseline; s.alive = false; check(s, F::DeviceLost);
    s = baseline; s.device = 0; check(s, F::DeviceLost);
    s = baseline; s.device = 43; check(s, F::DeviceChanged);
    s = baseline; s.default_device = 43; check(s, F::DefaultChanged);
    s = baseline; s.default_device = 0; check(s, F::DefaultChanged);
    s = baseline; s.sample_rate = 44100; check(s, F::FormatChanged);
    s = baseline; s.buffer_frames = 1024; check(s, F::FormatChanged);
    s = baseline; s.buffer_frames = 0; check(s, F::Properties);
    s = baseline; s.sample_rate = std::numeric_limits<double>::quiet_NaN(); check(s, F::Properties);
    s = baseline; s.callback_errors = 1; check(s, F::CallbackError);
    monitor.arm(baseline, false, 0);
    s = baseline; s.default_device = 43;
    require(monitor.observe(s, 1) == F::None, "explicit device followed unrelated system default");
    monitor.arm(s, true, 0);
    require(monitor.fault() == F::DefaultChanged, "start-time route race accepted");
    s = baseline; s.readable = false; monitor.arm(s, true, 0);
    require(monitor.fault() == F::Properties, "unreadable startup accepted");
    std::cout << "Output health: route/format/liveness/errors, stall deadline, sticky fault and explicit rearm passed\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
