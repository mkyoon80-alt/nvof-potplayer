// GPU regression for a broad, defocused foreground crossing a textured background.
// A correspondence failure must be reported and held exactly; ordinary motion
// immediately after it must resume interpolation, including after reset.
#define wmain original_gpu_smoke_entry
#include "gpu_engine_smoke.cpp"
#undef wmain
#include <d3d10_1.h>
namespace {
GpuFrame uploadGuard(ID3D11Device* device,const std::vector<uint8_t>& bytes,int w,int h,int64_t pts) {
    D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=1;
    d.Format=DXGI_FORMAT_NV12;d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{bytes.data(),UINT(w),UINT(bytes.size())};ComPtr<ID3D11Texture2D> t;
    check(device->CreateTexture2D(&d,&initial,&t),"Upload guard fixture");return {t,0,w,h,pts};
}
}
int wmain(int argc,wchar_t** argv){try{
    require(argc==2,"native_motion_guard <runtime>");
    ComPtr<IDXGIFactory1> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI");ComPtr<IDXGIAdapter1> adapter;
    for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> a;if(factory->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND)break;
        DXGI_ADAPTER_DESC1 d{};a->GetDesc1(&d);if(d.VendorId==0x10de){adapter=a;break;}}
    require(bool(adapter),"NVIDIA missing");ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
    check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_VIDEO_SUPPORT|D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels,2,D3D11_SDK_VERSION,&device,nullptr,&context),"D3D11");
    ComPtr<ID3D10Multithread> protection;check(context.As(&protection),"Context protection");protection->SetMultithreadProtected(TRUE);
    GpuFrucEngine engine(argv[1],device.Get(),context.Get(),nullptr,GpuCompletionMode::context_ordered,true,false,false,GpuInterpolationBackend::native_experimental);
    const int w=960,h=540;
    auto blurred=[&](double center) {
        auto image=moving_color(w,h,0,false);
        for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
            double alpha=std::exp(-std::pow((x-center)/90.0,2)*.5);
            double value=60+35*std::sin(y*.008)+12*std::cos((x-center)*.01);
            auto& v=image[size_t(y)*w+x];v=uint8_t(std::lround(v*(1-alpha)+value*alpha));
        }
        return image;
    };
    for(int trial=0;trial<2;++trial) {
        if(trial)engine.reset();
        auto a=blurred(340),b=blurred(510);
        auto first=engine.copy(uploadGuard(device.Get(),a,w,h,0));
        auto next=engine.copy(uploadGuard(device.Get(),b,w,h,400000));
        auto result=engine.interpolate_pair(first,next,{100000,200000,300000});
        std::cout<<"BLUR_GUARD trial="<<trial<<" repeat="<<result.quality.repeated_mask<<" cut="<<result.quality.scene_cut<<std::endl;
        require(!result.quality.scene_cut&&result.frames.size()==3,"Local motion failure treated as global cut");
        require(result.quality.repeated_mask==7&&result.quality.native_synthesized_mask==0,"Severe ambiguous motion was not protected");
        for(size_t i=0;i<result.frames.size();++i){require(readback(device.Get(),context.Get(),result.frames[i])==a,"Held source pixels changed");
            require(result.frames[i].pts==int64_t(i+1)*100000,"Held phase clock changed");}
        auto cleanA=moving_color(w,h,0,false),cleanB=moving_color(w,h,12,false),truth=moving_color(w,h,6,false);
        first=engine.copy(uploadGuard(device.Get(),cleanA,w,h,800000));next=engine.copy(uploadGuard(device.Get(),cleanB,w,h,1200000));
        result=engine.interpolate_pair(first,next,{1000000});
        require(result.frames.size()==1&&result.quality.repeated_mask==0&&result.quality.native_synthesized_mask==1,"Protection stuck on subsequent ordinary motion");
        require(mse(readback(device.Get(),context.Get(),result.frames[0]),truth,w,h)<2.0,"Recovered pan has wrong midpoint");
    }
    std::cout<<"PASS ambiguous blur, exact held pixels and phase times, recovery and reset"<<std::endl;return 0;
}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
