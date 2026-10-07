#include "nvof/gpu_pipeline.hpp"
#include <d3d11.h>
#include <wrl/client.h>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace nvof;
using Microsoft::WRL::ComPtr;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int main() { try {
    ComPtr<ID3D11Device> device;
    require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr)), "D3D11 device creation failed");
    D3D11_TEXTURE2D_DESC desc{}; desc.Width = 8; desc.Height = 4; desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
    ComPtr<ID3D11Texture2D> texture;
    require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)), "NV12 texture creation failed");
    auto make = [&](int64_t pts) { return GpuFrame{texture, 0, 8, 4, pts}; };
    int motion_calls = 0;
    auto midpoint = [&](const GpuFrame& a, const GpuFrame& b) { ++motion_calls; return make(a.pts + (b.pts - a.pts) / 2); };
    auto blend = [&](const GpuFrame&, const GpuFrame&, int64_t pts) { return make(pts); };
    // Callback stubs intentionally test only scheduling and COM ownership;
    // real pixel quality and no-readback processing use gpu_engine_smoke.
    for (int target : {60, 120}) {
        std::vector<GpuOutputFrame> output;
        GpuHybridPipeline p({target * 1000LL, 1001}, midpoint, blend);
        auto emit = [&](const GpuOutputFrame& f) { output.push_back(f); return true; };
        for (int i = 0; i < 240; ++i) p.push(make(int64_t(i) * 10000000 * 1001 / 24000), false, emit);
        p.finish(10000000LL * 1001 / 24000, emit);
        require(output.size() == size_t(target * 10), "wrong cadence count");
        for (size_t i = 0; i < output.size(); ++i) {
            require(output[i].frame.pts == int64_t(i) * 10000000 * 1001 / (target * 1000), "timestamp drift");
            require(output[i].stop > output[i].frame.pts, "invalid sample duration");
            if (i) require(output[i-1].stop == output[i].frame.pts, "timestamp gap");
        }
    }
    GpuHybridPipeline p({60, 1}, midpoint, blend);
    std::vector<GpuOutputFrame> output;
    auto emit = [&](const GpuOutputFrame& f) { output.push_back(f); return true; };
    motion_calls = 0;
    p.push(make(10000000), false, emit);
    require(output.size() == 1 && motion_calls == 0 && output.back().discontinuity, "first frame waits for motion engine");
    p.push(make(10416667), false, emit);
    require(motion_calls == 1, "not exactly one midpoint per pair");
    size_t before = output.size();
    p.push(make(0), true, emit);
    require(output.size() == before + 1 && output.back().frame.pts == 0 && output.back().discontinuity, "stale seek output");
    for (int i = 0; i < 1000; ++i) { p.reset(); p.push(make(i % 2 ? 90000000 : 0), false, emit); }
    require(motion_calls == 1, "seek preview invokes midpoint");
    p.reset(); p.push(make(0), false, [](const auto&) { return false; });
    before = output.size(); p.push(make(20000000), false, emit);
    require(output.size() == before + 1 && output.back().discontinuity, "cancel retained history");
    GpuHybridPipeline precise({23999999, 100000}, midpoint, blend);
    int64_t index = 0;
    for (int i = 0; i < 3000; ++i) precise.push(make(int64_t(i) * 10000000 / 24), false, [&](const GpuOutputFrame& f) {
        require(f.frame.pts == int64_t(static_cast<long double>(index) * 10000000 * 100000 / 23999999), "high-precision overflow"); ++index; return true;
    });
    require(index > 29000, "timing regression too short");
    bool rejected = false;
    try { p.push(GpuFrame{}, false, emit); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "invalid texture accepted");
    GpuHybridPipeline failure({60, 1}, [](const GpuFrame&, const GpuFrame&) -> GpuFrame { throw std::runtime_error("GPU error"); }, blend);
    failure.push(make(0), false, emit); rejected = false;
    try { failure.push(make(416667), false, emit); } catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "GPU error hidden");
    std::cout << "PASS GPU scheduling: 59.94/119.88 cadence, first original, backward seek, 1000 resets, cancel, long rational clock, texture validation, explicit errors\n";
    return 0;
} catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; } }
