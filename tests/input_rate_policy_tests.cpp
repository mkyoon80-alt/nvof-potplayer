#include "nvof/input_rate_policy.hpp"
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void expect(nvof::Rate rate, nvof::SourceRateBucket bucket) {
    require(nvof::source_rate_bucket(rate) == bucket, "source FPS classified in the wrong family");
    require(nvof::input_rate_selected(rate), "old settings without mask must preserve all-rate behavior");
    require(!nvof::input_rate_selected(rate, 0), "empty selection must bypass every rate");
    const unsigned selected = static_cast<unsigned>(bucket);
    require(nvof::input_rate_selected(rate, selected), "selected source family must interpolate");
    require(!nvof::input_rate_selected(rate, nvof::kAllInputRates ^ selected), "excluded source family must bypass");
}
}
int main() {
    try {
        using B = nvof::SourceRateBucket;
        const struct { nvof::Rate rate; B bucket; } standards[] = {
            {{24,1},B::Fps24}, {{24000,1001},B::Fps24}, {{25,1},B::Fps25},
            {{30,1},B::Fps30}, {{30000,1001},B::Fps30}, {{50,1},B::Fps50},
            {{60,1},B::Fps60}, {{60000,1001},B::Fps60},
        };
        for (const auto& entry : standards) {
            expect(entry.rate, entry.bucket);
            const int64_t duration = (10000000LL * entry.rate.den + entry.rate.num/2) / entry.rate.num;
            for (int delta = -1; delta <= 1; ++delta)
                expect({10000000, duration + delta}, entry.bucket);
        }
        // Typical media metadata rounding and the explicit tolerance boundary.
        expect({23976,1000},B::Fps24); expect({2997,100},B::Fps30);
        expect({5994,100},B::Fps60); expect({2402,100},B::Fps24);
        expect({24020001,1000000},B::Other); expect({241,10},B::Other);
        expect({2395,100},B::Other); expect({48,1},B::Other);
        expect({120,1},B::Other); expect({165,1},B::Other);
        expect({0,1},B::Other); expect({-24,1},B::Other);
        expect({24,0},B::Other); expect({-24,-1},B::Other);
        expect({(std::numeric_limits<int64_t>::max)(),1},B::Other);
        expect({(std::numeric_limits<int64_t>::max)(),(std::numeric_limits<int64_t>::max)()},B::Other);
        // A film-only setting must leave PAL/NTSC television rates untouched.
        require(nvof::input_rate_selected({24000,1001},1), "film-only should select NTSC film");
        require(!nvof::input_rate_selected({30000,1001},1), "film-only should exclude NTSC television");
        require(!nvof::input_rate_selected({24,1},64), "unknown mask bits cannot select a family");
        std::cout << "PASS source-rate policy: NTSC families, rounded frame duration, exclusion, defaults, unusual/invalid rates\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}