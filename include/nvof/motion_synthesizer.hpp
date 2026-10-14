#pragma once
#include <memory>
#include <functional>
#include <d3d11.h>
namespace nvof {
// Standalone NVOFA synthesis: no FRUC fallback and no CUDA transfer.
// Caller holds the shared immediate-context lock and restores pipeline state.
// prepare retains sources until the next call; outputs must not alias inputs.
enum class MotionCostMode { disabled, blend_only, confidence_fusion };
enum class MotionSessionMode { fresh, persistent };
enum class MotionFlowQuality { medium, slow };
enum class MotionGpuStage { repaired, glyphs, layers, inverse };
struct MotionFlowOptions {
    // 0 preserves capability-based 4/2/1 preference. Explicit grids must be supported.
    unsigned output_grid = 0;
    MotionFlowQuality quality = MotionFlowQuality::medium;
    // Fresh retains the pre-optimization reference path for hardware comparisons.
    MotionSessionMode session_mode = MotionSessionMode::persistent;
};
struct MotionAnalysisInfo {
    unsigned width = 0, height = 0, grid = 0, cost_buffers = 0;
    unsigned inconsistent_samples = 0;
};
class MotionSynthesizer {
public:
    MotionSynthesizer(ID3D11Device*, ID3D11DeviceContext*, unsigned max_flow_dimension = 1920, MotionCostMode cost_mode = MotionCostMode::disabled, MotionFlowOptions flow_options = {});
    ~MotionSynthesizer();
    MotionSynthesizer(const MotionSynthesizer&) = delete;
    MotionSynthesizer& operator=(const MotionSynthesizer&) = delete;
    // Optional synchronous completion boundary. It may restore/release the
    // caller's shared context while waiting, but must reacquire it and restore
    // our exact pipeline state before returning (also on failure).
    using GpuWait = std::function<void()>;
    // Optional same-pair gate runs after input completion, before NVOFA execute.
    // It may read earlier GPU statistics while the caller owns the context.
    // False leaves the pair unprepared; the caller must hold the original frame
    // and invalidate history. No earlier pair's decision may be reused.
    using PairReady = std::function<bool()>;
    // Diagnostic command markers only; never wait or read image data here.
    using GpuStageMark = std::function<void(MotionGpuStage)>;
    void prepare(ID3D11Texture2D*, ID3D11Texture2D*, int width, int height, const GpuWait& wait = {}, const PairReady& ready = {}, const GpuStageMark& trace = {});
    // False means the pair has widespread inconsistent, photometrically wrong motion.
    bool reliable() const noexcept;
    // Diagnostic: true only after a pair executed with hardware 8-bit costs.
    bool cost_map_active() const noexcept;
    MotionAnalysisInfo analysis_info() const noexcept;
    void render_phase(ID3D11RenderTargetView* y, ID3D11RenderTargetView* uv, float fraction, const GpuStageMark& trace = {});
    void render_midpoint(ID3D11RenderTargetView* y, ID3D11RenderTargetView* uv);
    // Discard prepared image references while retaining compatible allocations.
    // Every new pair overwrites both inputs and disables temporal hints.
    void invalidate_history() noexcept;
    // Release the OF session/resources, including after an execution failure.
    void reset() noexcept;
private:
    friend struct MotionSessionTestAccess;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
