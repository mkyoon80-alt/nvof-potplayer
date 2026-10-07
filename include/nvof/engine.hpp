#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
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
    // Returns a packed NV12 frame halfway between two strictly ordered frames.
    // Keeps one sequential FRUC session, re-primes when previous is not cached.
    // Throws on GPU/runtime failure. Never silently substitutes a blend.
    Frame midpoint(const Frame& previous, const Frame& current);
    void reset() noexcept;
    std::string device_name() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
