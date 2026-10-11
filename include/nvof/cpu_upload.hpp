#pragma once
#include "nvof/cpu_video.hpp"
#include "nvof/gpu_engine.hpp"
namespace nvof {
// Host decode -> GPU field reconstruction -> P010. No 8-bit step or readback.
class CpuUpload {
public:
    CpuUpload(ID3D11Device*,ID3D11DeviceContext*,HANDLE);
    ~CpuUpload();
    void begin(const uint8_t*,size_t,const CpuVideoLayout&,bool discontinuity);
    GpuFrame render(int parity,int64_t pts);
    // Only consult for an interlace-capable input whose sample claims WEAVE.
    // Checks moving combs on GPU; a tiny statistics readback preserves pixels.
    bool needs_field_reconstruction();
    // Same-input seeks may retain only the learned raw-field hint. Pixel
    // history is always discarded; the first new picture must confirm combs.
    void reset(bool preserve_field_hint=false) noexcept;
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
