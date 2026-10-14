#pragma once
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <stdexcept>
// Test-only CPU-released fence. The production filter uses no D3D12 device.
class GpuCompletionGate {
    Microsoft::WRL::ComPtr<ID3D12Device> device12_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence12_;
    Microsoft::WRL::ComPtr<ID3D11Fence> fence11_;
    static void checked(HRESULT hr){if(FAILED(hr))throw std::runtime_error("GPU gate setup failed");}
public:
    explicit GpuCompletionGate(ID3D11Device* device) {
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;checked(device->QueryInterface(IID_PPV_ARGS(&dxgi)));
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;checked(dxgi->GetAdapter(&adapter));
        checked(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device12_)));
        checked(device12_->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&fence12_)));
        Microsoft::WRL::ComPtr<ID3D11Device5> device5;checked(device->QueryInterface(IID_PPV_ARGS(&device5)));
        HANDLE shared=nullptr;checked(device12_->CreateSharedHandle(fence12_.Get(),nullptr,GENERIC_ALL,nullptr,&shared));
        const HRESULT opened=device5->OpenSharedFence(shared,IID_PPV_ARGS(&fence11_));CloseHandle(shared);checked(opened);
    }
    void arm(ID3D11DeviceContext* context) {
        Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context4;checked(context->QueryInterface(IID_PPV_ARGS(&context4)));
        checked(context4->Wait(fence11_.Get(),1));context->Flush();
    }
    void release() noexcept {if(fence12_)fence12_->Signal(1);}
    ~GpuCompletionGate(){release();}
};
