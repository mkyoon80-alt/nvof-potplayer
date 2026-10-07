#include "nvof/output_rate_policy.hpp"
#include "nvof/gpu_pipeline.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>
namespace nvof {
namespace {
void validate(const GpuFrame& f) {
    if (!f.texture || f.width < 2 || f.height < 2 || f.width > 8192 || f.height > 8192 || (f.width & 1) || (f.height & 1) || f.pts < 0)
        throw std::invalid_argument("Expected a nonnegative, even-sized GPU NV12 frame");
    D3D11_TEXTURE2D_DESC desc{}; f.texture->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_NV12 || UINT(f.width) > desc.Width || UINT(f.height) > desc.Height || f.array_slice >= desc.ArraySize)
        throw std::invalid_argument("GPU texture description does not match frame");
}
}
GpuHybridPipeline::GpuHybridPipeline(Rate target, GpuMidpoint midpoint, GpuBlend blend)
    : target_(target), midpoint_(std::move(midpoint)), blend_(std::move(blend)) {
    if (!supported_output_rate(target))
        throw std::invalid_argument("Invalid output frame rate");
    if (!midpoint_ || !blend_) throw std::invalid_argument("GPU interpolation and resampling are required");
}
void GpuHybridPipeline::reset() { previous_.reset(); origin_ = next_index_ = 0; }
int64_t GpuHybridPipeline::tick(int64_t index) const {
    const int64_t scale = 10000000LL * target_.den;
    if (index < 0) throw std::overflow_error("Playback frame index overflow");
    const int64_t whole = index / target_.num, remainder = index % target_.num;
    if (whole > (std::numeric_limits<int64_t>::max() - scale) / scale)
        throw std::overflow_error("Playback clock overflow");
    const int64_t fraction = remainder * (scale / target_.num) + remainder * (scale % target_.num) / target_.num;
    const int64_t offset = whole * scale + fraction;
    if (origin_ > std::numeric_limits<int64_t>::max() - offset) throw std::overflow_error("Playback timestamp overflow");
    return origin_ + offset;
}
bool GpuHybridPipeline::deliver(GpuFrame frame, bool discontinuity, const GpuEmit& emit) {
    frame.pts = tick(next_index_);
    if (!emit(GpuOutputFrame{std::move(frame), tick(next_index_ + 1), discontinuity})) { reset(); return false; }
    ++next_index_; return true;
}
void GpuHybridPipeline::push(GpuFrame frame, bool discontinuity, const GpuEmit& emit) {
    validate(frame);
    if (previous_ && (frame.width != previous_->width || frame.height != previous_->height || frame.pts <= previous_->pts || frame.pts - previous_->pts > 10000000LL)) discontinuity = true;
    if (discontinuity) reset();
    if (!previous_) {
        origin_ = frame.pts; previous_ = std::move(frame);
        deliver(*previous_, true, emit); return;
    }
    if (tick(next_index_) < frame.pts) {
        GpuFrame middle = midpoint_(*previous_, frame); validate(middle);
        if (middle.width != frame.width || middle.height != frame.height || middle.pts <= previous_->pts || middle.pts >= frame.pts)
            throw std::runtime_error("GPU interpolation returned an invalid midpoint");
        while (tick(next_index_) < frame.pts) {
            const int64_t pts = tick(next_index_);
            GpuFrame output = pts <= middle.pts ? blend_(*previous_, middle, pts) : blend_(middle, frame, pts);
            validate(output);
            if (output.width != frame.width || output.height != frame.height || output.pts != pts)
                throw std::runtime_error("GPU resampling returned an invalid frame");
            if (!deliver(std::move(output), false, emit)) return;
        }
    }
    previous_ = std::move(frame);
}
void GpuHybridPipeline::finish(int64_t last_duration, const GpuEmit& emit) {
    if (!previous_) return;
    if (last_duration > 0 && last_duration <= 10000000LL) {
        if (previous_->pts > std::numeric_limits<int64_t>::max() - last_duration) throw std::overflow_error("End timestamp overflow");
        const int64_t end = previous_->pts + last_duration;
        while (tick(next_index_) < end) {
            GpuFrame frame = *previous_; frame.pts = tick(next_index_);
            if (!emit(GpuOutputFrame{std::move(frame), std::min(tick(next_index_ + 1), end), false})) { reset(); return; }
            ++next_index_;
        }
    }
    reset();
}
}
