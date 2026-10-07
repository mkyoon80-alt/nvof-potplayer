#include "nvof/mf_bridge.hpp"
#include <dshow.h>
#include <streams.h>
#include <d3d10_1.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <evr.h>
#include <atomic>
#include <stdexcept>
#include <string>
#include <utility>
namespace nvof {
namespace {
using Microsoft::WRL::ComPtr;
void check(HRESULT hr,const char* action) {
    if(FAILED(hr))throw std::runtime_error(std::string(action)+" HRESULT="+std::to_string(static_cast<unsigned long>(hr)));
}
// Public LAV/MPC sample ABI, repeated privately to avoid a baseclass dependency.
MIDL_INTERFACE("BC8753F5-0AC8-4806-8E5F-A12B2AFE153E") PublicTextureSample : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetD3D11Texture(int,ID3D11Texture2D**,UINT*)=0;
};
// The decoder can retain its manager beyond the bridge object's own lifetime.
// Couple the successful MFStartup reference to that final COM manager Release,
// and release the real manager before balancing it with MFShutdown.
class LeasedManager final : public IMFDXGIDeviceManager, private CBaseObject {
public:
    explicit LeasedManager(ID3D11Device* device) : CBaseObject(NAME("MF DXGI manager lifetime lease")) {
        check(MFStartup(MF_VERSION,MFSTARTUP_LITE),"MFStartup");
        started_=true;
        try {
            UINT token=0;
            check(MFCreateDXGIDeviceManager(&token,&inner_),"MFCreateDXGIDeviceManager");
            check(inner_->ResetDevice(device,token),"IMFDXGIDeviceManager::ResetDevice");
        } catch(...) {inner_.Reset();MFShutdown();started_=false;throw;}
    }
    ~LeasedManager() {inner_.Reset();if(started_)MFShutdown();}
    STDMETHODIMP QueryInterface(REFIID iid,void** result) override {
        if(!result)return E_POINTER;*result=nullptr;
        if(iid!=IID_IUnknown && iid!=__uuidof(IMFDXGIDeviceManager))return E_NOINTERFACE;
        *result=static_cast<IMFDXGIDeviceManager*>(this);AddRef();return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override {return ++references_;}
    STDMETHODIMP_(ULONG) Release() override {const ULONG remaining=--references_;if(!remaining)delete this;return remaining;}
    STDMETHODIMP CloseDeviceHandle(HANDLE h) override {return inner_->CloseDeviceHandle(h);}
    STDMETHODIMP GetVideoService(HANDLE h,REFIID iid,void** value) override {return inner_->GetVideoService(h,iid,value);}
    STDMETHODIMP LockDevice(HANDLE h,REFIID iid,void** value,BOOL block) override {return inner_->LockDevice(h,iid,value,block);}
    STDMETHODIMP OpenDeviceHandle(HANDLE* h) override {return inner_->OpenDeviceHandle(h);}
    STDMETHODIMP ResetDevice(IUnknown* d,UINT token) override {return inner_->ResetDevice(d,token);}
    STDMETHODIMP TestDevice(HANDLE h) override {return inner_->TestDevice(h);}
    STDMETHODIMP UnlockDevice(HANDLE h,BOOL state) override {return inner_->UnlockDevice(h,state);}
private:
    std::atomic<ULONG> references_{1};
    bool started_=false;
    ComPtr<IMFDXGIDeviceManager> inner_;
};
GpuFrame validated(ComPtr<ID3D11Texture2D> texture,UINT subresource,int width,int height,int64_t pts) {
    if(!texture || width<2 || height<2 || (width&1) || (height&1))
        throw std::runtime_error("GPU sample has no texture or invalid visible dimensions");
    D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
    // IMFDXGIBuffer reports the full subresource index, not just an array slice.
    // Our NV12 engine supports a single mip, so the two are identical only here.
    if(desc.Format!=DXGI_FORMAT_NV12 || desc.MipLevels!=1 || desc.SampleDesc.Count!=1 ||
       UINT(width)>desc.Width || UINT(height)>desc.Height || subresource>=desc.ArraySize)
        throw std::runtime_error("GPU sample NV12 format, mip count, dimensions or subresource is unsupported");
    return {std::move(texture),subresource,width,height,pts};
}
GpuFrame from_buffer(IMFDXGIBuffer* buffer,int width,int height,int64_t pts) {
    if(!buffer)throw std::runtime_error("GPU buffer service returned a null interface");
    ComPtr<ID3D11Texture2D> texture;UINT subresource=0;
    check(buffer->GetResource(IID_PPV_ARGS(&texture)),"IMFDXGIBuffer::GetResource");
    check(buffer->GetSubresourceIndex(&subresource),"IMFDXGIBuffer::GetSubresourceIndex");
    return validated(std::move(texture),subresource,width,height,pts);
}
bool media_sample_buffer(IMFSample* sample,ComPtr<IMFDXGIBuffer>& found) {
    DWORD count=0;check(sample->GetBufferCount(&count),"IMFSample::GetBufferCount");
    // Current engine is one progressive view in one NV12 buffer. Do not guess
    // how multiple media buffers represent planes, fields or stereo views.
    if(count!=1)return false;
    ComPtr<IMFMediaBuffer> buffer;check(sample->GetBufferByIndex(0,&buffer),"IMFSample::GetBufferByIndex");
    return buffer && buffer.As(&found)==S_OK && found;
}
}
struct MfD3d11Bridge::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IMFDXGIDeviceManager> manager;
    HANDLE mutex=nullptr;
    explicit Impl(UINT requested) {
        try {
            ComPtr<IDXGIFactory1> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"CreateDXGIFactory1");
            ComPtr<IDXGIAdapter1> adapter;
            if(requested!=UINT_MAX)check(factory->EnumAdapters1(requested,&adapter),"Requested DXGI adapter");
            else {
                for(UINT i=0;;++i) {
                    ComPtr<IDXGIAdapter1> candidate;const HRESULT hr=factory->EnumAdapters1(i,&candidate);
                    if(hr==DXGI_ERROR_NOT_FOUND)break;check(hr,"Enumerate DXGI adapter");
                    DXGI_ADAPTER_DESC1 desc{};check(candidate->GetDesc1(&desc),"DXGI adapter description");
                    if(desc.VendorId==0x10de && !(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)) {
                        if(!adapter)adapter=candidate;
                        if(wcsstr(desc.Description,L"5090")){adapter=candidate;break;}
                    }
                }
            }
            if(!adapter)throw std::runtime_error("No NVIDIA D3D11 hardware adapter");
            DXGI_ADAPTER_DESC1 desc{};check(adapter->GetDesc1(&desc),"Selected DXGI adapter");
            if(desc.VendorId!=0x10de || (desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE))
                throw std::runtime_error("Requested adapter is not NVIDIA hardware");
            const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
            check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,
                D3D11_CREATE_DEVICE_VIDEO_SUPPORT|D3D11_CREATE_DEVICE_BGRA_SUPPORT,levels,2,D3D11_SDK_VERSION,
                &device,nullptr,&context),"Create MF D3D11 device");
            ComPtr<ID3D10Multithread> multithread;check(context.As(&multithread),"Query D3D11 multithread protection");
            multithread->SetMultithreadProtected(TRUE);
            if(!multithread->GetMultithreadProtected())throw std::runtime_error("D3D11 multithread protection was not enabled");
            mutex=CreateMutexW(nullptr,FALSE,nullptr);
            if(!mutex)check(HRESULT_FROM_WIN32(GetLastError()),"Create D3D11 renderer mutex");
            manager.Attach(new LeasedManager(device.Get()));
        }catch(...){if(mutex)CloseHandle(std::exchange(mutex,nullptr));throw;}
    }
    ~Impl(){manager.Reset();context.Reset();device.Reset();if(mutex)CloseHandle(mutex);}
};
MfD3d11Bridge::MfD3d11Bridge(UINT adapter):impl_(std::make_unique<Impl>(adapter)){}
MfD3d11Bridge::~MfD3d11Bridge()=default;
IMFDXGIDeviceManager* MfD3d11Bridge::manager()const noexcept{return impl_->manager.Get();}
ID3D11Device* MfD3d11Bridge::device()const noexcept{return impl_->device.Get();}
ID3D11DeviceContext* MfD3d11Bridge::context()const noexcept{return impl_->context.Get();}
HANDLE MfD3d11Bridge::mutex()const noexcept{return impl_->mutex;}
GpuFrame extract_gpu_frame(IMediaSample* sample,int width,int height,int64_t pts) {
    if(!sample)throw std::invalid_argument("A retained DirectShow GPU sample is required");
    ComPtr<PublicTextureSample> native;
    if(sample->QueryInterface(IID_PPV_ARGS(&native))==S_OK && native) {
        ComPtr<ID3D11Texture2D> texture;UINT slice=0;
        check(native->GetD3D11Texture(0,&texture,&slice),"IMediaSampleD3D11::GetD3D11Texture");
        return validated(std::move(texture),slice,width,height,pts);
    }
    ComPtr<IMFDXGIBuffer> dxgi;
    if(sample->QueryInterface(IID_PPV_ARGS(&dxgi))==S_OK && dxgi)return from_buffer(dxgi.Get(),width,height,pts);
    ComPtr<IMFSample> mf_sample;
    if(sample->QueryInterface(IID_PPV_ARGS(&mf_sample))==S_OK && mf_sample && media_sample_buffer(mf_sample.Get(),dxgi))
        return from_buffer(dxgi.Get(),width,height,pts);
    ComPtr<IMFGetService> services;
    if(sample->QueryInterface(IID_PPV_ARGS(&services))==S_OK && services) {
        if(services->GetService(MR_BUFFER_SERVICE,IID_PPV_ARGS(&dxgi))==S_OK && dxgi)
            return from_buffer(dxgi.Get(),width,height,pts);
        mf_sample.Reset();
        if(services->GetService(MR_BUFFER_SERVICE,IID_PPV_ARGS(&mf_sample))==S_OK && mf_sample && media_sample_buffer(mf_sample.Get(),dxgi))
            return from_buffer(dxgi.Get(),width,height,pts);
        ComPtr<IMFMediaBuffer> media_buffer;
        if(services->GetService(MR_BUFFER_SERVICE,IID_PPV_ARGS(&media_buffer))==S_OK && media_buffer && media_buffer.As(&dxgi)==S_OK && dxgi)
            return from_buffer(dxgi.Get(),width,height,pts);
        ComPtr<ID3D11Texture2D> texture;
        if(services->GetService(MR_BUFFER_SERVICE,IID_PPV_ARGS(&texture))==S_OK && texture) {
            D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
            if(desc.ArraySize!=1)throw std::runtime_error("GPU texture array has no public subresource index; refusing to guess the decoder slice");
            return validated(std::move(texture),0,width,height,pts);
        }
    }
    throw std::runtime_error("Decoder GPU sample exposes no supported public D3D11 texture/subresource interface");
}
}
