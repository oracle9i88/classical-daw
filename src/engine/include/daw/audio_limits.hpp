#pragma once
#include <cstddef>

namespace daw {
// Buffer-based render/read APIs retain their allocation guard (256 MiB stereo).
inline constexpr std::size_t kMaxBufferedAudioFrames = 32U * 1024U * 1024U;
// Fixed 48 kHz stereo streaming: two hours including release tail. Fits the
// existing DAWFRZ01 uint32 frame count; byte offsets require 64-bit arithmetic.
inline constexpr std::size_t kMaxStreamAudioFrames = 48000ULL * 60 * 120;
}
