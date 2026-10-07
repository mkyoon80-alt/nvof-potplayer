#pragma once
#include "gpu_pipeline.hpp"
#include "phase_quality.hpp"
#include "source_cadence.hpp"
namespace nvof {
using GpuInterpolatePhases = std::function<PhaseBatch<GpuFrame>(const GpuFrame&, const GpuFrame&, const std::vector<int64_t>&)>;
// Validates the entire pair before delivery so quality applies to every phase.
class GpuPhasePipeline {
public:
    explicit GpuPhasePipeline(Rate target, GpuInterpolatePhases interpolate, Rate source = {0, 1});
    void reset();
    void push(GpuFrame frame, bool discontinuity, const GpuEmit& emit);
    void finish(int64_t last_duration, const GpuEmit& emit);
    Rate rate() const { return target_; }
    bool source_cadence_active() const noexcept { return source_cadence_.active(); }
private:
    int64_t tick(int64_t index) const;
    bool deliver(GpuFrame frame, bool discontinuity, const GpuEmit& emit);
    Rate target_;
    SourceCadence source_cadence_;
    std::optional<int64_t> previous_raw_pts_;
    GpuInterpolatePhases interpolate_;
    std::optional<GpuFrame> previous_;
    int64_t origin_ = 0, next_index_ = 0;
};
}
