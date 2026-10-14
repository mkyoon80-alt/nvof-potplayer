#pragma once
#include "output_rate_policy.hpp"
#include "playback_limits.hpp"
#include <algorithm>

namespace nvof {
// Ready outputs can cover a brief GPU completion stall while the same worker
// services allocator-return events. This is a maximum, never a prefill floor.
inline size_t gpu_ready_output_count(int multiple,Rate target,uint64_t surface_bytes) noexcept {
    if(!supported_output_rate(target)||target.num<=int64_t(playback::headroom_threshold_fps)*target.den||!surface_bytes)return 0;
    const int64_t denominator=1000*target.den;
    const int64_t timed=(int64_t(playback::ready_output_ms)*target.num+denominator-1)/denominator;
    const int64_t desired=(std::max)(int64_t(std::clamp(multiple,1,33))+int64_t(playback::ready_extra_frames),timed);
    const uint64_t ready_budget=uint64_t(playback::ready_memory_mib)*1024*1024;
    return size_t((std::min)(uint64_t((std::min)(desired,int64_t(32))),ready_budget/surface_bytes));
}
// Capacity, not a prefill requirement. The first original is still delivered
// immediately. Preserve the legacy two-source-interval floor and host requests.
inline long gpu_output_buffer_count(long requested, int multiple, Rate target, uint64_t surface_bytes) noexcept {
    const long batch=std::clamp(multiple,1,33);
    const long baseline=(std::max)(requested,std::clamp(2L*batch,4L,32L));
    if(!supported_output_rate(target) || target.num<=int64_t(playback::headroom_threshold_fps)*target.den || !surface_bytes)return baseline;
    // At 240 fps, ten samples cover only 42 ms. Aim for 64 ms, rounded
    // to complete source batches, without growing by more than the memory cap.
    const int64_t denominator=1000*target.den;
    const int64_t frames=(int64_t(playback::allocator_headroom_ms)*target.num+denominator-1)/denominator;
    const long timed=long((frames+batch-1)/batch*batch);
    const uint64_t retained_budget=uint64_t(playback::allocator_memory_mib)*1024*1024;
    const long bounded=long((std::min)(uint64_t(32),retained_budget/surface_bytes));
    return (std::max)(baseline,(std::min)(timed,bounded));
}
}
