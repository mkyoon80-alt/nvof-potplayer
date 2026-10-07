#include "nvof/engine.hpp"
#include "nvof/scene_cut.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cuda.h>
#include <array>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace nvof {
namespace {
// The public MIT-licensed NVEnc wrapper ABI, extended by our owned bridge.
struct WrapperParams { void* frameIn; int64_t timestampIn; void* frameOut; int64_t timestampOut; };
struct ProcessResult { uint32_t size = 16, version = 1, flags = 0, reserved = 0; };
static_assert(sizeof(ProcessResult) == 16);
using CreateFn = int (__stdcall*)(void**);
using LoadFn = int (__stdcall*)(void*);
using DeleteFn = void (__stdcall*)(void*);
using InitFn = int (__stdcall*)(void*, int, int, bool);
using RegisterFn = int (__stdcall*)(void*, void*, void*, void*);
using ProcessExFn = int (__stdcall*)(void*, WrapperParams*, ProcessResult*);
void cuda_check(CUresult result, const char* operation) {
    if (result == CUDA_SUCCESS) return;
    const char* name = nullptr; const char* detail = nullptr;
    cuGetErrorName(result, &name); cuGetErrorString(result, &detail);
    throw std::runtime_error(std::string(operation) + ": " + (name ? name : "CUDA error") + " (" + (detail ? detail : "unknown") + ")");
}
void wrapper_check(int result, const char* operation) {
    if (result != 0) throw std::runtime_error(std::string(operation) + " failed with FRUC bridge status " + std::to_string(result));
}
struct ContextScope {
    explicit ContextScope(CUcontext context) { cuda_check(cuCtxPushCurrent(context), "cuCtxPushCurrent"); }
    ~ContextScope() { CUcontext popped = nullptr; cuCtxPopCurrent(&popped); }
};
size_t frame_bytes(const Frame& f) {
    if (f.width <= 0 || f.height <= 0 || (f.width & 1) || (f.height & 1) || f.width > 8192 || f.height > 8192)
        throw std::invalid_argument("FRUC requires even NV12 dimensions, at most 8192 x 8192");
    const size_t count = size_t(f.width) * size_t(f.height) * 3 / 2;
    if (f.pixels.size() != count) throw std::invalid_argument("NV12 frame must be tightly packed with stride equal to width");
    return count;
}
HMODULE load_module(const std::filesystem::path& path) {
    HMODULE result = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!result) throw std::runtime_error("Cannot load " + path.filename().string() + "; Windows error " + std::to_string(GetLastError()));
    return result;
}
template<class T> T proc(HMODULE module, const char* name) {
    const auto result = reinterpret_cast<T>(GetProcAddress(module, name));
    if (!result) throw std::runtime_error(std::string("Missing runtime export: ") + name);
    return result;
}
}

struct FrucEngine::Impl {
    std::mutex mutex;
    CUcontext context = nullptr;
    CUdevice device = 0;
    std::string name;
    HMODULE nvidia_module = nullptr, wrapper_module = nullptr;
    struct Session { void* wrapper = nullptr; };
    std::vector<Session> sessions;
    CreateFn create = nullptr; LoadFn load = nullptr; DeleteFn destroy = nullptr;
    InitFn init = nullptr; RegisterFn register_resources = nullptr; ProcessExFn process_ex = nullptr;
    // Calls share the GPU originals and output sequentially. Registered pointer
    // identities are owned separately for each generation of FRUC histories.
    std::array<CUdeviceptr, 3> buffers{};
    std::unique_ptr<std::array<CUdeviceptr, 3>> resources;
    size_t buffer_size = 0;
    int width = 0, height = 0, cached_buffer = 0;
    bool primed = false;
    int64_t cached_pts = 0, sequence_time = 0;
    std::vector<uint8_t> cached_pixels;

    explicit Impl(const std::filesystem::path& directory) {
        try {
            const auto runtime = std::filesystem::absolute(directory);
            if (!std::filesystem::is_directory(runtime)) throw std::runtime_error("Runtime directory does not exist: " + runtime.string());
            nvidia_module = load_module(runtime / "NvOFFRUC.dll");
            wrapper_module = load_module(runtime / "NvofFrucBridge.dll");
            create = proc<CreateFn>(wrapper_module, "NVEncNVOFFRUCCreate");
            load = proc<LoadFn>(wrapper_module, "NVEncNVOFFRUCLoad");
            destroy = proc<DeleteFn>(wrapper_module, "NVEncNVOFFRUCDelete");
            init = proc<InitFn>(wrapper_module, "NVEncNVOFFRUCCreateFURCHandle");
            register_resources = proc<RegisterFn>(wrapper_module, "NVEncNVOFFRUCRegisterResource");
            process_ex = proc<ProcessExFn>(wrapper_module, "NVEncNVOFFRUCProcEx");
            cuda_check(cuInit(0), "cuInit");
            int count = 0; cuda_check(cuDeviceGetCount(&count), "cuDeviceGetCount");
            if (count == 0) throw std::runtime_error("No NVIDIA CUDA device is available");
            cuda_check(cuDeviceGet(&device, 0), "cuDeviceGet");
            char label[256]{}; cuda_check(cuDeviceGetName(label, sizeof(label), device), "cuDeviceGetName"); name = label;
            cuda_check(cuCtxCreate(&context, CU_CTX_SCHED_BLOCKING_SYNC, device), "cuCtxCreate");
            CUcontext popped = nullptr; cuda_check(cuCtxPopCurrent(&popped), "cuCtxPopCurrent after create");
        } catch (...) { cleanup(); throw; }
    }
    ~Impl() { cleanup(); }
    void release_sessions() noexcept {
        // Caller holds this engine's CUDA context and mutex. Never in DllMain.
        if (context) cuCtxSynchronize();
        for (auto& session : sessions) if (session.wrapper && destroy) destroy(std::exchange(session.wrapper, nullptr));
        sessions.clear(); primed = false; sequence_time = cached_pts = 0;
        cached_pixels.clear();
    }
    void release_buffers() noexcept {
        for (auto& buffer : buffers) if (buffer) { cuMemFree(buffer); buffer = 0; }
        buffer_size = 0; width = height = 0;
    }
    void cleanup() noexcept {
        if (context) {
            if (cuCtxPushCurrent(context) == CUDA_SUCCESS) {
                release_sessions(); release_buffers();
                CUcontext popped = nullptr; cuCtxPopCurrent(&popped);
            }
            cuCtxDestroy(context); context = nullptr;
        }
        if (wrapper_module) { FreeLibrary(wrapper_module); wrapper_module = nullptr; }
        if (nvidia_module) { FreeLibrary(nvidia_module); nvidia_module = nullptr; }
    }
    void configure(const Frame& f, size_t bytes) {
        if (width == f.width && height == f.height && buffer_size == bytes) return;
        release_sessions(); release_buffers();
        try {
            for (auto& buffer : buffers) cuda_check(cuMemAlloc(&buffer, bytes), "cuMemAlloc packed NV12");
            width = f.width; height = f.height; buffer_size = bytes;
        } catch (...) { release_buffers(); throw; }
    }
    void prime(const Frame& previous) {
        release_sessions();
        cuda_check(cuMemcpyHtoD(buffers[0], previous.pixels.data(), buffer_size), "Upload previous original");
        // Allocate before retiring the old registration addresses. Reusing those
        // identities after teardown can make NVIDIA repeat the first new phase.
        resources = std::make_unique<std::array<CUdeviceptr, 3>>(buffers);
        cached_buffer = 0; cached_pts = previous.pts; cached_pixels = previous.pixels; primed = true;
    }
    void ensure_session(size_t index) {
        while (sessions.size() <= index) {
            sessions.push_back({}); auto& session = sessions.back();
            wrapper_check(create(&session.wrapper), "Create FRUC phase");
            if (!session.wrapper) throw std::runtime_error("FRUC bridge returned a null phase handle");
            wrapper_check(load(session.wrapper), "Load FRUC phase");
            wrapper_check(init(session.wrapper, width, height, true), "Initialize FRUC phase");
            wrapper_check(register_resources(session.wrapper, &(*resources)[0], &(*resources)[1], &(*resources)[2]), "Register shared FRUC phase buffers");
            WrapperParams params{&(*resources)[cached_buffer], sequence_time, &(*resources)[2], sequence_time}; ProcessResult ignored{};
            wrapper_check(process_ex(session.wrapper, &params, &ignored), "Prime FRUC phase");
        }
    }
};

FrucEngine::FrucEngine(const std::filesystem::path& directory) : impl_(std::make_unique<Impl>(directory)) {}
FrucEngine::~FrucEngine() = default;
std::string FrucEngine::device_name() const { return impl_->name; }
void FrucEngine::reset() noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (cuCtxPushCurrent(impl_->context) == CUDA_SUCCESS) {
        impl_->release_sessions();
        CUcontext popped = nullptr; cuCtxPopCurrent(&popped);
    }
}
Frame FrucEngine::midpoint(const Frame& previous, const Frame& current) {
    if (previous.pts < 0 || current.pts <= previous.pts) {
        reset();
        throw std::invalid_argument("FRUC requires nonnegative increasing timestamps");
    }
    const auto pts = previous.pts + (current.pts - previous.pts) / 2;
    auto batch = interpolate_pair(previous, current, {pts});
    if (batch.quality.scene_cut || batch.quality.repeated_mask) { auto held = previous; held.pts = pts; return held; }
    return std::move(batch.frames.at(0));
}
PhaseBatch<Frame> FrucEngine::interpolate_pair(const Frame& previous, const Frame& current,
                                             const std::vector<int64_t>& timestamps) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    ContextScope context(impl_->context);
    try {
        const auto bytes = frame_bytes(previous); frame_bytes(current);
        if (previous.width != current.width || previous.height != current.height || previous.pts < 0 || current.pts <= previous.pts || current.pts - previous.pts > 100000000LL)
            throw std::invalid_argument("FRUC requires matching adjacent frames, nonnegative increasing timestamps, and at most a 10 second interval");
        if (timestamps.size() > 32) throw std::invalid_argument("More than 32 motion phases per source pair are unsupported");
        int64_t last = previous.pts;
        for (const auto pts : timestamps) {
            if (pts <= last || pts >= current.pts) throw std::invalid_argument("Motion timestamps must increase inside the source pair");
            last = pts;
        }
        PhaseBatch<Frame> result{}; result.frames.reserve(timestamps.size());
        if (classify_scene(scene_stats_cpu(previous.pixels.data(), current.pixels.data(), previous.width, previous.height)).cut) {
            result.quality.scene_cut = true; impl_->release_sessions(); return result;
        }
        impl_->configure(previous, bytes);
        const int64_t duration = current.pts - previous.pts;
        const bool identical = previous.pixels == current.pixels;
        const bool cached = impl_->primed && impl_->cached_pts == previous.pts && impl_->cached_pixels == previous.pixels;
        // Relative times retain 100 ns precision even after a seek near INT64_MAX.
        // Reset very long histories before any bridge double timestamp can round.
        if (!cached || impl_->sequence_time > (int64_t(1) << 52) - duration) impl_->prime(previous);
        // There is no integral interior timestamp for a one-tick pair. No output
        // was requested, so retain B as the next prime without invalid FRUC calls.
        if (duration == 1) { impl_->prime(current); return result; }
        for (size_t i = 0; i < timestamps.size(); ++i) impl_->ensure_session(i);
        const int next = 1 - impl_->cached_buffer;
        cuda_check(cuMemcpyHtoD(impl_->buffers[next], current.pixels.data(), bytes), "Upload current original once");
        const int64_t next_time = impl_->sequence_time + duration;
        result.quality.repetition_known = !timestamps.empty();
        // Each history sees each original exactly once, including histories not
        // requested on this pair. Their midpoint result is deliberately discarded.
        for (size_t i = 0; i < impl_->sessions.size(); ++i) {
            const int64_t elapsed = i < timestamps.size() ? timestamps[i] - previous.pts : duration / 2;
            WrapperParams params{&(*impl_->resources)[next], next_time, &(*impl_->resources)[2], impl_->sequence_time + elapsed}; ProcessResult quality{};
            wrapper_check(impl_->process_ex(impl_->sessions[i].wrapper, &params, &quality), "Generate FRUC motion phase");
            if (!(quality.flags & 1)) throw std::runtime_error("FRUC repetition status unavailable");
            if (i < timestamps.size()) {
                if (quality.flags & 2) result.quality.repeated_mask |= uint32_t(1) << i;
                // Exact CPU identity guarantees a static scene. Preserve all Y/UV
                // bytes while still advancing FRUC and reporting its real metadata.
                Frame output{previous.width, previous.height, timestamps[i], identical ? previous.pixels : std::vector<uint8_t>(bytes)};
                // Blocking device-to-host copy completes before reusing buffer 2.
                if (!identical) cuda_check(cuMemcpyDtoH(output.pixels.data(), impl_->buffers[2], bytes), "Read FRUC phase NV12");
                result.frames.push_back(std::move(output));
            }
        }
        impl_->cached_buffer = next; impl_->sequence_time = next_time;
        impl_->cached_pts = current.pts; impl_->cached_pixels = current.pixels;
        return result;
    } catch (...) { impl_->release_sessions(); throw; }
}
}
