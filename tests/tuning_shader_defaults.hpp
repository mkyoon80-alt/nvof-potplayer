#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <stdexcept>
#if NVOF_EXPERIMENTAL_TUNING
#include "nvof/tuning.hpp"
#endif
// Direct shader tests bypass MotionSynthesizer, so they must supply b2 themselves.
inline void bind_tuning_test_defaults(ID3D11Device* device,ID3D11DeviceContext* context) {
#if NVOF_EXPERIMENTAL_TUNING
    D3D11_BUFFER_DESC desc{};desc.ByteWidth=sizeof(nvof::tuning::Values);
    desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA data{nvof::tuning::defaults.data(),0,0};
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    if(FAILED(device->CreateBuffer(&desc,&data,&buffer)))throw std::runtime_error("Test tuning constants");
    auto raw=buffer.Get();context->PSSetConstantBuffers(2,1,&raw);context->CSSetConstantBuffers(2,1,&raw);
#endif
}
