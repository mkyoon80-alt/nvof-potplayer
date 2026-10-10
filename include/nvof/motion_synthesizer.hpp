#pragma once
#include <memory>
#include <d3d11.h>
namespace nvof {
// Standalone NVOFA synthesis: no FRUC fallback and no CUDA transfer.
// Caller holds the shared immediate-context lock and restores pipeline state.
// prepare retains sources until the next call; outputs must not alias inputs.
class MotionSynthesizer {
public:
    MotionSynthesizer(ID3D11Device*, ID3D11DeviceContext*, unsigned max_flow_dimension = 1920);
    ~MotionSynthesizer();
    MotionSynthesizer(const MotionSynthesizer&) = delete;
    MotionSynthesizer& operator=(const MotionSynthesizer&) = delete;
    void prepare(ID3D11Texture2D*, ID3D11Texture2D*, int width, int height);
    // False means the pair has widespread inconsistent, photometrically wrong motion.
    bool reliable() const noexcept;
    // Diagnostic: true only after a pair executed with hardware 8-bit costs.
    bool cost_map_active() const noexcept;
    void render_phase(ID3D11RenderTargetView* y, ID3D11RenderTargetView* uv, float fraction);
    void render_midpoint(ID3D11RenderTargetView* y, ID3D11RenderTargetView* uv);
    void invalidate_history() noexcept;
    void reset() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
