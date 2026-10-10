// System-memory transport uses the same native D3D11 synthesizer as GPU playback.
// The historical FrucEngine class name is retained for source compatibility only.
#include "nvof/engine.hpp"
#include "nvof/gpu_engine.hpp"
#include <dxgi1_2.h>
#include <mutex>
#include <stdexcept>
#include <cstring>
namespace nvof {
using Microsoft::WRL::ComPtr;
namespace {
void check(HRESULT hr,const char* action){if(FAILED(hr))throw std::runtime_error(std::string(action)+" HRESULT="+std::to_string(static_cast<unsigned long>(hr)));}
void validate(const Frame& f){
    if(f.width<2||f.height<2||f.width>8192||f.height>8192||(f.width&1)||(f.height&1)||
       f.pixels.size()!=size_t(f.width)*f.height*3/2)throw std::invalid_argument("Expected tightly packed even-size NV12");
}
}
struct FrucEngine::Impl {
    std::mutex mutex;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> readback;
    std::unique_ptr<GpuFrucEngine> gpu;
    GpuFrame cached;
    Frame cached_cpu;
    explicit Impl(const std::filesystem::path& directory){
        ComPtr<IDXGIFactory1> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Create DXGI factory");
        for(UINT i=0;;++i){
            ComPtr<IDXGIAdapter1> adapter;
            const HRESULT next=factory->EnumAdapters1(i,&adapter);
            if(next==DXGI_ERROR_NOT_FOUND)break;
            check(next,"Enumerate adapter");
            DXGI_ADAPTER_DESC1 desc{};check(adapter->GetDesc1(&desc),"Describe adapter");
            if(desc.VendorId!=0x10de || (desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE))continue;
            const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
            D3D_FEATURE_LEVEL actual{};
            const HRESULT created=D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,levels,2,D3D11_SDK_VERSION,&device,&actual,&context);
            if(SUCCEEDED(created))break;
            device.Reset();context.Reset();
        }
        if(!device)throw std::runtime_error("A supported NVIDIA D3D11 device is required");
        gpu=std::make_unique<GpuFrucEngine>(directory,device.Get(),context.Get(),nullptr,
            GpuCompletionMode::blocking,true,false,false,GpuInterpolationBackend::native_experimental);
    }
    GpuFrame upload(const Frame& f){
        D3D11_TEXTURE2D_DESC desc{};desc.Width=f.width;desc.Height=f.height;desc.MipLevels=1;
        desc.ArraySize=1;desc.Format=DXGI_FORMAT_NV12;desc.SampleDesc.Count=1;
        desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{};data.pSysMem=f.pixels.data();data.SysMemPitch=f.width;
        ComPtr<ID3D11Texture2D> texture;check(device->CreateTexture2D(&desc,&data,&texture),"Upload NV12");
        return {texture,0,f.width,f.height,f.pts,{}};
    }
    Frame download(const GpuFrame& f){
        D3D11_TEXTURE2D_DESC desc{};f.texture->GetDesc(&desc);
        D3D11_TEXTURE2D_DESC existing{};if(readback)readback->GetDesc(&existing);
        if(!readback||existing.Width!=desc.Width||existing.Height!=desc.Height){
            desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            desc.MiscFlags=0;readback.Reset();check(device->CreateTexture2D(&desc,nullptr,&readback),"Create NV12 readback");
        }
        context->CopyResource(readback.Get(),f.texture.Get());
        Frame result{f.width,f.height,f.pts,std::vector<uint8_t>(size_t(f.width)*f.height*3/2)};
        D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped),"Read NV12");
        for(int row=0;row<f.height*3/2;++row)
            std::memcpy(result.pixels.data()+size_t(row)*f.width,static_cast<const uint8_t*>(mapped.pData)+size_t(row)*mapped.RowPitch,f.width);
        context->Unmap(readback.Get(),0);
        return result;
    }
};
FrucEngine::FrucEngine(const std::filesystem::path& directory):impl_(std::make_unique<Impl>(directory)){}
FrucEngine::~FrucEngine()=default;
std::string FrucEngine::device_name()const{return impl_->gpu->device_name();}
void FrucEngine::reset()noexcept{
    std::lock_guard<std::mutex> lock(impl_->mutex);impl_->gpu->reset();impl_->cached={};impl_->cached_cpu={};
}
PhaseBatch<Frame> FrucEngine::interpolate_pair(const Frame& a,const Frame& b,const std::vector<int64_t>& times){
    std::lock_guard<std::mutex> lock(impl_->mutex);validate(a);validate(b);
    if(a.width!=b.width||a.height!=b.height||a.pts<0||b.pts<=a.pts||b.pts-a.pts>100000000LL||times.size()>32)
        throw std::invalid_argument("Expected matching adjacent NV12 frames");
    auto last=a.pts;for(auto t:times){if(t<=last||t>=b.pts)throw std::invalid_argument("Invalid phase timestamp");last=t;}
    PhaseBatch<Frame> result;
    if(times.empty())return result;
    try {
        const bool reuse=impl_->cached.texture&&impl_->cached_cpu.width==a.width&&impl_->cached_cpu.height==a.height&&
            impl_->cached_cpu.pts==a.pts&&impl_->cached_cpu.pixels==a.pixels;
        auto first=reuse?impl_->cached:impl_->upload(a);
        auto second=impl_->upload(b);
        auto batch=impl_->gpu->interpolate_pair(first,second,times);
        result.quality=batch.quality;result.frames.reserve(batch.frames.size());
        for(const auto& f:batch.frames)result.frames.push_back(impl_->download(f));
        impl_->cached=std::move(second);impl_->cached_cpu=b;
        return result;
    }catch(...){impl_->gpu->reset();impl_->cached={};impl_->cached_cpu={};throw;}
}
Frame FrucEngine::midpoint(const Frame& a,const Frame& b){
    const auto t=a.pts+(b.pts-a.pts)/2;auto batch=interpolate_pair(a,b,{t});
    if(batch.quality.scene_cut||batch.quality.repeated_mask){auto f=a;f.pts=t;return f;}
    return std::move(batch.frames.at(0));
}
}
