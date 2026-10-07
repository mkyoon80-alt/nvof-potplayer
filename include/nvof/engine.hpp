#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include "nvof/phase_quality.hpp"
namespace nvof {
struct Frame {
    int width = 0;
    int height = 0;
    int64_t pts = 0; // DirectShow units: 100 ns; packed NV12, stride == width.
    std::vector<uint8_t> pixels;
};
class FrucEngine {
public:
    explicit FrucEngine(const std::filesystem::path& runtime_directory);
    ~FrucEngine();
    FrucEngine(const FrucEngine&) = delete;
    FrucEngine& operator=(const FrucEngine&) = delete;
    // Generate up to 32 strictly increasing timestamps inside an original pair.
    // Independent FRUC histories share one upload of each original. Empty requests
    // advance existing histories. Hard cuts return no frames and close history.
    // Output frames own their pixels; no input memory is retained by reference.
    PhaseBatch<Frame> interpolate_pair(const Frame& previous, const Frame& current,
                                      const std::vector<int64_t>& timestamps);
    // Compatibility helper: holds the previous original when quality rejects
    // motion; otherwise returns the genuine halfway phase, never a blend.
    Frame midpoint(const Frame& previous, const Frame& current);
    void reset() noexcept;
    std::string device_name() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
