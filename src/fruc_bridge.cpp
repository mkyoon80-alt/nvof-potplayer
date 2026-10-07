#define NVOF_FRUC_BRIDGE_BUILD
#include "nvof/fruc_bridge.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <array>
#include <cwchar>
#include <new>
#include <NvOFFRUC.h>

namespace {
// Legacy NVEnc wrapper error numbers; proprietary declarations stay private.
constexpr int null_pointer = -2;
constexpr int allocation_failure = -4;
constexpr int not_initialized = -8;
constexpr int invalid_parameter = -24;
int status(NvOFFRUC_STATUS value) noexcept {
    switch (value) {
    case NvOFFRUC_SUCCESS: return 0;
    case NvOFFRUC_ERR_NvOFFRUC_NOT_SUPPORTED: return -50001;
    case NvOFFRUC_ERR_INVALID_PTR: return -50002;
    case NvOFFRUC_ERR_INVALID_PARAM: return -50003;
    case NvOFFRUC_ERR_INVALID_HANDLE: return -50004;
    case NvOFFRUC_ERR_OUT_OF_SYSTEM_MEMORY: return -50005;
    case NvOFFRUC_ERR_OUT_OF_VIDEO_MEMORY: return -50006;
    case NvOFFRUC_ERR_OPENCV_NOT_AVAILABLE: return -50007;
    case NvOFFRUC_ERR_UNIMPLEMENTED: return -50008;
    case NvOFFRUC_ERR_OF_FAILURE: return -50009;
    case NvOFFRUC_ERR_DUPLICATE_RESOURCE: return -50010;
    case NvOFFRUC_ERR_UNREGISTERED_RESOURCE: return -50011;
    case NvOFFRUC_ERR_INCORRECT_API_SEQUENCE: return -50012;
    case NvOFFRUC_ERR_WRITE_TODISK_FAILED: return -50013;
    case NvOFFRUC_ERR_PIPELINE_EXECUTION_FAILURE: return -50014;
    case NvOFFRUC_ERR_SYNC_WRITE_FAILED: return -50015;
    case NvOFFRUC_ERR_GENERIC: return -50016;
    default: return -1;
    }
}

class Bridge {
public:
    ~Bridge() { close(); if (module_) FreeLibrary(module_); }
    int load() noexcept {
        if (module_) return 0;
        // Resolve beside the adapter, never in the player's current directory.
        HMODULE self = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&NVEncNVOFFRUCCreate), &self)) return null_pointer;
        std::array<wchar_t, 32768> path{};
        const DWORD length = GetModuleFileNameW(self, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length >= path.size()) return null_pointer;
        auto slash = wcsrchr(path.data(), L'\\');
        if (!slash || wcscpy_s(slash + 1, path.size() - (slash + 1 - path.data()), L"NvOFFRUC.dll")) return null_pointer;
        module_ = LoadLibraryExW(path.data(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module_) return null_pointer;
        create_ = reinterpret_cast<PtrToFuncNvOFFRUCCreate>(GetProcAddress(module_, CreateProcName));
        register_ = reinterpret_cast<PtrToFuncNvOFFRUCRegisterResource>(GetProcAddress(module_, RegisterResourceProcName));
        unregister_ = reinterpret_cast<PtrToFuncNvOFFRUCUnregisterResource>(GetProcAddress(module_, UnregisterResourceProcName));
        process_ = reinterpret_cast<PtrToFuncNvOFFRUCProcess>(GetProcAddress(module_, ProcessProcName));
        destroy_ = reinterpret_cast<PtrToFuncNvOFFRUCDestroy>(GetProcAddress(module_, DestroyProcName));
        if (create_ && register_ && unregister_ && process_ && destroy_) return 0;
        FreeLibrary(module_); module_ = nullptr;
        create_ = nullptr; register_ = nullptr; unregister_ = nullptr; process_ = nullptr; destroy_ = nullptr;
        return null_pointer;
    }

    int create(int width, int height, bool nv12) noexcept {
        if (!module_) return not_initialized;
        if (width <= 0 || height <= 0 || (nv12 && ((width & 1) || (height & 1)))) return invalid_parameter;
        const int closed = close();
        if (closed) return closed;
        NvOFFRUC_CREATE_PARAM parameters{};
        parameters.uiWidth = width;
        parameters.uiHeight = height;
        parameters.pDevice = nullptr;
        parameters.eResourceType = CudaResource;
        parameters.eSurfaceFormat = nv12 ? NV12Surface : ARGBSurface;
        parameters.eCUDAResourceType = CudaResourceCuDevicePtr;
        return status(create_(&parameters, &handle_));
    }

    int register_resources(void* first, void* second, void* output) noexcept {
        if (!handle_) return not_initialized;
        if (!first || !second || !output) return null_pointer;
        if (resources_[0]) return -50012;
        if (first == second || first == output || second == output) return invalid_parameter;
        NvOFFRUC_REGISTER_RESOURCE_PARAM parameters{};
        parameters.uiCount = 3;
        parameters.pArrResource[0] = first;
        parameters.pArrResource[1] = second;
        parameters.pArrResource[2] = output;
        const int error = status(register_(handle_, &parameters));
        if (!error) resources_ = {first, second, output};
        return error;
    }

    int close() noexcept {
        int error = 0;
        if (handle_) {
            if (resources_[0]) {
                NvOFFRUC_UNREGISTER_RESOURCE_PARAM parameters{};
                parameters.uiCount = 3;
                for (size_t i = 0; i < resources_.size(); ++i) parameters.pArrResource[i] = resources_[i];
                error = status(unregister_(handle_, &parameters));
            }
            const int destroyed = status(destroy_(handle_));
            if (!error) error = destroyed;
        }
        handle_ = nullptr; resources_ = {}; have_timestamp_ = false;
        return error;
    }

    int process(const NVEncNVOFFRUCParams* parameters, NVEncNVOFFRUCResult* result, bool skip_warp = false) noexcept {
        if (!parameters || !parameters->frameIn || !parameters->frameOut) return null_pointer;
        if (!handle_ || !resources_[0]) return not_initialized;
        const int64_t origin = have_timestamp_ ? first_timestamp_ : parameters->timestampIn;
        // Unsigned subtraction avoids signed overflow and preserves small
        // relative intervals even for large absolute input timestamps.
        const auto relative = [origin](int64_t timestamp) {
            return timestamp >= origin ? static_cast<double>(uint64_t(timestamp) - uint64_t(origin))
                                       : -static_cast<double>(uint64_t(origin) - uint64_t(timestamp));
        };
        bool repeated = false;
        NvOFFRUC_PROCESS_IN_PARAMS input{};
        NvOFFRUC_PROCESS_OUT_PARAMS output{};
        input.bSkipWarp = skip_warp;
        input.stFrameDataInput.pFrame = parameters->frameIn;
        input.stFrameDataInput.nTimeStamp = relative(parameters->timestampIn);
        output.stFrameDataOutput.pFrame = parameters->frameOut;
        output.stFrameDataOutput.nTimeStamp = relative(parameters->timestampOut);
        output.stFrameDataOutput.bHasFrameRepetitionOccurred = &repeated;
        const int error = status(process_(handle_, &input, &output));
        if (error) return error;
        first_timestamp_ = origin; have_timestamp_ = true;
        if (result) result->flags = skip_warp ? NVOF_FRUC_WARP_SKIPPED :
            NVOF_FRUC_REPETITION_KNOWN | (repeated ? NVOF_FRUC_FRAME_REPEATED : 0);
        return 0;
    }

private:
    HMODULE module_ = nullptr;
    PtrToFuncNvOFFRUCCreate create_ = nullptr;
    PtrToFuncNvOFFRUCRegisterResource register_ = nullptr;
    PtrToFuncNvOFFRUCUnregisterResource unregister_ = nullptr;
    PtrToFuncNvOFFRUCProcess process_ = nullptr;
    PtrToFuncNvOFFRUCDestroy destroy_ = nullptr;
    NvOFFRUCHandle handle_ = nullptr;
    std::array<void*, 3> resources_{};
    int64_t first_timestamp_ = 0;
    bool have_timestamp_ = false;
};
}

extern "C" {
int __stdcall NVEncNVOFFRUCCreate(void** handle) {
    if (!handle) return null_pointer;
    *handle = new (std::nothrow) Bridge;
    return *handle ? 0 : allocation_failure;
}
int __stdcall NVEncNVOFFRUCLoad(void* handle) { return handle ? static_cast<Bridge*>(handle)->load() : null_pointer; }
void __stdcall NVEncNVOFFRUCDelete(void* handle) { delete static_cast<Bridge*>(handle); }
int __stdcall NVEncNVOFFRUCCreateFURCHandle(void* handle, int width, int height, bool nv12) {
    return handle ? static_cast<Bridge*>(handle)->create(width, height, nv12) : null_pointer;
}
int __stdcall NVEncNVOFFRUCRegisterResource(void* handle, void* first, void* second, void* output) {
    return handle ? static_cast<Bridge*>(handle)->register_resources(first, second, output) : null_pointer;
}
int __stdcall NVEncNVOFFRUCCloseFURCHandle(void* handle) { return handle ? static_cast<Bridge*>(handle)->close() : null_pointer; }
int __stdcall NVEncNVOFFRUCProc(void* handle, NVEncNVOFFRUCParams* parameters) {
    return handle ? static_cast<Bridge*>(handle)->process(parameters, nullptr) : null_pointer;
}
int __stdcall NVEncNVOFFRUCProcEx(void* handle, NVEncNVOFFRUCParams* parameters, NVEncNVOFFRUCResult* result) {
    if (!result) return null_pointer;
    if (result->size < sizeof(*result) || result->version != 1) return invalid_parameter;
    result->flags = 0; result->reserved = 0;
    return handle ? static_cast<Bridge*>(handle)->process(parameters, result) : null_pointer;
}
int __stdcall NVEncNVOFFRUCAdvance(void* handle, NVEncNVOFFRUCParams* parameters, NVEncNVOFFRUCResult* result) {
    if (!result) return null_pointer;
    if (result->size < sizeof(*result) || result->version != 1) return invalid_parameter;
    result->flags = 0; result->reserved = 0;
    return handle ? static_cast<Bridge*>(handle)->process(parameters, result, true) : null_pointer;
}

}
