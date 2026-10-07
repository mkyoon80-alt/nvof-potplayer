#pragma once
#include <cstdint>

// Public adapter ABI. NVIDIA's SDK declarations remain private to the bridge.
#if defined(NVOF_FRUC_BRIDGE_BUILD)
#define NVOF_FRUC_BRIDGE_API __declspec(dllexport)
#else
#define NVOF_FRUC_BRIDGE_API __declspec(dllimport)
#endif

struct NVEncNVOFFRUCParams {
    void* frameIn;
    int64_t timestampIn;
    void* frameOut;
    int64_t timestampOut;
};

enum : uint32_t {
    NVOF_FRUC_REPETITION_KNOWN = 1,
    NVOF_FRUC_FRAME_REPEATED = 2,
    NVOF_FRUC_WARP_SKIPPED = 4,
};

struct NVEncNVOFFRUCResult {
    uint32_t size = 16;
    uint32_t version = 1;
    uint32_t flags = 0;
    uint32_t reserved = 0;
};
static_assert(sizeof(NVEncNVOFFRUCResult) == 16, "Bridge result ABI");

// Resources are addresses of CUdeviceptr values for CudaResourceCuDevicePtr.
// Keep addresses/allocations valid until CloseFURCHandle/Delete. Serialize calls
// to one handle, with its CUDA context current through teardown.
extern "C" {
NVOF_FRUC_BRIDGE_API int __stdcall NVEncNVOFFRUCCreate(void** handle);
NVOF_FRUC_BRIDGE_API int __stdcall NVEncNVOFFRUCLoad(void* handle);
NVOF_FRUC_BRIDGE_API void __stdcall NVEncNVOFFRUCDelete(void* handle);
NVOF_FRUC_BRIDGE_API int __stdcall NVEncNVOFFRUCCreateFURCHandle(void* handle, int width, int height, bool nv12);
NVOF_FRUC_BRIDGE_API int __stdcall NVEncNVOFFRUCRegisterResource(void* handle, void* first, void* second, void* output);
NVOF_FRUC_BRIDGE_API int __stdcall NVEncNVOFFRUCCloseFURCHandle(void* handle);
NVOF_FRUC_BRIDGE_API int __stdcall NVEncNVOFFRUCProc(void* handle, NVEncNVOFFRUCParams* params);
NVOF_FRUC_BRIDGE_API int __stdcall NVEncNVOFFRUCProcEx(void* handle, NVEncNVOFFRUCParams* params, NVEncNVOFFRUCResult* result);
// Advance one new input timestamp without producing a warped output.
NVOF_FRUC_BRIDGE_API int __stdcall NVEncNVOFFRUCAdvance(void* handle, NVEncNVOFFRUCParams* params, NVEncNVOFFRUCResult* result);
}
