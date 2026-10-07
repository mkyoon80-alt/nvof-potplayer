#pragma once
#include <cstdint>
#include <array>
#include <memory>
#include <d3d11.h>

namespace nvof {
// GPU-only motion reconstruction, with separate experimental fractional and
// guarded slow-midpoint entry points. The caller holds the
// shared immediate-context lock for construction, calls and destruction,
// restores pipeline state if needed, and waits before exposing the output.
// Inputs are single-slice NV12 snapshots; prepare owns its GPU copies.
class FractionalRefiner {
public:
    // Disabling appearance protection is for comparison diagnostics.
    FractionalRefiner(ID3D11Device* device, ID3D11DeviceContext* context,
                      bool protect_appearance = true);
    ~FractionalRefiner();
    FractionalRefiner(const FractionalRefiner&) = delete;
    FractionalRefiner& operator=(const FractionalRefiner&) = delete;
    void prepare(ID3D11Texture2D* previous, ID3D11Texture2D* current,
                 int width, int height);
    // Targets and fallbacks must be different resources. Formats are R8_UNORM
    // for Y, R8G8_UNORM for UV. Unreliable warps preserve the FRUC fallback.
    void render(float t, ID3D11RenderTargetView* target_y,
                ID3D11RenderTargetView* target_uv,
                ID3D11ShaderResourceView* fallback_y,
                ID3D11ShaderResourceView* fallback_uv);
    // Midpoint-only path with bounded motion and discrete-drawing protection.
    void render_midpoint(ID3D11RenderTargetView* target_y,
                         ID3D11RenderTargetView* target_uv,
                         ID3D11ShaderResourceView* fallback_y,
                         ID3D11ShaderResourceView* fallback_uv);
    // Reset on discontinuity/seek. No caller-owned host fence is waited here.
    void reset() noexcept;
    // Forget temporal hints after skipped pairs without reallocating textures.
    void invalidate_history() noexcept;
    bool ready() const noexcept;
    uint32_t grid_size() const noexcept;
    // Explicit blocking test diagnostic; never used by playback. Counts only,
    // no video readback. Returns reliable, moving and six speed-binned coherent-vote counts.
    std::array<uint32_t,8> diagnostic_motion_evidence() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
using D3D11FractionalRefiner = FractionalRefiner;
}
