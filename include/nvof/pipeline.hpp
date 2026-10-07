#pragma once
#include "engine.hpp"
#include <functional>
#include <optional>
namespace nvof {
struct Rate { int64_t num=60000; int64_t den=1001; };
struct OutputFrame { Frame frame; int64_t stop=0; bool discontinuity=false; };
using Midpoint = std::function<Frame(const Frame&, const Frame&)>;
using Emit = std::function<bool(const OutputFrame&)>;
class HybridPipeline {
public:
    explicit HybridPipeline(Rate target, Midpoint midpoint);
    void reset();
    // Emits the first original immediately after reset for responsive seeking.
    // Then resamples previous/original-midpoint/current onto the target clock.
    // callback false cancels delivery and clears retained history.
    void push(Frame frame, bool discontinuity, const Emit& emit);
    void finish(int64_t last_duration, const Emit& emit);
    Rate rate() const { return target_; }
private:
    int64_t tick(int64_t index) const;
    bool deliver(Frame frame, bool discontinuity, const Emit& emit);
    Rate target_;
    Midpoint midpoint_;
    std::optional<Frame> previous_;
    int64_t origin_=0, next_index_=0;
};
}
