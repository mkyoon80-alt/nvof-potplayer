#include "nvof/mf_bridge.hpp"
#include <dshow.h>
#include <d3d10_1.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <evr.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
using Microsoft::WRL::ComPtr;
using namespace nvof;
namespace {
void check(HRESULT hr,const char* action){if(FAILED(hr))throw std::runtime_error(std::string(action)+" HRESULT="+std::to_string(static_cast<unsigned long>(hr)));}
void require(bool test,const char* action){if(!test)throw std::runtime_error(action);}
MIDL_INTERFACE("BC8753F5-0AC8-4806-8E5F-A12B2AFE153E") TestTextureSample : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetD3D11Texture(int,ID3D11Texture2D**,UINT*)=0;
};
enum class Route {Native,DxgiBuffer,MediaBuffer,MediaSample,TextureOnly,NullSuccess,None};
class Sample final : public IMediaSample,public IMFGetService,public TestTextureSample {
public:
    Sample(Route route,IMFMediaBuffer* buffer,ID3D11Texture2D* texture):route_(route),buffer_(buffer),texture_(texture){
        check(MFCreateSample(&sample_),"Create test MF sample");if(buffer)check(sample_->AddBuffer(buffer),"Add test DXGI buffer");
    }
    STDMETHODIMP QueryInterface(REFIID iid,void** result) override {
        if(!result)return E_POINTER;*result=nullptr;
        if(iid==IID_IUnknown || iid==__uuidof(IMediaSample))*result=static_cast<IMediaSample*>(this);
        else if(iid==__uuidof(IMFGetService))*result=static_cast<IMFGetService*>(this);
        else if(route_==Route::Native && iid==__uuidof(TestTextureSample))*result=static_cast<TestTextureSample*>(this);
        else return E_NOINTERFACE;
        AddRef();return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override{return ++references_;}
    STDMETHODIMP_(ULONG) Release() override{const ULONG left=--references_;if(!left)delete this;return left;}
    STDMETHODIMP GetService(REFGUID service,REFIID iid,void** result) override{
        if(!result)return E_POINTER;*result=nullptr;
        if(service!=MR_BUFFER_SERVICE)return MF_E_UNSUPPORTED_SERVICE;
        if(route_==Route::NullSuccess)return S_OK; // Broken provider must not crash extraction.
        if(route_==Route::DxgiBuffer && iid==__uuidof(IMFDXGIBuffer))return buffer_->QueryInterface(iid,result);
        if(route_==Route::MediaBuffer && iid==__uuidof(IMFMediaBuffer))return buffer_->QueryInterface(iid,result);
        if(route_==Route::MediaSample && iid==__uuidof(IMFSample))return sample_->QueryInterface(iid,result);
        if(route_==Route::TextureOnly && iid==__uuidof(ID3D11Texture2D))return texture_->QueryInterface(iid,result);
        return E_NOINTERFACE;
    }
    STDMETHODIMP GetPointer(BYTE** p)override{++pointer_calls;if(p)*p=nullptr;return E_NOTIMPL;}
    STDMETHODIMP_(LONG) GetSize()override{return 0;}
    STDMETHODIMP GetTime(REFERENCE_TIME* a,REFERENCE_TIME* b)override{if(a)*a=0;if(b)*b=400000;return S_OK;}
    STDMETHODIMP SetTime(REFERENCE_TIME*,REFERENCE_TIME*)override{return E_NOTIMPL;}
    STDMETHODIMP IsSyncPoint()override{return S_OK;}
    STDMETHODIMP SetSyncPoint(BOOL)override{return E_NOTIMPL;}
    STDMETHODIMP IsPreroll()override{return S_FALSE;}
    STDMETHODIMP SetPreroll(BOOL)override{return E_NOTIMPL;}
    STDMETHODIMP_(LONG) GetActualDataLength()override{return 0;}
    STDMETHODIMP SetActualDataLength(LONG)override{return E_NOTIMPL;}
    STDMETHODIMP GetMediaType(AM_MEDIA_TYPE** p)override{if(p)*p=nullptr;return S_FALSE;}
    STDMETHODIMP SetMediaType(AM_MEDIA_TYPE*)override{return E_NOTIMPL;}
    STDMETHODIMP IsDiscontinuity()override{return S_FALSE;}
    STDMETHODIMP SetDiscontinuity(BOOL)override{return E_NOTIMPL;}
    STDMETHODIMP GetMediaTime(LONGLONG*,LONGLONG*)override{return E_NOTIMPL;}
    STDMETHODIMP SetMediaTime(LONGLONG*,LONGLONG*)override{return E_NOTIMPL;}
    STDMETHODIMP GetD3D11Texture(int,ID3D11Texture2D** texture,UINT* slice)override {
        if(!texture || !slice)return E_POINTER;
        *texture=texture_.Get();if(*texture)(*texture)->AddRef();*slice=native_slice;return S_OK;
    }
    UINT native_slice=0;
    int pointer_calls=0;
private:
    std::atomic<ULONG> references_{1};
    Route route_;
    ComPtr<IMFMediaBuffer> buffer_;
    ComPtr<IMFSample> sample_;
    ComPtr<ID3D11Texture2D> texture_;
};
ComPtr<Sample> make_sample(Route route,IMFMediaBuffer* buffer,ID3D11Texture2D* texture){ComPtr<Sample> sample;sample.Attach(new Sample(route,buffer,texture));return sample;}
}
int wmain(int argc,wchar_t** argv){
    if(argc<2){std::cerr<<"Usage: mf_bridge_smoke <runtime directory>\n";return 2;}
    const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    if(FAILED(com)){std::cerr<<"COM initialization failed\n";return 2;}
    int result=0;
    try {
        ComPtr<IMFDXGIDeviceManager> retained_manager;HANDLE retained_handle=nullptr;
        {
            MfD3d11Bridge bridge;
            retained_manager=bridge.manager();check(retained_manager->OpenDeviceHandle(&retained_handle),"Open manager device handle");
            ComPtr<ID3D11Device> managed_device;check(retained_manager->GetVideoService(retained_handle,IID_PPV_ARGS(&managed_device)),"Get manager D3D11 device");
            require(managed_device.Get()==bridge.device(),"Manager returned a different device");
            ComPtr<ID3D10Multithread> protection;check(bridge.context()->QueryInterface(IID_PPV_ARGS(&protection)),"Protected context interface");
            require(protection->GetMultithreadProtected()!=FALSE,"MF bridge context is not protected");
            require(WaitForSingleObject(bridge.mutex(),0)==WAIT_OBJECT_0,"Bridge mutex unavailable");
            require(WaitForSingleObject(bridge.mutex(),0)==WAIT_OBJECT_0,"Bridge mutex is not recursive");ReleaseMutex(bridge.mutex());ReleaseMutex(bridge.mutex());
            constexpr int w=640,h=360,coded_h=368;
            D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=coded_h;desc.MipLevels=1;desc.ArraySize=2;desc.Format=DXGI_FORMAT_NV12;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            ComPtr<ID3D11Texture2D> texture;check(bridge.device()->CreateTexture2D(&desc,nullptr,&texture),"Create test decoder texture array");
            std::vector<BYTE> pixels(size_t(w)*coded_h*3/2,128);
            std::fill(pixels.begin(),pixels.begin()+size_t(w)*coded_h,176);bridge.context()->UpdateSubresource(texture.Get(),1,nullptr,pixels.data(),w,0);
            std::fill(pixels.begin(),pixels.begin()+size_t(w)*coded_h,48);bridge.context()->UpdateSubresource(texture.Get(),0,nullptr,pixels.data(),w,0);
            ComPtr<IMFMediaBuffer> buffer;check(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D),texture.Get(),1,FALSE,&buffer),"MFCreateDXGISurfaceBuffer slice 1");
            for(auto route:{Route::DxgiBuffer,Route::MediaBuffer,Route::MediaSample}){
                auto sample=make_sample(route,buffer.Get(),texture.Get());auto frame=extract_gpu_frame(sample.Get(),w,h,100);
                require(frame.texture.Get()==texture.Get() && frame.array_slice==1 && frame.pts==100,"Public MF buffer extraction lost array index");
                require(sample->pointer_calls==0,"MF extraction tried CPU pointer access");
            }
            for(auto route:{Route::TextureOnly,Route::NullSuccess,Route::None}){
                auto sample=make_sample(route,buffer.Get(),texture.Get());bool rejected=false;
                try{extract_gpu_frame(sample.Get(),w,h,100);}catch(const std::runtime_error&){rejected=true;}
                require(rejected,"Unsupported or unindexed array sample was accepted");require(sample->pointer_calls==0,"Unsupported extraction attempted CPU access");
            }
            desc.ArraySize=1;ComPtr<ID3D11Texture2D> singleton;check(bridge.device()->CreateTexture2D(&desc,nullptr,&singleton),"Create singleton texture");
            auto single_sample=make_sample(Route::TextureOnly,nullptr,singleton.Get());require(extract_gpu_frame(single_sample.Get(),w,h,100).array_slice==0,"Singleton direct texture extraction failed");
            // Reproduce the second-PC failure before FRUC initialization.
            // Exercise the native interface used by PotPlayer as well as MF.
            {
                auto valid_native=make_sample(Route::Native,nullptr,texture.Get());valid_native->native_slice=1;
                auto extracted=extract_gpu_frame(valid_native.Get(),w,h,100);
                require(extracted.array_slice==1 && valid_native->pointer_calls==0,"Native indexed texture extraction failed");
                auto expect_rejection=[&](Sample* sample,int visible_w,int visible_h,const std::vector<std::string>& fields) {
                    std::string message;
                    try{extract_gpu_frame(sample,visible_w,visible_h,100);}catch(const std::runtime_error& error){message=error.what();}
                    for(const auto& field:fields)require(message.find(field)!=std::string::npos,"Surface rejection omitted diagnostic field");
                    require(sample->pointer_calls==0,"Invalid GPU sample attempted CPU access");
                    std::cout << "SURFACE_DIAGNOSTIC " << message << "\n";
                };
                auto outside=make_sample(Route::Native,nullptr,texture.Get());outside->native_slice=2;
                expect_rejection(outside.Get(),w,h,{"subresource-out-of-range","route=IMediaSampleD3D11","format=NV12(103)","arraySize=2","subresource=2"});
                expect_rejection(valid_native.Get(),w+2,h,{"visible-size-exceeds-texture","texture=640x368","visible=642x360"});
                for(auto format:{DXGI_FORMAT_P010,DXGI_FORMAT_B8G8R8A8_UNORM}) {
                    auto incompatible=desc;incompatible.Format=format;
                    ComPtr<ID3D11Texture2D> wrong;check(bridge.device()->CreateTexture2D(&incompatible,nullptr,&wrong),"Create incompatible decoder surface");
                    auto native_sample=make_sample(Route::Native,nullptr,wrong.Get());
                    const std::string tag=format==DXGI_FORMAT_P010?"format=P010(104)":"format=BGRA8(87)";
                    expect_rejection(native_sample.Get(),w,h,{"format-not-NV12",tag,"route=IMediaSampleD3D11","mips=1","samples=1"});
                    ComPtr<IMFMediaBuffer> wrong_buffer;check(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D),wrong.Get(),0,FALSE,&wrong_buffer),"Wrap incompatible surface");
                    auto mf_sample=make_sample(Route::DxgiBuffer,wrong_buffer.Get(),wrong.Get());
                    expect_rejection(mf_sample.Get(),w,h,{"format-not-NV12",tag,"route=IMFDXGIBuffer"});
                }
            }
            {
                GpuFrucEngine engine(std::filesystem::path(argv[1]),bridge.device(),bridge.context(),bridge.mutex());
                auto sample=make_sample(Route::DxgiBuffer,buffer.Get(),texture.Get());auto borrowed=extract_gpu_frame(sample.Get(),w,h,0);
                // Simulate a decoder retaining the nonrecursive MF manager lock
                // while synchronously delivering Receive. Engine must not relock it.
                ComPtr<ID3D11Device> locked_device;check(retained_manager->LockDevice(retained_handle,IID_PPV_ARGS(&locked_device),TRUE),"Hold decoder manager lock");
                GpuFrame original;
                try{original=engine.copy(borrowed);}catch(...){retained_manager->UnlockDevice(retained_handle,FALSE);throw;}
                check(retained_manager->UnlockDevice(retained_handle,FALSE),"Unlock decoder manager");
                auto next=engine.copy(borrowed);next.pts=400000;
                D3D11_VIEWPORT viewport{11,13,123,145,0,1};bridge.context()->RSSetViewports(1,&viewport);
                std::atomic<bool> done{false};std::atomic<unsigned> iterations{0};
                std::thread decoder([&]{while(!done){protection->Enter();bridge.context()->RSSetViewports(1,&viewport);protection->Leave();++iterations;std::this_thread::yield();}});
                try {
                    for(int i=0;i<3;++i){engine.reset();auto middle=engine.midpoint(original,next);require(middle.pts==200000,"Protected-context midpoint timestamp failed");auto blend=engine.blend(original,middle,100000);require(bool(blend.texture),"Protected-context GPU blend failed");}
                }catch(...){done=true;decoder.join();throw;}
                done=true;decoder.join();
                D3D11_VIEWPORT restored{};UINT count=1;bridge.context()->RSGetViewports(&count,&restored);
                require(count==1&&restored.TopLeftX==11&&restored.TopLeftY==13&&restored.Width==123&&restored.Height==145,"Protected operation lost decoder context state");
                std::cout<<"MF_PUBLIC_EXTRACTION routes=3 slice=1 unsupported_array_rejected=OK cpu_pointer_calls=0 protected_interop=OK concurrent_context_calls="<<iterations<<"\n";
            }
        }
        check(retained_manager->TestDevice(retained_handle),"Manager should survive bridge destruction");
        ComPtr<ID3D11Device> still_alive;check(retained_manager->GetVideoService(retained_handle,IID_PPV_ARGS(&still_alive)),"Retained manager device after bridge destruction");
        still_alive.Reset();check(retained_manager->CloseDeviceHandle(retained_handle),"Close retained handle");retained_manager.Reset();
        std::cout<<"PASS MF bridge: manager/device lifetime, safe indexed extraction, unsupported samples rejected, recursive renderer mutex, MF lock reentrancy avoided, protected CUDA/D3D11 interop, reset/shutdown\n";
    }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';result=1;}
    CoUninitialize();return result;
}
