#pragma once
#include "gpu_engine.hpp"
#include "pipeline.hpp"
#include <functional>
#include <optional>
namespace nvof {
struct GpuOutputFrame { GpuFrame frame; int64_t stop = 0; bool discontinuity = false; };
using GpuMidpoint = std::function<GpuFrame(const GpuFrame&, const GpuFrame&)>;
using GpuBlend = std::function<GpuFrame(const GpuFrame&, const GpuFrame&, int64_t)>;
using GpuEmit = std::function<bool(const GpuOutputFrame&)>;
// Frames passed to push must be immutable owned snapshots, not decoder slices
// whose lifetime ends when Receive returns. Delivery retains texture ownership.
class GpuHybridPipeline {
public:
    GpuHybridPipeline(Rate target, GpuMidpoint midpoint, GpuBlend blend);
    void reset();
    void push(GpuFrame frame, bool discontinuity, const GpuEmit& emit);
    void finish(int64_t last_duration, const GpuEmit& emit);
    Rate rate() const { return target_; }
private:
    int64_t tick(int64_t index) const;
    bool deliver(GpuFrame frame, bool discontinuity, const GpuEmit& emit);
    Rate target_;
    GpuMidpoint midpoint_;
    GpuBlend blend_;
    std::optional<GpuFrame> previous_;
    int64_t origin_ = 0, next_index_ = 0;
};
}
