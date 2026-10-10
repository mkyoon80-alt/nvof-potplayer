#pragma once
#include <cstdint>
#include "phase_quality.hpp"
#include "motion_synthesizer.hpp"
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
// Native synthesis is the only playback backend. The old enum value is rejected.
enum class GpuInterpolationBackend { fruc, native_experimental };
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
                  bool protect_appearance = true,
                  GpuInterpolationBackend backend = GpuInterpolationBackend::native_experimental,
                  unsigned max_flow_dimension = 1920,
                  MotionCostMode cost_mode = MotionCostMode::disabled, MotionFlowOptions flow_options = {});
    ~GpuFrucEngine();
    GpuFrucEngine(const GpuFrucEngine&) = delete;
    GpuFrucEngine& operator=(const GpuFrucEngine&) = delete;
    // Also accepts P010 SDR and rounds its Y/UV code values to owned 8-bit NV12.
    // P010 selects conservative GPU completion for the engine lifetime.
    // This is bit-depth conversion, not HDR tone mapping. Other methods require NV12.
    GpuFrame copy(const GpuFrame& input);
    GpuFrame midpoint(const GpuFrame& previous, const GpuFrame& current);
    PhaseBatch<GpuFrame> interpolate_pair(const GpuFrame& previous, const GpuFrame& current, const std::vector<int64_t>& timestamps);
    // GPU target-rate resampling on native Y/UV planes, with CPU-equivalent
    // fixed-point per-byte rounding, between immutable NV12 snapshots.
    GpuFrame blend(const GpuFrame& previous, const GpuFrame& current, int64_t pts);
    void reset() noexcept;
    std::string device_name() const;
    bool queued_completion() const;
    MotionAnalysisInfo analysis_info() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
