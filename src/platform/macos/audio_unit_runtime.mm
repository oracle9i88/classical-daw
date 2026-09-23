#include "audio_unit_runtime.hpp"
#import <AppKit/AppKit.h>
#include <chrono>
#include <cmath>
#include <stdexcept>

namespace daw {
void prepareAudioUnitRuntime() {
  if (![NSThread isMainThread]) throw std::logic_error("AU host startup must run on the main thread");
  @autoreleasepool { [NSApplication sharedApplication]; }
}

void serviceAudioUnitRuntime(double seconds) {
  if (![NSThread isMainThread] || !std::isfinite(seconds) || seconds < 0 || seconds > 10) {
    throw std::invalid_argument("invalid AU main-loop startup interval");
  }
  @autoreleasepool {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
      CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);
      // A run loop with no sources returns immediately. Bound CPU usage as well
      // as wall time while third-party initialization completes.
      [NSThread sleepForTimeInterval:0.01];
    }
  }
}
}
