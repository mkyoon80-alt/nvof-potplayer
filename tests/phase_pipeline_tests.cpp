#include "nvof/phase_pipeline.hpp"
#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
using namespace nvof;
void initialize() {}
Frame make(int64_t pts, uint8_t tag) { return Frame{8, 4, pts, std::vector<uint8_t>(48, tag)}; }
int level(const Frame& frame) { return frame.pixels[0]; }
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int64_t clock_at(Rate rate, int64_t index) { return index * 10000000LL * rate.den / rate.num; }
PhaseBatch<Frame> phases(const Frame& a, const Frame& b, const std::vector<int64_t>& times) {
    PhaseBatch<Frame> result;
    int64_t last = a.pts;
    for (size_t i = 0; i < times.size(); ++i) {
        require(times[i] > last && times[i] < b.pts, "callback received a non-interior or unordered phase");
        result.frames.push_back(make(times[i], uint8_t(100 + i)));
        last = times[i];
    }
    return result;
}
void cadence(Rate source, Rate target) {
    std::vector<OutputFrame> output;
    int calls = 0;
    PhasePipeline pipeline(target, [&](const Frame& a, const Frame& b, const std::vector<int64_t>& times) {
        ++calls; return phases(a, b, times);
    });
    auto emit = [&](const OutputFrame& frame) { output.push_back(frame); return true; };
    constexpr int count = 240;
    for (int i = 0; i < count; ++i) pipeline.push(make(clock_at(source, i), uint8_t(i % 90)), false, emit);
    const int64_t end = clock_at(source, count);
    pipeline.finish(end - clock_at(source, count - 1), emit);
    size_t expected = 0;
    while (clock_at(target, int64_t(expected)) < end) ++expected;
    require(output.size() == expected, "real-FPS cadence count mismatch");
    require(calls <= count - 1, "more than one batch requested per source pair");
    require(calls > 0, "interpolation was not exercised");
    for (size_t i = 0; i < output.size(); ++i) {
        require(output[i].frame.pts == clock_at(target, int64_t(i)), "rational cadence drift");
        require(output[i].stop == std::min(clock_at(target, int64_t(i + 1)), end), "incorrect target sample stop");
        require(output[i].discontinuity == (i == 0), "ordinary pair changed discontinuity state");
    }
    const auto before = output.size();
    pipeline.finish(400000, emit);
    require(output.size() == before, "EOS retained old frames");
}
void phase_and_original() {
    std::vector<OutputFrame> output;
    int calls = 0;
    PhasePipeline p({120, 1}, [&](const Frame& a, const Frame& b, const std::vector<int64_t>& times) {
        ++calls;
        require(times == std::vector<int64_t>({83333, 166666, 250000}), "incorrect direct motion phase timestamps");
        return phases(a, b, times);
    });
    auto emit = [&](const OutputFrame& f) { output.push_back(f); return true; };
    p.push(make(0, 11), false, emit);
    require(output.size() == 1 && calls == 0 && output[0].discontinuity, "first original is not immediate");
    p.push(make(333333, 22), false, emit);
    require(calls == 1 && output.size() == 5, "pair batch or exact original delivery mismatch");
    for (int i = 1; i <= 3; ++i) require(level(output[i].frame) == 99 + i, "generated phase was blended or substituted");
    require(level(output.back().frame) == 22 && output.back().frame.pts == 333333, "exact B tick did not reuse B");
}
void cut_and_repetition() {
    std::vector<OutputFrame> output;
    int calls = 0;
    PhasePipeline cut({60000, 1001}, [&](const Frame&, const Frame&, const std::vector<int64_t>&) {
        ++calls;
        PhaseBatch<Frame> result;
        result.quality.scene_cut = true; // Scene detection can avoid FRUC entirely.
        return result;
    });
    auto emit = [&](const OutputFrame& f) { output.push_back(f); return true; };
    for (int i = 0; i < 5; ++i) cut.push(make(int64_t(i) * 416667, uint8_t(10 + i)), false, emit);
    require(calls == 4, "cut should inspect one batch per pair");
    for (size_t i = 0; i < output.size(); ++i) {
        const auto expected = clock_at({60000, 1001}, int64_t(i));
        require(output[i].frame.pts == expected, "off-grid cut reset the rational clock");
        require(level(output[i].frame) == 10 + expected / 416667, "cut emitted B early or blended across boundary");
        require(output[i].discontinuity == (i == 0), "cut was treated as a seek");
    }
    output.clear();
    PhasePipeline repeated({120, 1}, [&](const Frame& a, const Frame& b, const std::vector<int64_t>& times) {
        auto result = phases(a, b, times);
        result.quality.repetition_known = true;
        result.quality.repeated_mask = 4; // Only the final phase reports repetition.
        return result;
    });
    repeated.push(make(0, 31), false, emit);
    repeated.push(make(333333, 42), false, emit);
    require(output.size() == 5, "repetition output count wrong");
    for (int i = 0; i < 4; ++i) require(level(output[i].frame) == 31, "late repetition evidence did not suppress entire pair");
    require(level(output[4].frame) == 42, "repetition held B beyond its exact tick");
}
void cancellation_seek_eos() {
    std::vector<OutputFrame> output;
    int calls = 0;
    PhasePipeline p({120, 1}, [&](const Frame& a, const Frame& b, const std::vector<int64_t>& t) {
        ++calls; return phases(a, b, t);
    });
    auto emit = [&](const OutputFrame& f) { output.push_back(f); return true; };
    p.push(make(0, 10), false, emit);
    int attempts = 0;
    p.push(make(333333, 20), false, [&](const OutputFrame&) { return ++attempts < 2; });
    require(attempts == 2 && calls == 1, "cancellation did not abort the remaining pair");
    p.push(make(500000, 30), false, emit);
    require(output.back().frame.pts == 500000 && output.back().discontinuity && calls == 1, "cancel retained pair history");
    p.push(make(125, 40), true, emit);
    require(output.back().frame.pts == 125 && output.back().discontinuity && level(output.back().frame) == 40, "seek leaked old history");
    for (int i = 0; i < 1000; ++i) {
        p.reset();
        p.push(make(i % 2 ? 90000000 : 0, uint8_t(i % 90)), false, emit);
    }
    require(calls == 1, "seek preview invoked interpolation");
    p.reset();
    p.push(make(0, 55), false, [](const OutputFrame&) { return false; });
    p.push(make(1000000, 66), false, emit);
    require(calls == 1 && output.back().discontinuity, "first-frame cancellation retained history");
    p.reset(); output.clear();
    p.push(make(0, 77), false, emit);
    p.finish(200000, emit);
    require(output.size() == 3 && output.back().stop == 200000, "EOS did not clip the last target sample");
    for (const auto& f : output) require(level(f.frame) == 77, "EOS did not hold final original");
    p.finish(200000, emit);
    require(output.size() == 3, "duplicate EOS output");
    p.push(make(2000000, 88), false, emit);
    p.finish(200000, [](const OutputFrame&) { return false; });
    p.push(make(3000000, 89), false, emit);
    require(output.back().discontinuity && calls == 1, "EOS cancellation retained history");
}
void faults() {
    for (int fault = 0; fault < 5; ++fault) {
        int emitted = 0;
        PhasePipeline p({120, 1}, [&](const Frame& a, const Frame& b, const std::vector<int64_t>& times) {
            if (fault == 0) throw std::runtime_error("engine failed");
            auto result = phases(a, b, times);
            if (fault == 1) result.frames.pop_back();
            if (fault == 2) result.frames.back().pts -= 1;
            if (fault == 3) result.frames.back().width = 2;
            if (fault == 4) std::swap(result.frames.front(), result.frames.back());
            return result;
        });
        auto emit = [&](const OutputFrame&) { ++emitted; return true; };
        p.push(make(0, 1), false, emit);
        bool caught = false;
        try { p.push(make(333333, 2), false, emit); } catch (const std::exception&) { caught = true; }
        require(caught && emitted == 1, "batch fault emitted partial pair or was hidden");
        p.push(make(500000, 3), false, [&](const OutputFrame& f) {
            require(f.discontinuity && f.frame.pts == 500000, "fault did not reset scheduler"); return true;
        });
    }
    int calls = 0;
    PhasePipeline limit({1000, 1}, [&](const Frame& a, const Frame& b, const std::vector<int64_t>& times) {
        ++calls; require(times.size() == 32, "32-phase boundary mismatch"); return phases(a, b, times);
    });
    auto discard = [](const OutputFrame&) { return true; };
    limit.push(make(0, 1), false, discard);
    limit.push(make(330000, 2), false, discard);
    require(calls == 1, "valid 32-phase pair rejected");
    limit.reset(); limit.push(make(0, 1), false, discard);
    bool caught = false;
    try { limit.push(make(340000, 2), false, discard); }
    catch (const std::runtime_error& e) { caught = std::string(e.what()).find("32") != std::string::npos; }
    require(caught && calls == 1, "excess phase count was truncated or sent to engine");
    PhasePipeline p({60, 1}, phases);
    p.push(make(0, 1), false, discard);
    caught = false;
    try { p.push(make(416667, 2), false, [](const OutputFrame&) -> bool { throw std::runtime_error("sink failed"); }); }
    catch (const std::runtime_error&) { caught = true; }
    require(caught, "sink exception hidden");
    p.push(make(600000, 3), false, [](const OutputFrame& f) { require(f.discontinuity, "sink exception retained history"); return true; });
}
void empty_requests() {
    int calls = 0, empty = 0, outputs = 0;
    PhasePipeline p({60, 1}, [&](const Frame& a, const Frame& b, const std::vector<int64_t>& times) {
        ++calls;
        if (times.empty()) ++empty;
        return phases(a, b, times);
    });
    auto emit = [&](const OutputFrame&) { ++outputs; return true; };
    p.push(make(0, 10), false, emit);
    p.push(make(100000, 20), false, emit);
    require(calls == 1 && empty == 1 && outputs == 1, "empty phase pair did not advance engine history");
    p.push(make(166666, 30), false, emit);
    require(calls == 2 && empty == 2 && outputs == 2, "empty request lost exact original tick");
    PhasePipeline bad({60, 1}, [](const Frame& a, const Frame&, const std::vector<int64_t>&) {
        PhaseBatch<Frame> result;
        result.quality.scene_cut = true;
        result.frames.push_back(a);
        return result;
    });
    bad.push(make(0, 1), false, emit);
    bool caught = false;
    try { bad.push(make(100000, 2), false, emit); } catch (const std::runtime_error&) { caught = true; }
    require(caught, "empty request accepted unexpected generated frames");
}
void long_clock() {
    const Rate rate{23999999, 100000};
    PhasePipeline p(rate, phases);
    int64_t index = 0;
    auto emit = [&](const OutputFrame& f) {
        const int64_t expected = int64_t(static_cast<long double>(index) * 10000000 * rate.den / rate.num);
        require(f.frame.pts == expected, "long rational clock overflow or drift");
        ++index; return true;
    };
    for (int i = 0; i < 3000; ++i) p.push(make(int64_t(i) * 10000000 / 24, 1), false, emit);
    require(index > 29000, "long clock test did not exercise enough output frames");
}

// Matroska H.264 commonly supplies 1/1000 PTS even for 24000/1001 video.
// mode 0: nearest ms; mode 1: floor ms; mode 2: nearest 100 ns.
int64_t quantized_source_pts(Rate rate, int64_t index, int64_t phase, int mode) {
    const int64_t quantum = mode == 2 ? 1 : 10000;
    const int64_t denominator = rate.num * quantum;
    const int64_t numerator = phase * rate.num + index * 10000000LL * rate.den;
    return (numerator + (mode == 1 ? 0 : denominator / 2)) / denominator * quantum;
}
void source_cadence_normalization() {
    for (const Rate rate : {Rate{24000,1001}, Rate{30000,1001}, Rate{60000,1001}, Rate{24,1}}) {
        const int64_t frames = 3600LL * rate.num / rate.den + 1;
        for (int mode = 0; mode < 3; ++mode) for (const int64_t phase : {0LL, 1234567LL, 87654321LL}) {
            SourceCadence clock(rate);
            const int64_t origin = quantized_source_pts(rate, 0, phase, mode);
            for (int64_t i = 0; i < frames; ++i) {
                const auto raw = quantized_source_pts(rate, i, phase, mode);
                require(clock.normalize(raw) == origin + clock_at(rate, i), "one-hour quantized source cadence drift");
            }
            require(clock.active(), "valid quantized source cadence lost alignment");
            const auto last = origin + clock_at(rate, frames - 1);
            const auto duration = quantized_source_pts(rate, frames, phase, mode) - quantized_source_pts(rate, frames - 1, phase, mode);
            require(clock.normalize_end(last + duration) == origin + clock_at(rate, frames), "quantized EOS added an extra target tick");
            require(clock.normalize_end(last + 100000) == last + 100000, "clipped EOS was extended to full source frame");
        }
    }
    const Rate source{24000,1001};
    for (const int64_t delta : {-10001LL, -10000LL, 10000LL, 10001LL}) {
        SourceCadence clock(source);
        require(clock.normalize(1234567) == 1234567, "source origin was rounded");
        const auto expected = 1234567 + clock_at(source, 1);
        const bool inside = delta >= -10000 && delta <= 10000;
        require(clock.normalize(expected + delta) == (inside ? expected : expected + delta), "1 ms normalization boundary is wrong");
        require(clock.active() == inside, "cadence active status does not report a deviation");
        if (!inside) {
            const auto later = 1234567 + clock_at(source, 2) + 1;
            require(clock.normalize(later) == later, "VFR correction resumed without a reset");
            require(clock.normalize_end(later + 420000) == later + 420000, "VFR EOS was retimed");
        }
        clock.reset();
        require(!clock.active(), "reset retained source cadence alignment");
        const int64_t seek_origin = 7654321;
        require(clock.normalize(seek_origin) == seek_origin, "seek origin was changed");
        require(clock.normalize(seek_origin + 420000) == seek_origin + clock_at(source, 1), "seek did not establish a fresh cadence");
    }
    SourceCadence unknown;
    require(unknown.normalize(0) == 0 && unknown.normalize(420000) == 420000 && !unknown.active(), "unknown source rate changed timestamps");
    const auto maximum = (std::numeric_limits<int64_t>::max)();
    SourceCadence large(source);
    require(large.normalize(maximum - 10) == maximum - 10, "large source origin changed");
    require(large.normalize(maximum - 1) == maximum - 1 && !large.active(), "source clock overflow was not disabled safely");
    for (const Rate rate : {Rate{-1,1}, Rate{1,0}, Rate{1,10000001}, Rate{1001,1}}) {
        bool caught = false;
        try { SourceCadence invalid(rate); } catch (const std::invalid_argument&) { caught = true; }
        require(caught, "invalid source rate was accepted");
    }
    bool caught = false;
    try { unknown.normalize(-1); } catch (const std::invalid_argument&) { caught = true; }
    require(caught, "negative source timestamp was accepted");
}
void quantized_pipeline_cadence() {
    const Rate source{24000,1001};
    constexpr int count = 240;
    for (const Rate target : {Rate{48000,1001}, Rate{60,1}, Rate{60000,1001}, Rate{120,1}, Rate{120000,1001}}) {
        for (int mode = 0; mode < 3; ++mode) for (const int64_t offset : {0LL, 1234567LL}) {
            const int64_t origin = quantized_source_pts(source, 0, offset, mode);
            std::vector<OutputFrame> actual, reference;
            int calls = 0, generated = 0;
            PhasePipeline corrected(target, [&](const Frame& a, const Frame& b, const std::vector<int64_t>& times) {
                ++calls;
                require(a.pts == origin + clock_at(source, calls - 1) && b.pts == origin + clock_at(source, calls), "engine received raw quantization jitter");
                generated += int(times.size());
                if (target.num == 48000) {
                    require(times.size() == 1, "x2 requested an extra near-original FRUC phase");
                    const int64_t twice_elapsed = 2 * (times[0] - a.pts);
                    require(std::llabs(twice_elapsed - (b.pts - a.pts)) <= 2, "x2 FRUC request was not a midpoint");
                }
                return phases(a, b, times);
            }, source);
            PhasePipeline ideal(target, phases);
            auto emit_actual = [&](const OutputFrame& output) { actual.push_back(output); return true; };
            auto emit_reference = [&](const OutputFrame& output) { reference.push_back(output); return true; };
            for (int i = 0; i < count; ++i) {
                corrected.push(make(quantized_source_pts(source, i, offset, mode), uint8_t(i % 90)), false, emit_actual);
                ideal.push(make(origin + clock_at(source, i), uint8_t(i % 90)), false, emit_reference);
            }
            require(corrected.source_cadence_active(), "pipeline lost known source cadence");
            corrected.finish(quantized_source_pts(source, count, offset, mode) - quantized_source_pts(source, count - 1, offset, mode), emit_actual);
            ideal.finish(clock_at(source, count) - clock_at(source, count - 1), emit_reference);
            require(actual.size() == reference.size(), "quantized pipeline output count differs from rational reference");
            for (size_t i = 0; i < actual.size(); ++i) {
                require(actual[i].frame.pts == reference[i].frame.pts && actual[i].stop == reference[i].stop &&
                    actual[i].discontinuity == reference[i].discontinuity && level(actual[i].frame) == level(reference[i].frame),
                    "quantized pipeline changed output timing, original identity, or motion phase");
            }
            if (target.num == 48000) {
                require(generated == count - 1, "x2 generated more than one motion phase per source pair");
                for (int i = 0; i < count; ++i) require(level(actual[size_t(i) * 2].frame) == i % 90, "x2 original was replaced by a near-endpoint interpolation");
            }
        }
    }
    std::vector<OutputFrame> output;
    PhasePipeline cut({48000,1001}, [](const Frame&, const Frame&, const std::vector<int64_t>&) {
        PhaseBatch<Frame> result; result.quality.scene_cut = true; return result;
    }, source);
    auto emit = [&](const OutputFrame& frame) { output.push_back(frame); return true; };
    cut.push(make(0, 11), false, emit);
    cut.push(make(420000, 22), false, emit);
    require(output.size() == 3 && level(output[1].frame) == 11 && level(output[2].frame) == 22 && output[2].frame.pts == clock_at(source,1), "cut crossed the canonical source boundary");
    cut.push(make(1234567, 33), true, emit);
    require(output.back().frame.pts == 1234567 && output.back().discontinuity && level(output.back().frame) == 33, "normalized pipeline seek retained old origin");
    cut.push(make(1654567, 44), false, emit);
    require(output.back().frame.pts == 1234567 + clock_at(source,1) && level(output.back().frame) == 44, "normalized pipeline did not align the new seek cadence");
    PhasePipeline vfr({48000,1001}, phases, source);
    auto discard = [](const OutputFrame&) { return true; };
    vfr.push(make(0, 1), false, discard);
    vfr.push(make(450000, 2), false, discard);
    require(!vfr.source_cadence_active(), "true VFR retained cadence correction");
    vfr.push(make(834167, 3), false, discard);
    require(!vfr.source_cadence_active(), "VFR cadence correction resumed without a seek");
}

int main() {
    try {
        initialize();
        for (Rate source : {Rate{24000,1001}, Rate{30000,1001}, Rate{30,1}, Rate{60,1}})
            for (Rate target : {Rate{60000,1001}, Rate{120000,1001}, Rate{144,1}, Rate{240,1}})
                cadence(source, target);
        source_cadence_normalization();
        quantized_pipeline_cadence();
        empty_requests();
        long_clock();
        phase_and_original();
        cut_and_repetition();
        cancellation_seek_eos();
        faults();
        std::cout << "PASS: 1-hour quantized cadence, VFR/seek/overflow, exact x2 originals, real-FPS direct phases, rational clocks, off-grid cuts, pair-wide repetition, cancellation, faults, seek, EOS, 32-phase limit\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
