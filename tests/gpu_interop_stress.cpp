#include "nvof/gpu_engine.hpp"
#include <d3d10_1.h>
#include <dxgi1_2.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using nvof::GpuFrame;
using nvof::GpuFrucEngine;
namespace {
void check(HRESULT value, const char* operation) {
    if (FAILED(value)) throw std::runtime_error(std::string(operation) + " HRESULT=" + std::to_string(static_cast<unsigned long>(value)));
}
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct ContextLock {
    ID3D10Multithread* context;
    HANDLE mutex;
    ContextLock(ID3D10Multithread* value, HANDLE shared_mutex) : context(value), mutex(shared_mutex) {
        if (mutex) {
            const DWORD result = WaitForSingleObject(mutex, 10000);
            require(result == WAIT_OBJECT_0 || result == WAIT_ABANDONED, "Test decoder mutex timed out");
        }
        context->Enter();
    }
    ~ContextLock() { context->Leave(); if (mutex) ReleaseMutex(mutex); }
};
struct MutexHandle {
    HANDLE value = CreateMutexW(nullptr, FALSE, nullptr);
    MutexHandle() { require(value != nullptr, "Create test decoder mutex failed"); }
    ~MutexHandle() { CloseHandle(value); }
};
uint32_t hash(uint32_t value) {
    value ^= value >> 16; value *= 0x7feb352d; value ^= value >> 15;
    value *= 0x846ca68b; return value ^ (value >> 16);
}
std::vector<uint8_t> pixels(int width, int height, int shift) {
    std::vector<uint8_t> result(size_t(width) * height * 3 / 2, 128);
    const auto cell = [](int x, int y) { return 32.f + float(hash(uint32_t(x) * 733U + uint32_t(y) * 19349663U) % 190U); };
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        // Signed translation is converted to unsigned only for the deterministic field hash.
        const int sx = x + shift + 1024, sy = y + 1024, gx = sx / 8, gy = sy / 8;
        const float a = float(sx % 8) / 8, b = float(sy % 8) / 8;
        result[size_t(y) * width + x] = uint8_t((1-a)*(1-b)*cell(gx,gy) + a*(1-b)*cell(gx+1,gy) + (1-a)*b*cell(gx,gy+1) + a*b*cell(gx+1,gy+1));
    }
    return result;
}
std::vector<uint8_t> readback(ID3D11Device* device, ID3D11DeviceContext* context,
        ID3D10Multithread* protected_context, HANDLE mutex, const GpuFrame& frame) {
    ContextLock lock(protected_context, mutex);
    D3D11_TEXTURE2D_DESC description{}; frame.texture->GetDesc(&description);
    description.ArraySize = 1; description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0; description.MiscFlags = 0; description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    check(device->CreateTexture2D(&description, nullptr, &staging), "Create readback surface");
    context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, frame.texture.Get(), frame.array_slice, nullptr);
    std::vector<uint8_t> result(size_t(frame.width) * frame.height * 3 / 2);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read retained output");
    for (int y = 0; y < frame.height * 3 / 2; ++y)
        std::memcpy(result.data() + size_t(y) * frame.width, static_cast<uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, frame.width);
    context->Unmap(staging.Get(), 0);
    return result;
}
// Uses unrelated resources on the exact immediate context shared by FRUC.
// Per-call D3D protection cannot protect CUDA's internal map/unmap context use.
class ConcurrentRenderer {
public:
    ConcurrentRenderer(ID3D11Device* device, ID3D11DeviceContext* context,
            ID3D10Multithread* protected_context, HANDLE mutex)
        : device_(device), context_(context), protected_context_(protected_context), shared_mutex_(mutex) {
        D3D11_TEXTURE2D_DESC description{};
        description.Width = 512; description.Height = 512; description.MipLevels = 1; description.ArraySize = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM; description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT; description.BindFlags = D3D11_BIND_RENDER_TARGET;
        check(device_->CreateTexture2D(&description, nullptr, &target_), "Create competing render target");
        check(device_->CreateTexture2D(&description, nullptr, &copy_), "Create competing copy target");
        check(device_->CreateRenderTargetView(target_.Get(), nullptr, &view_), "Create competing target view");
        D3D11_BUFFER_DESC buffer{}; buffer.ByteWidth = 64; buffer.Usage = D3D11_USAGE_DEFAULT;
        check(device_->CreateBuffer(&buffer, nullptr, &upload_), "Create competing upload buffer");
        buffer.Usage = D3D11_USAGE_STAGING; buffer.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check(device_->CreateBuffer(&buffer, nullptr, &readback_), "Create competing readback buffer");
        worker_ = std::thread([this] { run(); });
    }
    ~ConcurrentRenderer() { stop(); }
    void stop() noexcept { stop_.store(true); if (worker_.joinable()) worker_.join(); }
    void rethrow_failure() {
        std::lock_guard<std::mutex> lock(error_mutex_);
        if (error_) std::rethrow_exception(error_);
    }
    uint64_t iterations() const { return iterations_.load(); }
    void stage(const char* value) { stage_.store(value); }
private:
    void run() noexcept {
        try {
            bool established_state = false;
            while (!stop_.load()) {
                {
                    ContextLock lock(protected_context_.Get(), shared_mutex_);
                    if (established_state) {
                        UINT count = 1; D3D11_VIEWPORT actual{}; context_->RSGetViewports(&count, &actual);
                        require(count == 1 && actual.TopLeftX == 11 && actual.TopLeftY == 13 && actual.Width == 123 && actual.Height == 145,
                            ("Concurrent renderer context state was corrupted; stage=" + std::string(stage_.load()) + " iteration=" + std::to_string(iterations_.load()) + " count=" + std::to_string(count) + " viewport=" + std::to_string(actual.TopLeftX) + "," + std::to_string(actual.TopLeftY) + "," + std::to_string(actual.Width) + "," + std::to_string(actual.Height)).c_str());
                    }
                    D3D11_VIEWPORT viewport{11, 13, 123, 145, 0, 1}; context_->RSSetViewports(1, &viewport);
                    established_state = true;
                    const auto tick = uint32_t(iterations_.load());
                    std::array<uint32_t, 16> expected{};
                    for (size_t i = 0; i < expected.size(); ++i) expected[i] = hash(tick + uint32_t(i));
                    context_->UpdateSubresource(upload_.Get(), 0, nullptr, expected.data(), 0, 0);
                    const float color[4] = {float(tick % 127) / 127, .25f, .75f, 1};
                    context_->ClearRenderTargetView(view_.Get(), color);
                    context_->CopyResource(copy_.Get(), target_.Get());
                    if ((tick % 128) == 0) {
                        context_->CopyResource(readback_.Get(), upload_.Get());
                        D3D11_MAPPED_SUBRESOURCE mapped{};
                        check(context_->Map(readback_.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Concurrent renderer readback");
                        const bool exact = std::memcmp(mapped.pData, expected.data(), sizeof(expected)) == 0;
                        context_->Unmap(readback_.Get(), 0);
                        require(exact, "Concurrent renderer buffer contents were corrupted");
                    }
                    context_->Flush();
                    check(device_->GetDeviceRemovedReason(), "Concurrent renderer device state");
                    ++iterations_;
                }
                std::this_thread::yield();
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(error_mutex_); error_ = std::current_exception();
            stop_.store(true);
        }
    }
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D10Multithread> protected_context_;
    HANDLE shared_mutex_;
    ComPtr<ID3D11Texture2D> target_, copy_;
    ComPtr<ID3D11RenderTargetView> view_;
    ComPtr<ID3D11Buffer> upload_, readback_;
    std::atomic<const char*> stage_{"renderer startup"};
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> iterations_{0};
    std::mutex error_mutex_;
    std::exception_ptr error_;
    std::thread worker_;
};
}
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc < 2) { std::cerr << "gpu_interop_stress <runtime> [seconds=30] [resets=100] [width=320] [height=180] [protected|mutex] [x2|native]\n"; return 2; }
        const int seconds = argc > 2 ? _wtoi(argv[2]) : 30;
        const int requested_resets = argc > 3 ? _wtoi(argv[3]) : 100;
        const int width = argc > 4 ? _wtoi(argv[4]) : 320, height = argc > 5 ? _wtoi(argv[5]) : 180;
        require(seconds > 0 && seconds <= 3600 && requested_resets >= 0 && requested_resets <= 10000, "Invalid stress duration/reset count");
        require(width >= 160 && height >= 96 && width <= 3840 && height <= 2160 && !(width & 1) && !(height & 1), "Invalid stress dimensions");
        const std::wstring lock_mode = argc > 6 ? argv[6] : L"protected";
        require(lock_mode == L"protected" || lock_mode == L"mutex", "Lock mode must be protected or mutex");
        const bool x2=argc>7;
        const bool native=argc>7&&std::wstring(argv[7])==L"native";
        const int phase_count=argc>8?_wtoi(argv[8]):x2?1:4;
        require(phase_count>=1&&phase_count<=4,"Stress phase count must be 1..4");
        const auto options=x2?nvof::GpuCompletionMode::context_ordered:nvof::GpuCompletionMode::blocking;
        MutexHandle shared_mutex;
        const HANDLE mutex = lock_mode == L"mutex" ? shared_mutex.value : nullptr;
        ComPtr<IDXGIFactory1> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create DXGI factory");
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> candidate;
            if (factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 description{}; candidate->GetDesc1(&description);
            if (description.VendorId == 0x10de) { adapter = candidate; break; }
        }
        require(bool(adapter), "NVIDIA adapter missing");
        ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        check(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
            D3D11_SDK_VERSION, &device, nullptr, &context), "Create shared D3D11 device");
        ComPtr<ID3D10Multithread> protected_context;
        check(context.As(&protected_context), "Get shared context protection");
        protected_context->SetMultithreadProtected(TRUE);
        ConcurrentRenderer renderer(device.Get(), context.Get(), protected_context.Get(), mutex);
        std::unique_ptr<GpuFrucEngine> engine;
        uint64_t pairs = 0, recreations = 0, skipped = 0; int resets = 0;
        const auto start = std::chrono::steady_clock::now();
        try {
            const auto create_engine = [&] {
                renderer.stage("engine constructor");
                std::cout << "GPU_INTEROP_STAGE constructor_begin" << std::endl;
                auto result = std::make_unique<GpuFrucEngine>(std::filesystem::path(argv[1]), device.Get(), context.Get(), mutex,options,true,!native,!native,native?nvof::GpuInterpolationBackend::native_experimental:nvof::GpuInterpolationBackend::fruc);
                renderer.stage("constructor complete");
                renderer.rethrow_failure();
                std::cout << "GPU_INTEROP_STAGE constructor_complete" << std::endl;
                return result;
            };
            engine = create_engine();
            const auto upload = [&](int shift, int64_t pts) {
                renderer.stage("source creation");
                const auto data = pixels(width, height, shift);
                D3D11_TEXTURE2D_DESC description{};
                description.Width = width; description.Height = height; description.MipLevels = 1; description.ArraySize = 1;
                description.Format = DXGI_FORMAT_NV12; description.SampleDesc.Count = 1;
                description.Usage = D3D11_USAGE_DEFAULT; description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                const D3D11_SUBRESOURCE_DATA initial{data.data(), UINT(width), UINT(data.size())};
                ComPtr<ID3D11Texture2D> texture;
                check(device->CreateTexture2D(&description, &initial, &texture), "Create moving NV12 source");
                renderer.stage("input snapshot");
                auto result = engine->copy({texture, 0, width, height, pts});
                renderer.rethrow_failure();
                return result;
            };
            auto previous = upload(0, 0);
            GpuFrame retained;
            std::vector<uint8_t> retained_bytes;
            while (resets < requested_resets || std::chrono::steady_clock::now() - start < std::chrono::seconds(seconds)) {
                renderer.rethrow_failure();
                if (resets < requested_resets) {
                    renderer.stage("history reset"); engine->reset(); ++resets;
                    if ((resets % 25) == 0) {
                        // Renderer continues issuing work during interop unregister/re-register.
                        renderer.stage("engine recreate teardown"); engine.reset(); engine = create_engine(); ++recreations;
                    }
                }
                const int64_t current_pts = int64_t(pairs + 1) * 1000000;
                auto current = upload(x2&&phase_count==1?int((pairs+1)/3)*6:int(pairs+1)*5, current_pts);
                std::vector<int64_t> times;
                for (int phase = 1; phase <= phase_count; ++phase) times.push_back(previous.pts + int64_t(phase) * 1000000/(phase_count+1));
                renderer.stage("interpolate pair");
                const auto batch = engine->interpolate_pair(previous, current, times);
                renderer.stage("interpolation complete"); renderer.rethrow_failure();
                skipped+=batch.quality.identical_warp_skipped;
                require((batch.quality.repetition_known||batch.quality.identical_warp_skipped||(native&&batch.quality.native_synthesized_mask==1)) && !batch.quality.scene_cut && batch.frames.size() == size_t(phase_count), "Interpolation failed under contention");
                if (pairs == 0) {
                    require(!batch.quality.repeated_mask, "Initial phases unexpectedly repeated");
                    retained = batch.frames.front();
                    retained_bytes = readback(device.Get(), context.Get(), protected_context.Get(), mutex, retained);
                    auto prior_phase = retained_bytes;
                    for (size_t phase = 1; phase < batch.frames.size(); ++phase) {
                        auto bytes = readback(device.Get(), context.Get(), protected_context.Get(), mutex, batch.frames[phase]);
                        require(bytes != prior_phase, "Independent phases produced identical images");
                        prior_phase = std::move(bytes);
                    }
                }
                previous = std::move(current); ++pairs;
            }
            require(retained.texture && readback(device.Get(), context.Get(), protected_context.Get(), mutex, retained) == retained_bytes,
                "Held renderer lease was overwritten during stress/reset");
            // Exercise final engine teardown while the other context user is active.
            renderer.stage("final engine teardown");
            engine.reset();
            require(readback(device.Get(), context.Get(), protected_context.Get(), mutex, retained) == retained_bytes,
                "Engine teardown corrupted a retained renderer frame");
            renderer.stop(); renderer.rethrow_failure();
            require(renderer.iterations() >= 100, "Competing renderer did not execute enough context work");
            check(device->GetDeviceRemovedReason(), "Final D3D11 device state");
        } catch (...) {
            // Join before engine/resource unwinding so a failing test cannot leave a live worker.
            renderer.stop(); engine.reset(); throw;
        }
        if(x2&&phase_count==1)require(skipped>0,"Stress did not exercise identical-picture advance");
        std::cout<<"IDENTICAL_SKIPPED "<<skipped<<std::endl;
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::cout << "GPU_INTEROP_STRESS_PASS pairs=" << pairs << " phases=" << pairs * phase_count
            << " resets=" << resets << " recreations=" << recreations << " renderer_iterations=" << renderer.iterations()
            << " seconds=" << elapsed << " lock=" << (mutex ? "mutex+protected" : "protected") << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GPU_INTEROP_STRESS_FAIL: " << error.what() << '\n'; return 1;
    }
}
