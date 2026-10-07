#pragma once
#include "pipeline.hpp"
#include "phase_quality.hpp"
#include "source_cadence.hpp"
namespace nvof {
using InterpolatePhases = std::function<PhaseBatch<Frame>(const Frame&, const Frame&, const std::vector<int64_t>&)>;
// Validates the entire pair before delivery so quality applies to every phase.
class PhasePipeline {
public:
    explicit PhasePipeline(Rate target, InterpolatePhases interpolate, Rate source = {0, 1});
    void reset();
    void push(Frame frame, bool discontinuity, const Emit& emit);
    void finish(int64_t last_duration, const Emit& emit);
    Rate rate() const { return target_; }
    bool source_cadence_active() const noexcept { return source_cadence_.active(); }
private:
    int64_t tick(int64_t index) const;
    bool deliver(Frame frame, bool discontinuity, const Emit& emit);
    Rate target_;
    SourceCadence source_cadence_;
    std::optional<int64_t> previous_raw_pts_;
    InterpolatePhases interpolate_;
    std::optional<Frame> previous_;
    int64_t origin_ = 0, next_index_ = 0;
};
}
