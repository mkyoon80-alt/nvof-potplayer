#include "nvof/engine.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cuda.h>
#include <array>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace nvof {
namespace {
// The public MIT-licensed NVEncNVOFFRUC wrapper ABI, not the gated NVIDIA SDK ABI.
struct WrapperParams { void* frameIn; int64_t timestampIn; void* frameOut; int64_t timestampOut; };
using CreateFn = int (__stdcall*)(void**);
using LoadFn = int (__stdcall*)(void*);
using DeleteFn = void (__stdcall*)(void*);
using InitFn = int (__stdcall*)(void*, int, int, bool);
using RegisterFn = int (__stdcall*)(void*, void*, void*, void*);
using ProcessFn = int (__stdcall*)(void*, WrapperParams*);
void cuda_check(CUresult result, const char* operation) {
    if (result == CUDA_SUCCESS) return;
    const char* name = nullptr; const char* detail = nullptr;
    cuGetErrorName(result, &name); cuGetErrorString(result, &detail);
    throw std::runtime_error(std::string(operation) + ": " + (name ? name : "CUDA error") + " (" + (detail ? detail : "unknown") + ")");
}
void wrapper_check(int result, const char* operation) {
    if (result != 0) throw std::runtime_error(std::string(operation) + " failed with NVEncNVOFFRUC status " + std::to_string(result));
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
uint64_t signature(const Frame& f) {
    // Cache identity must include content: a seek may revisit an equal timestamp.
    uint64_t h = 1469598103934665603ULL;
    for (uint8_t byte : f.pixels) { h ^= byte; h *= 1099511628211ULL; }
    return h;
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
    HMODULE nvidia_module = nullptr;
    HMODULE wrapper_module = nullptr;
    void* wrapper = nullptr;
    CreateFn create = nullptr; LoadFn load = nullptr; DeleteFn destroy = nullptr;
    InitFn init = nullptr; RegisterFn register_resources = nullptr; ProcessFn process = nullptr;
    std::array<CUdeviceptr, 3> buffers{};
    size_t buffer_size = 0;
    int width = 0, height = 0, cached_buffer = 0;
    bool primed = false;
    int64_t cached_pts = 0, sequence_time = 0;
    uint64_t cached_signature = 0;

    explicit Impl(const std::filesystem::path& directory) {
        try {
            const auto runtime = std::filesystem::absolute(directory);
            if (!std::filesystem::is_directory(runtime)) throw std::runtime_error("Runtime directory does not exist: " + runtime.string());
            // Preload by absolute path before the wrapper's LoadLibrary("NvOFFRUC.dll").
            nvidia_module = load_module(runtime / "NvOFFRUC.dll");
            wrapper_module = load_module(runtime / "NVEncNVOFFRUC.dll");
            create = proc<CreateFn>(wrapper_module, "NVEncNVOFFRUCCreate");
            load = proc<LoadFn>(wrapper_module, "NVEncNVOFFRUCLoad");
            destroy = proc<DeleteFn>(wrapper_module, "NVEncNVOFFRUCDelete");
            init = proc<InitFn>(wrapper_module, "NVEncNVOFFRUCCreateFURCHandle");
            register_resources = proc<RegisterFn>(wrapper_module, "NVEncNVOFFRUCRegisterResource");
            process = proc<ProcessFn>(wrapper_module, "NVEncNVOFFRUCProc");
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
    void release_session() noexcept {
        // Caller holds this engine's CUDA context and mutex. Destruction is never in DllMain.
        if (context) cuCtxSynchronize();
        if (wrapper && destroy) destroy(std::exchange(wrapper, nullptr));
        primed = false; sequence_time = 0;
    }
    void release_buffers() noexcept {
        for (auto& buffer : buffers) if (buffer) { cuMemFree(buffer); buffer = 0; }
        buffer_size = 0; width = height = 0;
    }
    void cleanup() noexcept {
        if (context) {
            if (cuCtxPushCurrent(context) == CUDA_SUCCESS) {
                release_session(); release_buffers();
                CUcontext popped = nullptr; cuCtxPopCurrent(&popped);
            }
            cuCtxDestroy(context); context = nullptr;
        }
        if (wrapper_module) { FreeLibrary(wrapper_module); wrapper_module = nullptr; }
        if (nvidia_module) { FreeLibrary(nvidia_module); nvidia_module = nullptr; }
    }
    void initialize_session(const Frame& f) {
        release_session();
        const size_t bytes = frame_bytes(f);
        if (width != f.width || height != f.height || buffer_size != bytes) {
            release_buffers();
            try {
                for (auto& buffer : buffers) cuda_check(cuMemAlloc(&buffer, bytes), "cuMemAlloc packed NV12");
                width = f.width; height = f.height; buffer_size = bytes;
            } catch (...) { release_buffers(); throw; }
        }
        try {
            wrapper_check(create(&wrapper), "NVEncNVOFFRUCCreate");
            if (!wrapper) throw std::runtime_error("NVEnc wrapper returned a null handle");
            wrapper_check(load(wrapper), "NVEncNVOFFRUCLoad");
            wrapper_check(init(wrapper, width, height, true), "NVEncNVOFFRUCCreateFURCHandle");
            wrapper_check(register_resources(wrapper, &buffers[0], &buffers[1], &buffers[2]), "NVEncNVOFFRUCRegisterResource");
            cached_buffer = 0;
            cuda_check(cuMemcpyHtoD(buffers[0], f.pixels.data(), bytes), "cuMemcpyHtoD prime");
            WrapperParams params{ &buffers[0], 0, &buffers[2], 0 };
            wrapper_check(process(wrapper, &params), "NVEncNVOFFRUCProc prime");
            cuda_check(cuCtxSynchronize(), "cuCtxSynchronize prime");
            primed = true; sequence_time = 0;
            cached_pts = f.pts; cached_signature = signature(f);
        } catch (...) { release_session(); throw; }
    }
};

FrucEngine::FrucEngine(const std::filesystem::path& directory) : impl_(std::make_unique<Impl>(directory)) {}
FrucEngine::~FrucEngine() = default;
std::string FrucEngine::device_name() const { return impl_->name; }
void FrucEngine::reset() noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (cuCtxPushCurrent(impl_->context) == CUDA_SUCCESS) {
        impl_->release_session();
        CUcontext popped = nullptr; cuCtxPopCurrent(&popped);
    }
}
Frame FrucEngine::midpoint(const Frame& previous, const Frame& current) {
    const auto bytes = frame_bytes(previous);
    frame_bytes(current);
    if (previous.width != current.width || previous.height != current.height || current.pts <= previous.pts)
        throw std::invalid_argument("FRUC requires matching dimensions and increasing timestamps");
    if (previous.pts < 0 || current.pts - previous.pts > 100000000LL)
        throw std::invalid_argument("FRUC timestamps must be nonnegative with at most 10 seconds between adjacent frames");
    Frame output{previous.width, previous.height, previous.pts + (current.pts - previous.pts) / 2, std::vector<uint8_t>(bytes)};
    std::lock_guard<std::mutex> lock(impl_->mutex);
    ContextScope context(impl_->context);
    try {
        const bool cached = impl_->primed && impl_->width == previous.width && impl_->height == previous.height
            && impl_->cached_pts == previous.pts && impl_->cached_signature == signature(previous);
        if (!cached) impl_->initialize_session(previous);
        const int next_buffer = 1 - impl_->cached_buffer;
        cuda_check(cuMemcpyHtoD(impl_->buffers[next_buffer], current.pixels.data(), bytes), "cuMemcpyHtoD current");
        const int64_t next_time = impl_->sequence_time + 2;
        WrapperParams params{ &impl_->buffers[next_buffer], next_time, &impl_->buffers[2], next_time - 1 };
        wrapper_check(impl_->process(impl_->wrapper, &params), "NVEncNVOFFRUCProc midpoint");
        cuda_check(cuCtxSynchronize(), "cuCtxSynchronize midpoint");
        cuda_check(cuMemcpyDtoH(output.pixels.data(), impl_->buffers[2], bytes), "cuMemcpyDtoH midpoint");
        impl_->cached_buffer = next_buffer; impl_->sequence_time = next_time;
        impl_->cached_pts = current.pts; impl_->cached_signature = signature(current);
        return output;
    } catch (...) { impl_->release_session(); throw; }
}
}

