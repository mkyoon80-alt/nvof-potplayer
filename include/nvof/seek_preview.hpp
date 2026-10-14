#pragma once
#include <cstdint>
#include <limits>
#include <optional>

namespace nvof {
// Bound both wall time and the amount of source video queued without synthesis.
// A fast decoder can otherwise fill a high-FPS allocator with originals whose
// longer source-frame durations greatly exceed the intended settling interval.
class SeekPreviewWindow {
public:
    void clear() noexcept { armed_ = false; first_pts_.reset(); }
    void begin(uint64_t now_ms, unsigned duration_ms) noexcept {
        const auto maximum = (std::numeric_limits<uint64_t>::max)();
        deadline_ms_ = now_ms > maximum - duration_ms ? maximum : now_ms + duration_ms;
        media_span_ = uint64_t(duration_ms) * 10000;
        first_pts_.reset();
        armed_ = duration_ms != 0;
    }
    bool armed() const noexcept { return armed_; }
    bool active(uint64_t now_ms, int64_t pts) noexcept {
        if (!armed_) return false;
        if (now_ms >= deadline_ms_) { clear(); return false; }
        // Preroll must not establish the first visible timestamp.
        if (pts < 0) return false;
        if (!first_pts_) first_pts_ = pts;
        if (pts < *first_pts_ || uint64_t(pts) - uint64_t(*first_pts_) >= media_span_) {
            clear();
            return false;
        }
        return true;
    }
private:
    uint64_t deadline_ms_ = 0, media_span_ = 0;
    std::optional<int64_t> first_pts_;
    bool armed_ = false;
};
}
