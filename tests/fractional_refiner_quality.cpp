#define NVOF_MULTI_PHASE_HELPERS_ONLY
#include "multi_phase_quality.cpp"
#undef NVOF_MULTI_PHASE_HELPERS_ONLY
#include "nvof/fractional_refiner.hpp"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstring>
#include <set>
using Microsoft::WRL::ComPtr;
namespace {
void hr_check(HRESULT hr,const char* what){if(FAILED(hr))throw std::runtime_error(std::string(what)+" HRESULT="+std::to_string(static_cast<unsigned long>(hr)));}
struct TestGpu {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    TestGpu() {
        ComPtr<IDXGIFactory1> factory;hr_check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI factory");ComPtr<IDXGIAdapter1> adapter;
        for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> candidate;if(factory->EnumAdapters1(i,&candidate)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 d{};candidate->GetDesc1(&d);if(d.VendorId==0x10de){adapter=candidate;break;}}
        require(bool(adapter),"NVIDIA adapter missing");
        D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
        hr_check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_VIDEO_SUPPORT|D3D11_CREATE_DEVICE_BGRA_SUPPORT,levels,2,D3D11_SDK_VERSION,&device,nullptr,&context),"D3D11 device");
    }
    ComPtr<ID3D11Texture2D> texture(const nvof::Frame* initial=nullptr) {
        D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.MipLevels=1;d.ArraySize=1;d.Format=DXGI_FORMAT_NV12;
        d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        D3D11_SUBRESOURCE_DATA data{};if(initial){data.pSysMem=initial->pixels.data();data.SysMemPitch=width;data.SysMemSlicePitch=UINT(initial->pixels.size());}
        ComPtr<ID3D11Texture2D> result;hr_check(device->CreateTexture2D(&d,initial?&data:nullptr,&result),"NV12 test texture");return result;
    }
    ComPtr<ID3D11ShaderResourceView> srv(ID3D11Texture2D* texture,bool uv) {
        D3D11_SHADER_RESOURCE_VIEW_DESC d{};d.Format=uv?DXGI_FORMAT_R8G8_UNORM:DXGI_FORMAT_R8_UNORM;d.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;d.Texture2D.MipLevels=1;
        ComPtr<ID3D11ShaderResourceView> result;hr_check(device->CreateShaderResourceView(texture,&d,&result),"NV12 test SRV");return result;
    }
    ComPtr<ID3D11RenderTargetView> rtv(ID3D11Texture2D* texture,bool uv) {
        D3D11_RENDER_TARGET_VIEW_DESC d{};d.Format=uv?DXGI_FORMAT_R8G8_UNORM:DXGI_FORMAT_R8_UNORM;d.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11RenderTargetView> result;hr_check(device->CreateRenderTargetView(texture,&d,&result),"NV12 test RTV");return result;
    }
    nvof::Frame readback(ID3D11Texture2D* texture,int64_t pts) {
        D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.MiscFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;hr_check(device->CreateTexture2D(&d,nullptr,&staging),"Test-only staging texture");
        context->CopyResource(staging.Get(),texture);D3D11_MAPPED_SUBRESOURCE mapped{};hr_check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Test-only readback");
        nvof::Frame result{width,height,pts,std::vector<uint8_t>(size_t(width)*height*3/2)};
        for(int y=0;y<height*3/2;++y)std::memcpy(result.pixels.data()+size_t(y)*width,static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch,width);
        context->Unmap(staging.Get(),0);return result;
    }
};
double edge_mse(const nvof::Frame& actual,const nvof::Frame& truth,double shift) {
    double sum=0;int count=0;
    for(int y=int(object_top())-8;y<int(object_bottom())+8;++y)for(int x=int(left(shift))-12;x<int(left(shift)+object_width())+12;++x) {
        if(x<16||x>=width-16||y<16||y>=height-16)continue;
        if(std::abs(x-left(shift))>12&&std::abs(x-left(shift)-object_width())>12&&std::abs(y-object_top())>8&&std::abs(y-object_bottom())>8)continue;
        const auto k=size_t(y)*width+x;const int d=int(actual.pixels[k])-truth.pixels[k];sum+=d*d;++count;
    }
    return count?sum/count:0;
}
struct RefinedStats {double raw=0,refined=0,raw_edge=0,refined_edge=0,max_background=0,max_position=0;int count=0,static_frames=0;};
void refiner_case(TestGpu& gpu,const std::filesystem::path& runtime,Scene scene,int speed,bool anime,int pairs,std::vector<std::string>& failures) {
    const std::string label=std::string(name(scene))+"_"+std::to_string(speed)+"px_"+(anime?"AABB":"continuous");
    nvof::FrucEngine engine(runtime);nvof::FractionalRefiner refiner(gpu.device.Get(),gpu.context.Get());
    RefinedStats stats;ComPtr<ID3D11Texture2D> held;std::vector<uint8_t> held_bytes;
    const auto shift=[=](int i){return double((anime?i/2:i)*speed);};
    auto previous=detailed_frame(scene,shift(0),0);auto previous_texture=gpu.texture(&previous);
    int midpoint_regressions=0;
    for(int pair=1;pair<=pairs;++pair) {
        auto current=detailed_frame(scene,shift(pair),int64_t(pair)*1000000);auto current_texture=gpu.texture(&current);
        const std::vector<int64_t> times={previous.pts+200000,previous.pts+400000,previous.pts+500000,previous.pts+600000,previous.pts+800000};
        auto raw=engine.interpolate_pair(previous,current,times);
        require(!raw.quality.scene_cut&&raw.frames.size()==times.size()&&raw.quality.repetition_known,"FRUC baseline did not produce real phase batch");
        refiner.prepare(previous_texture.Get(),current_texture.Get(),width,height);require(refiner.ready(),"Refiner not ready after prepare");
        for(size_t p=0;p<times.size();++p) {
            const float phase=float(times[p]-previous.pts)/1000000.f;
            const double expected=shift(pair-1)+(shift(pair)-shift(pair-1))*double(times[p]-previous.pts)/1000000.0;
            auto fallback=gpu.texture(&raw.frames[p]);auto fallback_y=gpu.srv(fallback.Get(),false),fallback_uv=gpu.srv(fallback.Get(),true);
            auto output=gpu.texture();auto output_y=gpu.rtv(output.Get(),false),output_uv=gpu.rtv(output.Get(),true);
            refiner.render(phase,output_y.Get(),output_uv.Get(),fallback_y.Get(),fallback_uv.Get());
            const auto actual=gpu.readback(output.Get(),times[p]);const auto truth=detailed_frame(scene,expected,times[p]);
            const double raw_error=mse(raw.frames[p],truth),error=mse(actual,truth);
            const double position=detailed_position(actual,scene,expected,speed);
            const double raw_edge=scene==Scene::foreground?edge_mse(raw.frames[p],truth,expected):0;
            const double refined_edge=scene==Scene::foreground?edge_mse(actual,truth,expected):0;
            const double background=scene==Scene::foreground?mse(actual,truth,true):0;
            std::cout<<"REFINE case="<<label<<" pair="<<pair<<" phase="<<phase<<" expected_x="<<expected<<" inferred_x="<<position
              <<" baseline_mse="<<raw_error<<" refined_mse="<<error<<" baseline_uv_mse="<<uv_mse(raw.frames[p],truth)<<" refined_uv_mse="<<uv_mse(actual,truth)
              <<" baseline_edge_mse="<<raw_edge<<" refined_edge_mse="<<refined_edge<<" static_background_mse="<<background
              <<" baseline_detail_rms="<<detail_rms(raw.frames[p])<<" refined_detail_rms="<<detail_rms(actual)<<" truth_detail_rms="<<detail_rms(truth)
              <<" grid="<<refiner.grid_size()<<" output_resource="<<reinterpret_cast<uintptr_t>(output.Get())<<'\n';
            if(previous.pixels==current.pixels){++stats.static_frames;if(actual.pixels!=previous.pixels)failures.push_back(label+" changed exact static NV12 pixels");}
            else if(p!=2){++stats.count;stats.raw+=raw_error;stats.refined+=error;stats.raw_edge+=raw_edge;stats.refined_edge+=refined_edge;}
            else if(error>std::max(.5,raw_error*1.25+.1))++midpoint_regressions;
            stats.max_background=std::max(stats.max_background,background);stats.max_position=std::max(stats.max_position,std::abs(position-expected));
            if(!held){held=output;held_bytes=actual.pixels;}
            else require(held.Get()!=output.Get(),"New output reused a still-held GPU resource");
        }
        require(gpu.readback(previous_texture.Get(),previous.pts).pixels==previous.pixels,"Refiner modified previous original");
        require(gpu.readback(current_texture.Get(),current.pts).pixels==current.pixels,"Refiner modified current original");
        previous=std::move(current);previous_texture=current_texture;
    }
    require(gpu.readback(held.Get(),0).pixels==held_bytes,"Later flow batches overwrote held GPU output");
    refiner.reset();require(gpu.readback(held.Get(),0).pixels==held_bytes,"Refiner reset invalidated held GPU output");
    const double raw_mean=stats.raw/std::max(1,stats.count),refined_mean=stats.refined/std::max(1,stats.count);
    std::cout<<"REFINER_RESULT case="<<label<<" phases="<<stats.count<<" exact_static_phases="<<stats.static_frames<<" baseline_mean_mse="<<raw_mean
      <<" refined_mean_mse="<<refined_mean<<" improvement="<<raw_mean/std::max(1e-12,refined_mean)<<" baseline_mean_edge_mse="<<stats.raw_edge/std::max(1,stats.count)
      <<" refined_mean_edge_mse="<<stats.refined_edge/std::max(1,stats.count)<<" max_static_background_mse="<<stats.max_background
      <<" max_position_error="<<stats.max_position<<" midpoint_regressions="<<midpoint_regressions<<" held_output_unchanged=1 originals_unchanged=1\n";
    if(scene==Scene::pan&&speed==4&&refined_mean>raw_mean/5)failures.push_back(label+" did not improve fractional pan MSE at least 5x");
    if(scene==Scene::pan&&speed==10&&refined_mean>raw_mean*1.5+.5)failures.push_back(label+" regressed integer-position pan");
    if(scene==Scene::foreground&&refined_mean>raw_mean*1.5+.5)failures.push_back(label+" substantially regressed foreground quality");
    if(scene==Scene::foreground&&stats.refined_edge>stats.raw_edge*2+stats.count*4)failures.push_back(label+" substantially regressed occlusion edges");
    if(midpoint_regressions)failures.push_back(label+" regressed midpoint quality");
}
void chroma_only_case(TestGpu& gpu,const std::filesystem::path& runtime) {
    nvof::Frame a{width,height,0,std::vector<uint8_t>(size_t(width)*height*3/2,96)},b=a;b.pts=1000000;
    for(int y=0;y<height/2;++y)for(int x=0;x<width;x+=2) {
        const auto k=size_t(width)*height+size_t(y)*width+x;const bool stripe=(x/4)&1;
        a.pixels[k]=stripe?176:80;a.pixels[k+1]=stripe?144:112;
        b.pixels[k]=stripe?80:176;b.pixels[k+1]=stripe?112:144;
    }
    nvof::FrucEngine engine(runtime);auto batch=engine.interpolate_pair(a,b,{200000,400000,600000,800000});
    require(batch.frames.size()==4&&!batch.quality.scene_cut,"Missing chroma-only FRUC fallback");
    nvof::FractionalRefiner refiner(gpu.device.Get(),gpu.context.Get());auto ga=gpu.texture(&a),gb=gpu.texture(&b);
    refiner.prepare(ga.Get(),gb.Get(),width,height);
    for(size_t p=0;p<batch.frames.size();++p) {
        auto fallback=gpu.texture(&batch.frames[p]);auto fy=gpu.srv(fallback.Get(),false),fuv=gpu.srv(fallback.Get(),true);
        auto output=gpu.texture();auto oy=gpu.rtv(output.Get(),false),ouv=gpu.rtv(output.Get(),true);
        refiner.render(float(p+1)/5,oy.Get(),ouv.Get(),fy.Get(),fuv.Get());auto actual=gpu.readback(output.Get(),batch.frames[p].pts);
        std::cout<<"CHROMA_ONLY phase="<<float(p+1)/5<<" fallback_uv_difference="<<uv_mse(actual,batch.frames[p])<<" fallback_luma_difference="<<mse(actual,batch.frames[p])<<'\n';
        require(actual.pixels==batch.frames[p].pixels,"Ambiguous chroma-only motion changed fallback instead of rejecting zero-luma flow");
    }
    require(gpu.readback(ga.Get(),0).pixels==a.pixels&&gpu.readback(gb.Get(),0).pixels==b.pixels,"Chroma-only refiner changed originals");
}}
#ifndef NVOF_REFINER_HELPERS_ONLY
int wmain(int argc,wchar_t** argv) {
    try {
        if(argc<2){std::cerr<<"fractional_refiner_quality <runtime> [pairs=4] [scene=all,pan,foreground] [speed=0,4,10] [cadence=all,continuous,AABB]\n";return 2;}
        const int pairs=argc>2?std::stoi(argv[2]):4;const std::wstring wanted_scene=argc>3?argv[3]:L"all";
        const int wanted_speed=argc>4?std::stoi(argv[4]):0;const std::wstring wanted_cadence=argc>5?argv[5]:L"all";
        require(pairs>=2&&pairs<=24,"Use 2 to 24 source pairs");require(wanted_scene==L"all"||wanted_scene==L"pan"||wanted_scene==L"foreground","Invalid scene");
        require(wanted_speed==0||wanted_speed==4||wanted_speed==10,"Invalid speed");require(wanted_cadence==L"all"||wanted_cadence==L"continuous"||wanted_cadence==L"AABB","Invalid cadence");
        TestGpu gpu;std::vector<std::string> failures;std::cout<<std::fixed<<std::setprecision(6);
        for(Scene scene:{Scene::pan,Scene::foreground})for(int speed:{4,10})for(bool anime:{false,true}) {
            if(wanted_scene!=L"all"&&wanted_scene!=(scene==Scene::pan?L"pan":L"foreground"))continue;
            if(wanted_speed&&wanted_speed!=speed)continue;if(wanted_cadence!=L"all"&&wanted_cadence!=(anime?L"AABB":L"continuous"))continue;
            refiner_case(gpu,std::filesystem::path(argv[1]),scene,speed,anime,pairs,failures);
        }
        if(wanted_scene==L"all")chroma_only_case(gpu,std::filesystem::path(argv[1]));
        for(const auto& failure:failures)std::cout<<"QUALITY_FAILURE "<<failure<<'\n';
        require(failures.empty(),"Fractional refiner quality regression; see per-case measurements");
        std::cout<<"PASS fractional-position quality, integer/midpoint stability, occlusion guards, original preservation, held GPU output lifetime\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}



#endif
