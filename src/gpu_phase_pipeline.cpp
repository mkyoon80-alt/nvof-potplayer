#include "nvof/gpu_phase_pipeline.hpp"
#include "nvof/output_rate_policy.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>
namespace nvof {
namespace {
DXGI_FORMAT format(const GpuFrame& f) {D3D11_TEXTURE2D_DESC d{};f.texture->GetDesc(&d);return d.Format;}
void validate(const GpuFrame& f) {
    if (!f.texture || f.width < 2 || f.height < 2 || f.width > 8192 || f.height > 8192 || (f.width & 1) || (f.height & 1) || f.pts < 0)
        throw std::invalid_argument("Expected a nonnegative, even-sized GPU NV12/P010 frame");
    D3D11_TEXTURE2D_DESC desc{};
    f.texture->GetDesc(&desc);
    if ((desc.Format != DXGI_FORMAT_NV12 && desc.Format != DXGI_FORMAT_P010) || UINT(f.width) > desc.Width || UINT(f.height) > desc.Height || f.array_slice >= desc.ArraySize)
        throw std::invalid_argument("GPU texture description does not match frame");
}
}
GpuPhasePipeline::GpuPhasePipeline(Rate target, GpuInterpolatePhases interpolate, Rate source)
    : target_(target), source_cadence_(source), interpolate_(std::move(interpolate)) {
    if (!supported_output_rate(target_)) throw std::invalid_argument("Invalid output frame rate");
    if (!interpolate_) throw std::invalid_argument("A phase interpolation engine is required");
}
void GpuPhasePipeline::reset() { previous_.reset(); previous_raw_pts_.reset(); source_cadence_.reset(); origin_ = next_index_ = 0; }
int64_t GpuPhasePipeline::tick(int64_t index) const {
    const int64_t scale = 10000000LL * target_.den;
    if (index < 0) throw std::overflow_error("Playback frame index overflow");
    const int64_t whole = index / target_.num, remainder = index % target_.num;
    if (whole > (std::numeric_limits<int64_t>::max() - scale) / scale)
        throw std::overflow_error("Playback clock overflow");
    const int64_t fraction = remainder * (scale / target_.num) + remainder * (scale % target_.num) / target_.num;
    const int64_t offset = whole * scale + fraction;
    if (origin_ > std::numeric_limits<int64_t>::max() - offset)
        throw std::overflow_error("Playback timestamp overflow");
    return origin_ + offset;
}
bool GpuPhasePipeline::deliver(GpuFrame frame, bool discontinuity, const GpuEmit& emit) {
    frame.pts = tick(next_index_);
    if (!emit(GpuOutputFrame{std::move(frame), tick(next_index_ + 1), discontinuity})) { reset(); return false; }
    ++next_index_;
    return true;
}
void GpuPhasePipeline::push(GpuFrame frame, bool discontinuity, const GpuEmit& emit) {
    try {
        validate(frame);
        // A cut is quality evidence, never a clock reset. Only a seek or invalid
        // source continuity establishes a new clock origin.
        if (previous_ && (format(frame)!=format(*previous_) || frame.width != previous_->width || frame.height != previous_->height ||
            frame.pts <= *previous_raw_pts_ || (*previous_raw_pts_ <= std::numeric_limits<int64_t>::max() - 10000000LL &&
            frame.pts > *previous_raw_pts_ + 10000000LL))) discontinuity = true;
        if (discontinuity) reset();
        const int64_t raw_pts = frame.pts;
        frame.pts = source_cadence_.normalize(raw_pts);
        // A VFR transition can fall behind a previously corrected timestamp.
        // Re-anchor this boundary instead of creating an invalid source pair.
        if (previous_ && frame.pts <= previous_->pts) {
            reset();
            frame.pts = source_cadence_.normalize(raw_pts);
        }
        previous_raw_pts_ = raw_pts;
        if (!previous_) {
            origin_ = frame.pts;
            previous_ = std::move(frame);
            deliver(*previous_, true, emit);
            return;
        }
        std::vector<int64_t> timestamps;
        for (int64_t index = next_index_; tick(index) < frame.pts; ++index) {
            const int64_t pts = tick(index);
            if (pts <= previous_->pts) throw std::logic_error("Phase clock is not strictly inside the source pair");
            if (timestamps.size() == 32) throw std::runtime_error("Source pair requires more than 32 interpolation phases");
            timestamps.push_back(pts);
        }
        PhaseBatch<GpuFrame> batch;
        {
            // Even an empty request advances a sequential engine through every original.
            batch = interpolate_(*previous_, frame, timestamps);
            if (timestamps.empty() && !batch.frames.empty())
                throw std::runtime_error("Empty phase request returned unexpected frames");
            const bool hold = batch.quality.scene_cut || batch.quality.repeated_mask != 0;
            if (!hold && batch.frames.size() != timestamps.size())
                throw std::runtime_error("Phase interpolation returned the wrong frame count");
            // Quality holds may omit generated frames; otherwise validate the whole batch first.
            for (size_t i = 0; !hold && i < timestamps.size(); ++i) {
                validate(batch.frames[i]);
                if (format(batch.frames[i])!=format(frame) || batch.frames[i].width != frame.width || batch.frames[i].height != frame.height ||
                    batch.frames[i].pts != timestamps[i])
                    throw std::runtime_error("Phase interpolation returned an invalid or out-of-order timestamp or frame size");
            }
            for (size_t i = 0; i < timestamps.size(); ++i) {
                GpuFrame output = hold ? *previous_ : std::move(batch.frames[i]);
                if (!deliver(std::move(output), false, emit)) return;
            }
        }
        // Originals are reused exactly on the canonical source clock. Cuts never
        // advance B before that clock; correction is bounded to 1 ms of raw PTS.
        if (tick(next_index_) == frame.pts && !deliver(frame, false, emit)) return;
        previous_ = std::move(frame);
    } catch (...) {
        reset();
        throw;
    }
}
void GpuPhasePipeline::finish(int64_t last_duration, const GpuEmit& emit) {
    try {
        if (!previous_) return;
        if (last_duration > 0 && last_duration <= 10000000LL) {
            if (previous_->pts > std::numeric_limits<int64_t>::max() - last_duration)
                throw std::overflow_error("End timestamp overflow");
            const int64_t end = source_cadence_.normalize_end(previous_->pts + last_duration);
            while (tick(next_index_) < end) {
                GpuFrame frame = *previous_;
                frame.pts = tick(next_index_);
                if (!emit(GpuOutputFrame{std::move(frame), std::min(tick(next_index_ + 1), end), false})) { reset(); return; }
                ++next_index_;
            }
        }
        reset();
    } catch (...) {
        reset();
        throw;
    }
}
}
