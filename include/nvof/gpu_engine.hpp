#pragma once
#include <cstdint>
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
};
// 8-bit NV12 GPU-only image path. Native Y/UV processing preserves stored
// channel values; the caller remains responsible for supported color metadata.
// Input textures must remain unchanged until
// the method returns. For decoder arrays, retain IMediaSample through copy().
// All returned textures are engine-owned snapshots with completed GPU writes.
class GpuFrucEngine {
public:
    GpuFrucEngine(const std::filesystem::path& runtime_directory,
                  ID3D11Device* device, ID3D11DeviceContext* context,
                  HANDLE borrowed_decoder_mutex = nullptr);
    ~GpuFrucEngine();
    GpuFrucEngine(const GpuFrucEngine&) = delete;
    GpuFrucEngine& operator=(const GpuFrucEngine&) = delete;
    GpuFrame copy(const GpuFrame& input);
    GpuFrame midpoint(const GpuFrame& previous, const GpuFrame& current);
    // GPU target-rate resampling on native Y/UV planes, with CPU-equivalent
    // fixed-point per-byte rounding, between immutable NV12 snapshots.
    GpuFrame blend(const GpuFrame& previous, const GpuFrame& current, int64_t pts);
    void reset() noexcept;
    std::string device_name() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

