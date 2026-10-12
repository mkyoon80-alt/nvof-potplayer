#pragma once
#include <cstdint>
#include <optional>
#include <vector>

namespace nvof {
struct AdapterCandidate {
    unsigned index;
    uint64_t luid;
    unsigned vendor;
    bool software;
    bool preferred_model = false;
};
enum class AdapterSelectionReason { retained_renderer, renderer_hint, nvidia_fallback };
struct AdapterSelection {
    unsigned index;
    uint64_t luid;
    AdapterSelectionReason reason;
};
// A renderer index is only a hint. A successfully activated renderer's LUID
// identifies its actual GPU even when DXGI enumeration order changes.
inline std::optional<AdapterSelection> select_nvidia_adapter(
    const std::vector<AdapterCandidate>& candidates,
    unsigned renderer_hint,
    std::optional<uint64_t> retained_luid = {}) noexcept {
    const AdapterCandidate* first = nullptr;
    const AdapterCandidate* preferred = nullptr;
    const AdapterCandidate* hinted = nullptr;
    for (const auto& c : candidates) {
        if (c.vendor != 0x10de || c.software) continue;
        if (retained_luid && c.luid == *retained_luid)
            return AdapterSelection{c.index, c.luid, AdapterSelectionReason::retained_renderer};
        if (c.index == renderer_hint) hinted = &c;
        if (!first) first = &c;
        if (!preferred && c.preferred_model) preferred = &c;
    }
    // Never switch an existing GPU connection to an unrelated adapter.
    if (retained_luid) return {};
    const auto* selected = hinted ? hinted : preferred ? preferred : first;
    if (!selected) return {};
    return AdapterSelection{selected->index, selected->luid,
        hinted ? AdapterSelectionReason::renderer_hint : AdapterSelectionReason::nvidia_fallback};
}
}
