#include "daw/callback_timing.hpp"
#include "daw/output_blocks.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace {
void require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
bool near(double a, double b) { return std::abs(a - b) < 1e-12; }
}

int main() {
  try {
    static_assert(std::is_trivially_copyable_v<daw::CallbackTimingStats>);
    static_assert(noexcept(daw::CallbackTimingStats{}.record(557, 48000, .004)));

    // The old 45-frame tail has a high per-subdivision ratio, although the
    // aggregate work easily meets the complete callback's 50% headroom gate.
    const double old_subblock_ratio = .0007 / (45. / 48000.);
    daw::CallbackTimingStats full;
    full.record(557, 48000, .0016 + .0016 + .0007);
    require(old_subblock_ratio > .5, "fixture did not expose the short-tail proxy");
    require(full.callback_count == 1 && full.total_frames == 557 &&
            full.headroom_misses == 0 && full.deadline_misses == 0 &&
            near(full.max_callback_budget_ratio, .0039 / (557. / 48000.)),
            "complete callback gate inherited a sub-block denominator");

    // Balanced splitting still records once, after all three subdivisions.
    float samples[1114]{};
    unsigned int blocks = 0;
    double total_seconds = 0;
    daw::renderOutputBlocks(samples, 557, 2, 256,
        [&](float*, std::uint32_t frames) noexcept {
          ++blocks;
          total_seconds += .0007 + frames * .000003;
        });
    daw::CallbackTimingStats balanced;
    balanced.record(557, 48000, total_seconds);
    require(blocks == 3 && balanced.callback_count == 1 && balanced.total_frames == 557 &&
            near(balanced.total_seconds, .0007 * 3 + 557 * .000003),
            "callback metric counted quanta or discarded fixed call overhead");

    // Slowest wall time and worst ratio need not belong to the same callback.
    // Preserve both complete tuples instead of combining unrelated maxima.
    daw::CallbackTimingStats association;
    association.record(100, 1000, .04);
    association.record(10, 1000, .008);
    require(near(association.max_callback_seconds, .04) &&
            association.max_seconds_frames == 100 &&
            near(association.max_seconds_budget_ratio, .4) &&
            near(association.max_callback_budget_ratio, .8) &&
            near(association.max_ratio_seconds, .008) && association.max_ratio_frames == 10,
            "maxima lost their own frames/time/ratio association");
    require(association.headroom_misses == 1 && association.deadline_misses == 0,
            "headroom failure was mislabeled as a deadline miss");

    // Inclusive boundaries: equality already consumes the promised margin.
    daw::CallbackTimingStats boundary;
    boundary.record(1000, 1000, .5);
    boundary.record(1000, 1000, 1);
    boundary.record(1000, 1000, .499);
    require(boundary.headroom_misses == 2 && boundary.deadline_misses == 1 &&
            boundary.callback_count == 3 && boundary.valid_budget_callbacks == 3,
            "50%/100% threshold boundaries are wrong");

    daw::CallbackTimingStats invalid;
    invalid.record(0, 48000, .01);
    invalid.record(256, 0, .02);
    invalid.record(256, std::numeric_limits<double>::quiet_NaN(), .03);
    invalid.record(256, 48000, -1);
    invalid.record(256, 48000, std::numeric_limits<double>::infinity());
    invalid.record(256, 48000, std::numeric_limits<double>::quiet_NaN());
    require(invalid.callback_count == 6 && invalid.invalid_budget_callbacks == 6 &&
            invalid.valid_budget_callbacks == 0 && near(invalid.total_seconds, .06) &&
            std::isfinite(invalid.max_callback_budget_ratio),
            "invalid timing inputs contaminated valid budget evidence");
    invalid.record(256, 48000, 0);
    require(invalid.valid_budget_callbacks == 1 && invalid.max_ratio_frames == 256 &&
            invalid.max_seconds_frames == 256, "zero-duration valid callback lost its tuple");

    const auto text = daw::formatCallbackTiming(association);
    require(text.find("output_timing_scope=complete_client_render_callback") != std::string::npos &&
            text.find("output_callback_count=2") != std::string::npos &&
            text.find("output_max_seconds_frames=100") != std::string::npos &&
            text.find("output_max_ratio_frames=10") != std::string::npos &&
            text.find("output_headroom_misses=1") != std::string::npos &&
            text.find("output_deadline_misses=0") != std::string::npos,
            "stable evidence output omitted the scope/associated maxima/distinct gates");
    association = {};
    require(association.callback_count == 0 && association.total_seconds == 0 &&
            association.max_ratio_frames == 0, "restart reset retained old measurements");
    std::cout << "Callback timing: full callback budgets, associated maxima, headroom/deadline boundaries, "
                 "invalid input and reset passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
