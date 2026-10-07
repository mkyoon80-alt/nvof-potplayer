#include "nvof/output_rate_policy.hpp"
#include "nvof/pipeline.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>
namespace nvof {
namespace {
void validate(const Frame& f) {
    if (f.width < 2 || f.height < 2 || f.width > 8192 || f.height > 8192 || (f.width & 1) || (f.height & 1))
        throw std::invalid_argument("Expected an even-sized NV12 frame no larger than 8192 pixels per axis");
    const size_t required = size_t(f.width) * size_t(f.height) * 3 / 2;
    if (f.pixels.size() != required) throw std::invalid_argument("NV12 buffer size mismatch");
}
Frame mix(const Frame& a, const Frame& b, int64_t pts) {
    if (pts <= a.pts) { Frame result = a; result.pts = pts; return result; }
    if (pts >= b.pts) { Frame result = b; result.pts = pts; return result; }
    Frame result{a.width, a.height, pts, std::vector<uint8_t>(a.pixels.size())};
    // Integer interpolation on the native 2x sequence; this is target-rate
    // resampling, not an alternative motion estimator or silent GPU fallback.
    const uint32_t weight = uint32_t((pts-a.pts) * 65536 / (b.pts-a.pts));
    for (size_t i=0; i<a.pixels.size(); ++i)
        result.pixels[i] = uint8_t((uint32_t(a.pixels[i])*(65536-weight) + uint32_t(b.pixels[i])*weight + 32768) >> 16);
    return result;
}
}
HybridPipeline::HybridPipeline(Rate target, Midpoint midpoint)
    : target_(target), midpoint_(std::move(midpoint)) {
    if (!supported_output_rate(target))
        throw std::invalid_argument("Invalid output frame rate");
    if (!midpoint_) throw std::invalid_argument("A motion interpolation engine is required");
}
void HybridPipeline::reset() { previous_.reset(); origin_=0; next_index_=0; }
int64_t HybridPipeline::tick(int64_t index) const {
    // Quotient/remainder avoids cumulative rounding drift and large products.
    const int64_t scale = 10000000LL * target_.den;
    if (index < 0) throw std::overflow_error("Playback frame index overflow");
    const int64_t whole = index / target_.num;
    const int64_t remainder = index % target_.num;
    if (whole > (std::numeric_limits<int64_t>::max()-scale)/scale)
        throw std::overflow_error("Playback clock overflow");
    const int64_t fraction = remainder*(scale/target_.num)
        + remainder*(scale%target_.num)/target_.num;
    const int64_t offset = whole*scale+fraction;
    if (origin_ > std::numeric_limits<int64_t>::max()-offset)
        throw std::overflow_error("Playback timestamp overflow");
    return origin_+offset;
}
bool HybridPipeline::deliver(Frame frame, bool discontinuity, const Emit& emit) {
    frame.pts = tick(next_index_);
    OutputFrame output{std::move(frame),tick(next_index_+1),discontinuity};
    if (!emit(output)) { reset(); return false; }
    ++next_index_;
    return true;
}
void HybridPipeline::push(Frame frame, bool discontinuity, const Emit& emit) {
    validate(frame);
    if (previous_ && (frame.width != previous_->width || frame.height != previous_->height || frame.pts <= previous_->pts || frame.pts-previous_->pts > 10000000LL))
        discontinuity=true;
    if (discontinuity) reset();
    if (!previous_) {
        origin_=frame.pts;
        previous_=std::move(frame);
        // The seek preview never waits for a second decoded frame or the GPU.
        deliver(*previous_,true,emit);
        return;
    }
    if (tick(next_index_) < frame.pts) {
        Frame middle=midpoint_(*previous_,frame);
        validate(middle);
        if (middle.width != frame.width || middle.height != frame.height || middle.pts <= previous_->pts || middle.pts >= frame.pts)
            throw std::runtime_error("Motion interpolation engine returned an invalid midpoint");
        while (tick(next_index_) < frame.pts) {
            const int64_t t=tick(next_index_);
            Frame output=t<=middle.pts ? mix(*previous_,middle,t) : mix(middle,frame,t);
            if (!deliver(std::move(output),false,emit)) return;
        }
    }
    previous_=std::move(frame);
}
void HybridPipeline::finish(int64_t last_duration, const Emit& emit) {
    if (!previous_) return;
    if (last_duration > 0 && last_duration <= 10000000LL) {
        const int64_t end=previous_->pts+last_duration;
        while (tick(next_index_) < end) {
            Frame last=*previous_;
            const int64_t start=tick(next_index_);
            const int64_t stop=std::min(tick(next_index_+1),end);
            last.pts=start;
            if (!emit(OutputFrame{std::move(last),stop,false})) { reset(); return; }
            ++next_index_;
        }
    }
    reset();
}
}

