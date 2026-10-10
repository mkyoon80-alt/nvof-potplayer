// Hardware-only regression: compare actual raw OF vectors before any repair.
#define wmain original_gpu_smoke_entry
#include "gpu_engine_smoke.cpp"
#undef wmain
#include <d3d10_1.h>
namespace {
GpuFrame upload(ID3D11Device* device,const std::vector<uint8_t>& bytes,int w,int h,int64_t pts) {
    D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=1;
    d.Format=DXGI_FORMAT_NV12;d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{bytes.data(),UINT(w),UINT(bytes.size())};
    ComPtr<ID3D11Texture2D> t;check(device->CreateTexture2D(&d,&initial,&t),"Upload fixture");
    return {t,0,w,h,pts};
}
void drain(ID3D11Device* device,ID3D11DeviceContext* context) {
    D3D11_QUERY_DESC q{D3D11_QUERY_EVENT,0};ComPtr<ID3D11Query> query;
    check(device->CreateQuery(&q,&query),"Benchmark completion");context->End(query.Get());context->Flush();
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    for(;;){auto hr=context->GetData(query.Get(),nullptr,0,0);check(hr,"GPU completion");if(hr==S_OK)break;
        require(std::chrono::steady_clock::now()<deadline,"GPU completion timed out");SwitchToThread();}
}
}
// This test builds the implementation locally to inspect private GPU resources.
// The static linker does not pull the same implementation from the library.
#define NVOF_SESSION_DIAGNOSTICS 1
#include "../src/motion_synthesizer.cpp"
namespace nvof {
struct MotionSessionTestAccess {
    static std::vector<int16_t> raw(MotionSynthesizer& synth) {
        auto& s=*synth.impl_;std::vector<int16_t> result;
        for(auto& texture:s.flows) {
            D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
            auto width=desc.Width,height=desc.Height;desc.Usage=D3D11_USAGE_STAGING;
            desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> stage;check(s.device->CreateTexture2D(&desc,nullptr,&stage),"Raw staging");
            s.context->CopyResource(stage.Get(),texture.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
            check(s.context->Map(stage.Get(),0,D3D11_MAP_READ,0,&mapped),"Raw flow readback");
            for(unsigned y=0;y<height;++y){auto row=reinterpret_cast<const int16_t*>(static_cast<const uint8_t*>(mapped.pData)+y*mapped.RowPitch);result.insert(result.end(),row,row+width*2);}
            s.context->Unmap(stage.Get(),0);
        }
        return result;
    }
    static auto times(MotionSynthesizer& synth){return synth.impl_->timings;}
    static auto sessions(MotionSynthesizer& synth){return synth.impl_->sessionCreations;}
    static auto surfaces(MotionSynthesizer& synth){return synth.impl_->surfaceSets;}
};
}
int wmain(int argc,wchar_t** argv){try{
    int mode=argc>1?std::stoi(argv[1]):1,count=argc>2?std::stoi(argv[2]):64;
    require(mode>=0&&mode<=1&&count>=1&&count<=300,"Invalid trial");
    ComPtr<IDXGIFactory1> factory;nvof::check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI");ComPtr<IDXGIAdapter1> adapter;
    for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> a;if(factory->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND)break;
        DXGI_ADAPTER_DESC1 d{};a->GetDesc1(&d);if(d.VendorId==0x10de){adapter=a;break;}}
    require(bool(adapter),"NVIDIA missing");ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
    nvof::check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_VIDEO_SUPPORT|D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels,2,D3D11_SDK_VERSION,&device,nullptr,&context),"D3D11");
    MotionFlowOptions options;options.session_mode=static_cast<MotionSessionMode>(mode);
    MotionFlowOptions isolated;isolated.session_mode=MotionSessionMode::fresh;
    MotionSynthesizer baseline(device.Get(),context.Get(),1920,MotionCostMode::disabled,isolated),candidate(device.Get(),context.Get(),1920,MotionCostMode::disabled,options);
    const int w=960,h=540;
    auto vertical=[&](double shift){auto horizontal=moving_color(h,w,shift,false);std::vector<uint8_t> image(size_t(w)*h*3/2);
        for(int y=0;y<h;++y)for(int x=0;x<w;++x)image[size_t(y)*w+x]=horizontal[size_t(x)*h+y];
        for(int y=0;y<h/2;++y)for(int x=0;x<w/2;++x)for(int c=0;c<2;++c)image[size_t(w)*h+size_t(y)*w+x*2+c]=horizontal[size_t(w)*h+size_t(x)*h+y*2+c];return image;};
    auto output=upload(device.Get(),vertical(0),w,h,0);
    D3D11_TEXTURE2D_DESC od{};output.texture->GetDesc(&od);od.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
    output.texture.Reset();nvof::check(device->CreateTexture2D(&od,nullptr,&output.texture),"Output");
    ComPtr<ID3D11RenderTargetView> rt[2];for(int p=0;p<2;++p){D3D11_RENDER_TARGET_VIEW_DESC rd{};rd.Format=p?DXGI_FORMAT_R8G8_UNORM:DXGI_FORMAT_R8_UNORM;rd.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;nvof::check(device->CreateRenderTargetView(output.texture.Get(),&rd,&rt[p]),"RTV");}
    // Run the complete baseline first: interleaving a fresh session between
    // every persistent execute can hide driver/resource lifetime regressions.
    std::vector<std::vector<int16_t>> referenceRaw;
    std::vector<std::vector<uint8_t>> referencePixels;
    std::vector<bool> referenceReliable;
    double shift=0,worst=0,worstBase=0;uint64_t rawDifferent=0,rawTotal=0,pixelDifferent=0,badPairs=0;
    for(int pass=0;pass<2;++pass) {
        auto& synth=pass?candidate:baseline;shift=0;
        for(int i=0;i<count;++i){double step=8+(i%9)*.5;if(i>=64)step=-step;
            if(i>=80&&i%4==0){synth.invalidate_history();shift=-i*3;}
            auto a=upload(device.Get(),vertical(shift),w,h,0),b=upload(device.Get(),vertical(shift+step),w,h,166666);
            synth.prepare(a.texture.Get(),b.texture.Get(),w,h);
            // Capture after rendering: a test-only raw readback must not supply
            // an extra synchronization boundary before normal synthesis.
            synth.render_midpoint(rt[0].Get(),rt[1].Get());auto actual=readback(device.Get(),context.Get(),output);
            const auto raw=nvof::MotionSessionTestAccess::raw(synth);
            auto truth=vertical(shift+step*.5);double error=mse(actual,truth,w,h);
            if(!pass){referenceRaw.push_back(raw);referencePixels.push_back(actual);referenceReliable.push_back(synth.reliable());worstBase=std::max(worstBase,error);}
            else {
                uint64_t differences=0;for(size_t j=0;j<raw.size();++j)differences+=raw[j]!=referenceRaw[i][j];
                rawDifferent+=differences;rawTotal+=raw.size();pixelDifferent+=actual!=referencePixels[i];worst=std::max(worst,error);
                if(error>=8||synth.reliable()!=referenceReliable[i])++badPairs;
                if(differences||error>=8)std::cout<<"PAIR "<<i<<" raw_diff="<<differences<<" mse="<<error<<std::endl;
            }
            shift+=step;
        }
    }
    std::cout<<"QUALITY mode="<<mode<<" pairs="<<count<<" raw_diff="<<rawDifferent<<"/"<<rawTotal<<" output_different="<<pixelDifferent<<" worst_mse="<<worst<<" baseline="<<worstBase<<" bad_pairs="<<badPairs<<std::endl;
    auto a=upload(device.Get(),vertical(0),w,h,0),b=upload(device.Get(),vertical(10),w,h,166666);
    for(auto* s:{&baseline,&candidate}){s->invalidate_history();std::array<double,6> sums{};double total=0;
        for(int i=0;i<36;++i){auto start=std::chrono::steady_clock::now();s->prepare(a.texture.Get(),b.texture.Get(),w,h);s->render_midpoint(rt[0].Get(),rt[1].Get());drain(device.Get(),context.Get());
            if(i>=6){total+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();auto t=nvof::MotionSessionTestAccess::times(*s);for(int j=0;j<6;++j)sums[j]+=t[j];}}
        std::cout<<"TIMING "<<(s==&baseline?"fresh":"candidate")<<" total="<<total/30<<" stages=";for(auto t:sums)std::cout<<t/30<<",";
        std::cout<<" sessions="<<nvof::MotionSessionTestAccess::sessions(*s)<<" surfaces="<<nvof::MotionSessionTestAccess::surfaces(*s)<<std::endl;
    }
    require(badPairs==0&&worstBase<8&&rawDifferent==0&&pixelDifferent==0,"Session reuse changed baseline motion/pixels");std::cout<<"PASS"<<std::endl;return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
