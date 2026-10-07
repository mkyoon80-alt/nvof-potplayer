#pragma once
#include "nvof/gpu_engine.hpp"
#include <mfobjects.h>
#include <climits>
#include <memory>
struct IMediaSample;
namespace nvof {
// Construct/release on ordinary DirectShow host threads, outside DllMain and
// Media Foundation work queues. Returned COM pointers are borrowed; AddRef to
// retain them. The forwarding manager keeps its MF startup lease while retained.
class MfD3d11Bridge {
public:
    explicit MfD3d11Bridge(UINT adapter_index = UINT_MAX);
    ~MfD3d11Bridge();
    MfD3d11Bridge(const MfD3d11Bridge&) = delete;
    MfD3d11Bridge& operator=(const MfD3d11Bridge&) = delete;
    IMFDXGIDeviceManager* manager() const noexcept;
    ID3D11Device* device() const noexcept;
    ID3D11DeviceContext* context() const noexcept;
    HANDLE mutex() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// No GetPointer, Lock2D or host copy. The caller must retain the original sample
// until GpuFrucEngine::copy completes; a texture AddRef does not reserve a decoder
// array slice. Unindexed texture arrays and unsupported formats fail explicitly.
// P010 is opt-in: the caller must normalize supported SDR to NV12 on GPU.
GpuFrame extract_gpu_frame(IMediaSample* sample, int width, int height, int64_t pts, bool allow_p010 = false);
}
