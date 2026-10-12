#include "nvof/adapter_selection.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>

int main() {
    using namespace nvof;
    constexpr unsigned automatic = std::numeric_limits<unsigned>::max();
    const std::vector<AdapterCandidate> hybrid = {
        {0, 11, 0x1002, false}, {1, 22, 0x10de, false}, {2, 33, 0x1414, true}};
    auto expect = [](auto result, unsigned index, AdapterSelectionReason reason) {
        if (!result || result->index != index || result->reason != reason)
            throw std::runtime_error("wrong adapter or selection reason");
    };
    try {
        expect(select_nvidia_adapter(hybrid, 0), 1, AdapterSelectionReason::nvidia_fallback);
        expect(select_nvidia_adapter(hybrid, 2), 1, AdapterSelectionReason::nvidia_fallback);
        expect(select_nvidia_adapter(hybrid, 999), 1, AdapterSelectionReason::nvidia_fallback);
        expect(select_nvidia_adapter(hybrid, 1), 1, AdapterSelectionReason::renderer_hint);
        expect(select_nvidia_adapter(hybrid, automatic), 1, AdapterSelectionReason::nvidia_fallback);
        expect(select_nvidia_adapter(hybrid, 0, 22), 1, AdapterSelectionReason::retained_renderer);
        const std::vector<AdapterCandidate> reordered = {
            {0, 44, 0x10de, false, true}, {1, 11, 0x1002, false}, {2, 22, 0x10de, false}};
        expect(select_nvidia_adapter(reordered, 0, 22), 2, AdapterSelectionReason::retained_renderer);
        expect(select_nvidia_adapter(reordered, 2), 2, AdapterSelectionReason::renderer_hint);
        expect(select_nvidia_adapter(reordered, 1), 0, AdapterSelectionReason::nvidia_fallback);
        if (select_nvidia_adapter(reordered, 0, 99)) throw std::runtime_error("lost retained GPU must fail");
        if (select_nvidia_adapter(hybrid, 1, 11)) throw std::runtime_error("retained AMD must fail");
        if (select_nvidia_adapter({{0, 1, 0x10de, true}}, 0)) throw std::runtime_error("software GPU accepted");
        if (select_nvidia_adapter({}, 0)) throw std::runtime_error("empty enumeration accepted");
        expect(select_nvidia_adapter({{0, 22, 0x10de, false}}, 0), 0, AdapterSelectionReason::renderer_hint);
        std::cout << "PASS hybrid GPU hints, stable LUID, reordered/missing GPU, cold start, single GPU\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
