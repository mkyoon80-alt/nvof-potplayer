#pragma once
#include "pipeline.hpp"
#include <cmath>

namespace nvof {
// Persisted InputRateMask contract, shared with the control app. NTSC rates
// belong to the corresponding 24/30/60 family, rather than a separate option.
enum class SourceRateBucket : unsigned {
    Fps24 = 1u,
    Fps25 = 2u,
    Fps30 = 4u,
    Fps50 = 8u,
    Fps60 = 16u,
    Other = 32u,
};
inline constexpr unsigned kAllInputRates = 63u;

inline SourceRateBucket source_rate_bucket(Rate rate) noexcept {
    if (rate.num <= 0 || rate.den <= 0) return SourceRateBucket::Other;
    const long double fps = static_cast<long double>(rate.num) / rate.den;
    // DirectShow stores a rounded 100-ns frame duration. This tolerance also
    // covers rates rounded to two decimals, without swallowing e.g. 24.1 fps.
    const auto matches_rate = [fps](long double expected) {
        return std::fabs(fps - expected) <= 0.020000001L;
    };
    if (matches_rate(24.0L) || matches_rate(24000.0L / 1001.0L)) return SourceRateBucket::Fps24;
    if (matches_rate(25.0L)) return SourceRateBucket::Fps25;
    if (matches_rate(30.0L) || matches_rate(30000.0L / 1001.0L)) return SourceRateBucket::Fps30;
    if (matches_rate(50.0L)) return SourceRateBucket::Fps50;
    if (matches_rate(60.0L) || matches_rate(60000.0L / 1001.0L)) return SourceRateBucket::Fps60;
    return SourceRateBucket::Other;
}

inline bool input_rate_selected(Rate rate, unsigned mask = kAllInputRates) noexcept {
    return (mask & static_cast<unsigned>(source_rate_bucket(rate))) != 0;
}
} // namespace nvof