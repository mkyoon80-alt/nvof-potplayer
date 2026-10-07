#pragma once
#include <cstdint>
#include "phase_quality.hpp"
#include <vector>
#include <filesystem>
#include <memory>
#include <string>
#include <d3d11.h>
#include <wrl/client.h>
namespace nvof {
struct GpuFrame {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    UINT array_slice = 0;
    int width = 0;
    int height = 0;
    int64_t pts = 0;
    std::shared_ptr<void> lease; // Keeps a pooled surface reserved until every owner releases it.
};
enum class GpuCompletionMode { blocking, context_ordered };
// 8-bit NV12 GPU-only image path. Native Y/UV processing preserves stored
// channel values; the caller remains responsible for supported color metadata.
// Input textures must remain unchanged until
// the method returns. For decoder arrays, retain IMediaSample through copy().
// Returned textures are engine-owned snapshots. The default waits for GPU
// completion. context_ordered instead submits all work to this device's shared
// immediate context before returning: consumers MUST submit their GPU use to
// that same context. D3D command/resource ordering then protects source reuse;
// consumers of other GPU APIs must establish their own completion dependency.
class GpuFrucEngine {
public:
    GpuFrucEngine(const std::filesystem::path& runtime_directory,
                  ID3D11Device* device, ID3D11DeviceContext* context,
                  HANDLE borrowed_decoder_mutex = nullptr,
                  GpuCompletionMode completion = GpuCompletionMode::blocking,
                  bool skip_identical_warp = true,
                  bool stabilize_midpoint = true,
                  bool protect_appearance = true); // Independent optional GPU passes.
    ~GpuFrucEngine();
    GpuFrucEngine(const GpuFrucEngine&) = delete;
    GpuFrucEngine& operator=(const GpuFrucEngine&) = delete;
    GpuFrame copy(const GpuFrame& input);
    GpuFrame midpoint(const GpuFrame& previous, const GpuFrame& current);
    PhaseBatch<GpuFrame> interpolate_pair(const GpuFrame& previous, const GpuFrame& current, const std::vector<int64_t>& timestamps);
    // GPU target-rate resampling on native Y/UV planes, with CPU-equivalent
    // fixed-point per-byte rounding, between immutable NV12 snapshots.
    GpuFrame blend(const GpuFrame& previous, const GpuFrame& current, int64_t pts);
    void reset() noexcept;
    std::string device_name() const;
    bool queued_completion() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
