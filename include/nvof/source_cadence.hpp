#pragma once
#include "output_rate_policy.hpp"
#include <limits>
#include <optional>
#include <stdexcept>

namespace nvof {
// Decoder/container PTS can quantize a known constant source cadence to 1 ms.
// Anchor to the first actual PTS, then remove at most 1 ms of relative jitter.
// A larger deviation disables correction until reset: genuine VFR keeps its PTS.
class SourceCadence {
public:
    explicit SourceCadence(Rate source = {0, 1}) : source_(source) {
        if (source_.num == 0 ? source_.den <= 0 : !supported_output_rate(source_))
            throw std::invalid_argument("Invalid source frame rate");
        reset();
    }
    void reset() noexcept {
        origin_ = next_index_ = 0;
        anchored_ = false;
        active_ = source_.num > 0;
    }
    bool active() const noexcept { return active_ && anchored_; }
    int64_t normalize(int64_t pts) {
        if (pts < 0) throw std::invalid_argument("Expected a nonnegative source timestamp");
        if (!active_) return pts;
        if (!anchored_) {
            anchored_ = true; origin_ = pts; next_index_ = 1;
            return pts;
        }
        const auto expected = next_tick();
        if (!expected || !within_tolerance(pts, *expected)) {
            active_ = false;
            return pts;
        }
        if (next_index_ == (std::numeric_limits<int64_t>::max)()) active_ = false;
        else ++next_index_;
        return *expected;
    }
    // Correct an ordinary final frame's quantized duration without extending a
    // deliberately clipped final sample. Unknown/VFR streams remain untouched.
    int64_t normalize_end(int64_t end) const noexcept {
        const auto expected = active_ && anchored_ ? next_tick() : std::nullopt;
        return expected && end >= 0 && within_tolerance(end, *expected) ? *expected : end;
    }
private:
    static bool within_tolerance(int64_t actual, int64_t expected) noexcept {
        // Both operands are nonnegative, so subtraction cannot overflow.
        constexpr int64_t tolerance = 10000; // 1 ms in DirectShow's 100 ns units.
        return actual >= expected ? actual - expected <= tolerance : expected - actual <= tolerance;
    }
    std::optional<int64_t> next_tick() const noexcept {
        const int64_t scale = 10000000LL * source_.den;
        const int64_t whole = next_index_ / source_.num, remainder = next_index_ % source_.num;
        const int64_t maximum = (std::numeric_limits<int64_t>::max)();
        if (whole > maximum / scale) return std::nullopt;
        const int64_t fraction = remainder * (scale / source_.num) + remainder * (scale % source_.num) / source_.num;
        const int64_t integral = whole * scale;
        if (integral > maximum - fraction) return std::nullopt;
        const int64_t offset = integral + fraction;
        if (origin_ > maximum - offset) return std::nullopt;
        return origin_ + offset;
    }
    Rate source_;
    int64_t origin_ = 0, next_index_ = 0;
    bool anchored_ = false, active_ = false;
};
} // namespace nvof
