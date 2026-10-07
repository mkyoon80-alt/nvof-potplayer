#pragma once
#include <cstdint>
#include <vector>
namespace nvof {
struct PairQuality {
    bool scene_cut = false;
    bool repetition_known = false;
    bool identical_warp_skipped = false;
    uint32_t repeated_mask = 0;
    // Phases passed through native fractional-flow refinement; unreliable pixels
    // retain FRUC output. This is not a claim that every pixel was replaced.
    uint32_t subpixel_refined_mask = 0;
    bool subpixel_unavailable = false;
    // Midpoint-only slow-motion pass; unreliable/fast pixels retain FRUC.
    // This counts a shader pass, not accepted pixels.
    uint32_t midpoint_stabilized_mask = 0;
    // Appearance shader was enabled; does not imply that any mouth was detected.
    uint32_t appearance_protected_mask = 0;
    bool midpoint_stabilization_unavailable = false;
    bool midpoint_budget_limited = false;
};
template<class FrameType> struct PhaseBatch {
    std::vector<FrameType> frames;
    PairQuality quality;
};
}
