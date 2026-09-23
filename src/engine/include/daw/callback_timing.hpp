#pragma once

#include <cmath>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>

namespace daw {

// Single-writer statistics for one COMPLETE client render callback, not each
// engine/AU subdivision. Read/reset only after the callback has stopped.
// The budget is client_frames / client_sample_rate; it does not measure the
// output AudioUnit's surrounding rate conversion, HAL, or device driver work.
// No allocation, lock, property call or formatting occurs in record().
struct CallbackTimingStats {
  std::uint64_t callback_count = 0;
  std::uint64_t valid_budget_callbacks = 0;
  std::uint64_t invalid_budget_callbacks = 0;
  std::uint64_t total_frames = 0;
  double total_seconds = 0;
  double max_callback_seconds = 0;
  std::uint32_t max_seconds_frames = 0;
  double max_seconds_budget_ratio = 0;
  double max_callback_budget_ratio = 0;
  double max_ratio_seconds = 0;
  std::uint32_t max_ratio_frames = 0;
  std::uint64_t deadline_misses = 0;  // Measured ratio >= 1, not an OS xrun counter.
  std::uint64_t headroom_misses = 0;  // Measured ratio >= 0.5, our acceptance gate.

  void record(std::uint32_t client_frames, double client_sample_rate,
              double elapsed_seconds) noexcept {
    ++callback_count;
    total_frames += client_frames;
    const bool valid_elapsed = std::isfinite(elapsed_seconds) && elapsed_seconds >= 0;
    if (valid_elapsed) total_seconds += elapsed_seconds;
    if (!valid_elapsed || !client_frames || !std::isfinite(client_sample_rate) ||
        client_sample_rate <= 0) {
      ++invalid_budget_callbacks;
      return;
    }
    const double budget = static_cast<double>(client_frames) / client_sample_rate;
    const double ratio = elapsed_seconds / budget;
    if (!std::isfinite(budget) || budget <= 0 || !std::isfinite(ratio)) {
      ++invalid_budget_callbacks;
      return;
    }
    ++valid_budget_callbacks;
    if (valid_budget_callbacks == 1 || elapsed_seconds > max_callback_seconds) {
      max_callback_seconds = elapsed_seconds;
      max_seconds_frames = client_frames;
      max_seconds_budget_ratio = ratio;
    }
    if (valid_budget_callbacks == 1 || ratio > max_callback_budget_ratio) {
      max_callback_budget_ratio = ratio;
      max_ratio_seconds = elapsed_seconds;
      max_ratio_frames = client_frames;
    }
    if (ratio >= 1) ++deadline_misses;
    if (ratio >= 0.5) ++headroom_misses;
  }
};

// Control-thread evidence formatting only. Prefix every key to distinguish
// complete callback timing from any source's diagnostic sub-block timing.
inline std::string formatCallbackTiming(const CallbackTimingStats& stats) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(17)
      << "output_timing_scope=complete_client_render_callback"
      << " output_callback_count=" << stats.callback_count
      << " output_valid_budget_callbacks=" << stats.valid_budget_callbacks
      << " output_invalid_budget_callbacks=" << stats.invalid_budget_callbacks
      << " output_total_frames=" << stats.total_frames
      << " output_total_seconds=" << stats.total_seconds
      << " output_max_callback_seconds=" << stats.max_callback_seconds
      << " output_max_seconds_frames=" << stats.max_seconds_frames
      << " output_max_seconds_budget_ratio=" << stats.max_seconds_budget_ratio
      << " output_max_callback_budget_ratio=" << stats.max_callback_budget_ratio
      << " output_max_ratio_seconds=" << stats.max_ratio_seconds
      << " output_max_ratio_frames=" << stats.max_ratio_frames
      << " output_deadline_misses=" << stats.deadline_misses
      << " output_headroom_misses=" << stats.headroom_misses;
  return out.str();
}

}  // namespace daw
