#pragma once
#include "pipeline.hpp"
#include <cstdint>
#include <cstdlib>
#include <numeric>

namespace nvof {
// These bounds also protect the decomposed 64-bit tick calculation:
// scale <= 1e14, remainder products < 1e18. Validate den before multiplying it.
inline bool supported_output_rate(Rate rate) noexcept {
    return rate.num > 0 && rate.den > 0 && rate.den <= 10000000 &&
        rate.num <= 1000000000 && rate.num <= 1000 * rate.den;
}

// No guessed fixed FPS when the source is unknown. A zero numerator tells the
// caller to preserve the original stream and explain why x2 cannot be applied.
inline Rate double_source_rate(int64_t duration) noexcept {
    if (duration <= 0) return {0, 1};
    constexpr Rate common[] = {
        {24000,1001}, {30000,1001}, {48000,1001}, {60000,1001},
        {120000,1001}, {240000,1001}, {480000,1001},
        {15,1}, {24,1}, {25,1}, {30,1}, {48,1}, {50,1}, {60,1},
        {72,1}, {90,1}, {100,1}, {120,1}, {144,1}, {165,1},
        {180,1}, {200,1}, {240,1}, {250,1}, {300,1}, {360,1}, {480,1},
    };
    for (const auto source : common) {
        const int64_t quantized = (10000000LL * source.den + source.num/2) / source.num;
        if (std::llabs(duration - quantized) <= 1) return {source.num * 2, source.den};
    }
    const int64_t divisor = std::gcd(20000000LL, duration);
    const Rate doubled{20000000LL / divisor, duration / divisor};
    return supported_output_rate(doubled) ? doubled : Rate{0,1};
}
// Recover the advertised rational cadence instead of accumulating the rounded
// 100 ns AvgTimePerFrame (417083 ticks would drift about 2.9 ms per hour).
inline Rate canonical_source_rate(int64_t duration) noexcept {
    const Rate doubled = double_source_rate(duration);
    if (doubled.num == 0) return {0,1};
    const int64_t denominator = doubled.den * 2;
    const int64_t divisor = std::gcd(doubled.num, denominator);
    return {doubled.num / divisor, denominator / divisor};
}
} // namespace nvof
