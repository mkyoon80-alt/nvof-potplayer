#pragma once
#include <cstdint>

namespace nvof::playback {
// Accepted common scheduling limits. These bound capacity, never startup prefill.
inline constexpr unsigned input_queue_frames=2;
inline constexpr unsigned seek_preview_ms=200;
inline constexpr int64_t ready_output_ms=40;
inline constexpr int64_t ready_extra_frames=2;
inline constexpr uint64_t ready_memory_mib=1024;
inline constexpr int64_t allocator_headroom_ms=64;
inline constexpr uint64_t allocator_memory_mib=1536;
inline constexpr int64_t headroom_threshold_fps=120;
}
