#include "nvof/gpu_engine.hpp"
#include "nvof/gpu_pipeline.hpp"
#include <dxgi1_2.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
using namespace nvof;
namespace {
void check(HRESULT hr,const char* what){if(FAILED(hr))throw std::runtime_error(std::string(what)+" HRESULT="+std::to_string(static_cast<unsigned long>(hr)));}
void require(bool v,const char* what){if(!v)throw std::runtime_error(what);}
uint32_t hash(uint32_t v){v^=v>>16;v*=0x7feb352d;v^=v>>15;v*=0x846ca68b;return v^(v>>16);}
float cell(int x,int y){return 32.f+float(hash(uint32_t(x)*733U+uint32_t(y)*19349663U)%190U);}
std::vector<uint8_t> pixels(int w,int h,int shift){std::vector<uint8_t> data(size_t(w)*h*3/2,128);for(int y=0;y<h;++y)for(int x=0;x<w;++x){const int sx=x-shift+1024,sy=y+1024,gx=sx/8,gy=sy/8;const float a=float(sx%8)/8,b=float(sy%8)/8;data[size_t(y)*w+x]=uint8_t((1-a)*(1-b)*cell(gx,gy)+a*(1-b)*cell(gx+1,gy)+(1-a)*b*cell(gx,gy+1)+a*b*cell(gx+1,gy+1));}return data;}
std::vector<uint8_t> readback(ID3D11Device* device,ID3D11DeviceContext* context,const GpuFrame& frame){
    D3D11_TEXTURE2D_DESC desc{};frame.texture->GetDesc(&desc);desc.ArraySize=1;desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;check(device->CreateTexture2D(&desc,nullptr,&staging),"Create test-only staging texture");context->CopySubresourceRegion(staging.Get(),0,0,0,0,frame.texture.Get(),frame.array_slice,nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Test-only readback Map");std::vector<uint8_t> result(size_t(frame.width)*frame.height*3/2);
    for(int y=0;y<frame.height*3/2;++y)memcpy(result.data()+size_t(y)*frame.width,static_cast<uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch,frame.width);
    context->Unmap(staging.Get(),0);return result;
}
double mse(const std::vector<uint8_t>&a,const std::vector<uint8_t>&b,int w,int h){double sum=0;int count=0;for(int y=32;y<h-32;++y)for(int x=64;x<w-64;++x){const double delta=double(a[size_t(y)*w+x])-b[size_t(y)*w+x];sum+=delta*delta;++count;}return sum/count;}
// Independent BT.709 limited-range oracle. Broad constant patches keep
// chroma siting/filtering at boundaries out of this matrix/channel-order check.
std::vector<uint8_t> color_bars(int w,int h){
    constexpr std::array<std::array<int,3>,8> rgb={{{0,0,0},{255,255,255},
        {176,64,48},{48,176,64},{64,48,176},{48,160,176},{176,48,160},{176,160,48}}};
    std::array<std::array<uint8_t,3>,8> yuv{};
    for(size_t i=0;i<rgb.size();++i){
        const double r=rgb[i][0]/255.0,g=rgb[i][1]/255.0,b=rgb[i][2]/255.0;
        const double y=.2126*r+.7152*g+.0722*b;
        yuv[i]={uint8_t(std::lround(16+219*y)),
            uint8_t(std::lround(128+224*(b-y)/1.8556)),
            uint8_t(std::lround(128+224*(r-y)/1.5748))};
    }
    const int bar_width=(w/8)&~1;require(bar_width>=16,"Color oracle requires width >= 128");
    std::vector<uint8_t> data(size_t(w)*h*3/2);
    for(int y=0;y<h;++y)for(int x=0;x<w;++x)data[size_t(y)*w+x]=yuv[(std::min)(7,x/bar_width)][0];
    for(int y=0;y<h/2;++y)for(int x=0;x<w;x+=2){
        const auto& patch=yuv[(std::min)(7,x/bar_width)];
        data[size_t(w)*h+size_t(y)*w+x]=patch[1];data[size_t(w)*h+size_t(y)*w+x+1]=patch[2];
    }
    return data;
}
std::array<int,3> color_error(const std::vector<uint8_t>& actual,const std::vector<uint8_t>& expected,int w,int h){
    std::array<int,3> maximum{};const int bar_width=(w/8)&~1;
    for(int bar=0;bar<8;++bar){
        const int begin=bar*bar_width+4,end=(bar==7?w:(bar+1)*bar_width)-4;
        for(int y=4;y<h-4;++y)for(int x=begin;x<end;++x)
            maximum[0]=(std::max)(maximum[0],std::abs(int(actual[size_t(y)*w+x])-expected[size_t(y)*w+x]));
        for(int y=2;y<h/2-2;++y)for(int x=begin;x<end;x+=2)for(int channel=0;channel<2;++channel){
            const size_t at=size_t(w)*h+size_t(y)*w+x+channel;
            maximum[channel+1]=(std::max)(maximum[channel+1],std::abs(int(actual[at])-expected[at]));
        }
    }
    return maximum;
}
std::vector<uint8_t> gray_ramp(int w,int h,bool legal){
    std::vector<uint8_t> data(size_t(w)*h*3/2,128);
    for(int y=0;y<h;++y)for(int x=0;x<w;++x)
        data[size_t(y)*w+x]=uint8_t((legal?16:0)+(legal?219:255)*x/(w-1));
    return data;
}
std::vector<uint8_t> chroma_detail(int w,int h){
    // Legal, in-gamut chroma alternates at the native 2x2 NV12 sampling rate.
    // The native plane path must preserve this detail exactly.
    std::vector<uint8_t> data(size_t(w)*h*3/2,128);
    for(int y=0;y<h/2;++y)for(int x=0;x<w;x+=2){
        const bool alternate=((x/2+y)&1)!=0;const size_t at=size_t(w)*h+size_t(y)*w+x;
        data[at]=alternate?104:152;data[at+1]=alternate?152:104;
    }
    return data;
}
std::vector<uint8_t> color_grid(int w,int h){
    const int tile_w=(w/16)&~1,tile_h=(h/8)&~1;
    require(tile_w>=12&&tile_h>=12,"Color grid requires at least 192x96");
    std::vector<uint8_t> data(size_t(w)*h*3/2);
    const auto yuv=[&](int x,int y){
        const int col=(std::min)(15,x/tile_w),row=(std::min)(7,y/tile_h);
        const double r=col/15.0,g=row/7.0,b=((col*73+row*53)%256)/255.0;
        const double luma=.2126*r+.7152*g+.0722*b;
        return std::array<uint8_t,3>{uint8_t(std::lround(16+219*luma)),
            uint8_t(std::lround(128+224*(b-luma)/1.8556)),
            uint8_t(std::lround(128+224*(r-luma)/1.5748))};
    };
    for(int y=0;y<h;++y)for(int x=0;x<w;++x)data[size_t(y)*w+x]=yuv(x,y)[0];
    for(int y=0;y<h;y+=2)for(int x=0;x<w;x+=2){const auto v=yuv(x,y);const size_t at=size_t(w)*h+size_t(y/2)*w+x;data[at]=v[1];data[at+1]=v[2];}
    return data;
}
void report_color_difference(const char* label,const std::vector<uint8_t>& actual,const std::vector<uint8_t>& expected,int w,int h,bool grid,bool enforce){
    std::array<int,3> maximum{};std::array<double,3> bias{},absolute{},counts{};
    const int tile_w=(w/16)&~1,tile_h=(h/8)&~1;
    const auto inside=[&](int x,int y){
        if(!grid)return true;
        const int col=(std::min)(15,x/tile_w),row=(std::min)(7,y/tile_h);
        return x>=col*tile_w+4&&x<(col==15?w:(col+1)*tile_w)-4&&y>=row*tile_h+4&&y<(row==7?h:(row+1)*tile_h)-4;
    };
    const auto add=[&](size_t at,int channel){const int delta=int(actual[at])-expected[at];maximum[channel]=(std::max)(maximum[channel],std::abs(delta));bias[channel]+=delta;absolute[channel]+=std::abs(delta);++counts[channel];};
    for(int y=0;y<h;++y)for(int x=0;x<w;++x)if(inside(x,y))add(size_t(y)*w+x,0);
    for(int y=0;y<h;y+=2)for(int x=0;x<w;x+=2)if(inside(x,y)){const size_t at=size_t(w)*h+size_t(y/2)*w+x;add(at,1);add(at+1,2);}
    std::cout<<label<<" MAX_YUV "<<maximum[0]<<' '<<maximum[1]<<' '<<maximum[2]<<" MEAN_ABS_YUV";
    for(int i=0;i<3;++i)std::cout<<' '<<absolute[i]/counts[i];std::cout<<" BIAS_YUV";
    for(int i=0;i<3;++i)std::cout<<' '<<bias[i]/counts[i];std::cout<<'\n';
    if(enforce)require(*std::max_element(maximum.begin(),maximum.end())<=3,"Color ramp/grid changed by more than 3 LSB");
}
double color_field(double x,double y,uint32_t seed){
    x=(x+1024)/8.0;y=(y+1024)/8.0;const int gx=int(std::floor(x)),gy=int(std::floor(y));
    const double a=x-gx,b=y-gy;
    const auto at=[&](int dx,int dy){return double(hash(uint32_t(gx+dx)*733U+uint32_t(gy+dy)*19349663U+seed)%1024)/1023.0;};
    return (1-a)*(1-b)*at(0,0)+a*(1-b)*at(1,0)+(1-a)*b*at(0,1)+a*b*at(1,1);
}
std::vector<uint8_t> moving_color(int w,int h,double shift,bool hard){
    std::vector<uint8_t> data(size_t(w)*h*3/2);
    for(int y=0;y<h;++y)for(int x=0;x<w;++x)data[size_t(y)*w+x]=uint8_t(std::lround(72+112*color_field(x-shift,y,0)));
    for(int y=0;y<h;y+=2)for(int x=0;x<w;x+=2){
        const size_t at=size_t(w)*h+size_t(y/2)*w+x;
        if(hard){const bool tile=((int(std::floor((x-shift+1024)/8))+y/8)&1)!=0;data[at]=tile?104:152;data[at+1]=tile?152:104;}
        else{data[at]=uint8_t(std::lround(104+48*color_field(x-shift,y,918273)));data[at+1]=uint8_t(std::lround(104+48*color_field(x-shift,y,192837)));}
    }
    return data;
}
struct MotionColorError {
    std::array<double,3> square{},bias{},count{};double actual_chroma=0,expected_chroma=0;
    void add(const std::vector<uint8_t>& actual,const std::vector<uint8_t>& expected,int w,int h){
        const auto value=[&](size_t i,int c){double d=int(actual[i])-expected[i];square[c]+=d*d;bias[c]+=d;++count[c];};
        for(int y=32;y<h-32;++y)for(int x=64;x<w-64;++x)value(size_t(y)*w+x,0);
        for(int y=16;y<h/2-16;++y)for(int x=64;x<w-64;x+=2){const size_t at=size_t(w)*h+size_t(y)*w+x;value(at,1);value(at+1,2);
            actual_chroma+=std::hypot(int(actual[at])-128,int(actual[at+1])-128);expected_chroma+=std::hypot(int(expected[at])-128,int(expected[at+1])-128);}
    }
    void report(const std::string& label)const{std::cout<<label<<" MSE_YUV";for(int c=0;c<3;++c)std::cout<<' '<<square[c]/count[c];std::cout<<" BIAS_YUV";for(int c=0;c<3;++c)std::cout<<' '<<bias[c]/count[c];std::cout<<" CHROMA_MAGNITUDE_RATIO "<<actual_chroma/expected_chroma<<'\n';}
};
void report_motion_position(const std::string& label,const std::vector<uint8_t>& data,int w,int h,double base,bool hard){
    std::cout<<label<<" LUMA_MSE_SHIFT";
    for(double shift:{base-4,base,base+4,base+8,base+12})std::cout<<' '<<shift<<':'<<mse(data,moving_color(w,h,shift,hard),w,h);
    std::cout<<'\n';
}
void moving_color_diagnostic(GpuFrucEngine& engine,ID3D11Device* device,ID3D11DeviceContext* context,int w,int h){
    const auto upload=[&](const std::vector<uint8_t>& data,int64_t pts){
        D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.MipLevels=1;desc.ArraySize=1;desc.Format=DXGI_FORMAT_NV12;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial{data.data(),UINT(w),UINT(data.size())};ComPtr<ID3D11Texture2D> texture;check(device->CreateTexture2D(&desc,&initial,&texture),"Upload moving-color test frame");return engine.copy({texture,0,w,h,pts});
    };
    for(bool hard:{false,true}){
        const std::string label=hard?"MOVING_HARD_CHROMA":"MOVING_SMOOTH_CHROMA";
        auto a=moving_color(w,h,0,hard),b=moving_color(w,h,8,hard),truth=moving_color(w,h,4,hard);
        auto left=upload(a,0),right=upload(b,417083);engine.reset();
        MotionColorError motion_error,blend_error;
        const auto first_middle=readback(device,context,engine.midpoint(left,right));
        motion_error.add(first_middle,truth,w,h);report_motion_position(label+"_FRUC_POSITION",first_middle,w,h,0,hard);
        blend_error.add(readback(device,context,engine.blend(left,right,208541)),truth,w,h);
        motion_error.report(label+"_FRUC");blend_error.report(label+"_DIRECT_BLEND");
        for(int trial=0;trial<2;++trial){engine.reset();const auto repeat=readback(device,context,engine.midpoint(left,right));report_motion_position(label+"_RESET_"+std::to_string(trial),repeat,w,h,0,hard);}
        for(Rate rate:{Rate{48000,1001},Rate{120000,1001}}){
            std::array<std::vector<uint8_t>,3> source;std::array<int64_t,3> times{};
            for(int i=0;i<3;++i){source[i]=moving_color(w,h,8*i,hard);times[i]=int64_t(i)*10000000*1001/24000;}
            engine.reset();GpuHybridPipeline pipeline(rate,[&](const GpuFrame& first,const GpuFrame& second){auto middle=engine.midpoint(first,second);
                if(rate.num==48000){auto bytes=readback(device,context,middle);auto a_bytes=readback(device,context,first),b_bytes=readback(device,context,second);std::cout<<"MIDPOINT_BEFORE_PIPELINE_BLEND pts="<<middle.pts<<" equalsPrevious="<<(bytes==a_bytes)<<" equalsCurrent="<<(bytes==b_bytes)<<'\n';}
                return middle;},[&](const GpuFrame& first,const GpuFrame& second,int64_t time){return engine.blend(first,second,time);});
            MotionColorError generated_error;int original_count=0,generated_count=0;
            const auto emit=[&](const GpuOutputFrame& frame){
                if(frame.frame.pts>times.back())return true;
                auto actual=readback(device,context,frame.frame);bool original=false;
                for(int i=0;i<3;++i)if(frame.frame.pts==times[i]){require(actual==source[i],"Active interpolation changed an original source-time frame");++original_count;original=true;break;}
                if(!original){const double shift=double(frame.frame.pts)*24000/(10000000.0*1001)*8;auto expected=moving_color(w,h,shift,hard);generated_error.add(actual,expected,w,h);++generated_count;
                    if(rate.num==48000)report_motion_position(label+"_2X_FRAME_"+std::to_string(frame.frame.pts),actual,w,h,std::floor(shift/8)*8,hard);
                }
                return true;
            };
            for(int i=0;i<3;++i)pipeline.push(upload(source[i],times[i]),false,emit);
            pipeline.finish(417084,emit);require(original_count==3,"Active pipeline did not retain all exact source-time frames");
            generated_error.report(label+(rate.num==48000?"_PIPELINE_2X":"_PIPELINE_120"));
            std::cout<<"ACTIVE_ORIGINAL_FRAMES_EXACT "<<original_count<<" GENERATED_FRAMES "<<generated_count<<'\n';
        }
    }
}
void validate_state(ID3D11DeviceContext* context){D3D11_VIEWPORT viewport{};UINT count=1;context->RSGetViewports(&count,&viewport);require(count==1&&viewport.TopLeftX==11&&viewport.TopLeftY==13&&viewport.Width==123&&viewport.Height==145,"Engine failed to restore caller D3D11 context state");}
}
int wmain(int argc,wchar_t**argv){
    try{
        if(argc<2){std::cerr<<"Usage: gpu_engine_smoke <runtime directory> [width height]\n";return 2;}
        const int w=argc>2?_wtoi(argv[2]):640,h=argc>3?_wtoi(argv[3]):360;
        ComPtr<IDXGIFactory1> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"CreateDXGIFactory1");ComPtr<IDXGIAdapter1> adapter;
        for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> candidate;if(factory->EnumAdapters1(i,&candidate)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 desc{};candidate->GetDesc1(&desc);if(desc.VendorId==0x10de){adapter=candidate;break;}}
        require(bool(adapter),"No NVIDIA D3D11 adapter");ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
        check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_VIDEO_SUPPORT|D3D11_CREATE_DEVICE_BGRA_SUPPORT,levels,2,D3D11_SDK_VERSION,&device,nullptr,&context),"D3D11CreateDevice");
        D3D11_VIEWPORT preserved{11,13,123,145,0,1};context->RSSetViewports(1,&preserved);
        const int coded_h=(h+15)/16*16;
        const auto pad=[&](const std::vector<uint8_t>& input){std::vector<uint8_t> output(size_t(w)*coded_h*3/2,128);memcpy(output.data(),input.data(),size_t(w)*h);memcpy(output.data()+size_t(w)*coded_h,input.data()+size_t(w)*h,size_t(w)*h/2);return output;};
        auto a=pixels(w,h,0),b=pixels(w,h,8),truth=pixels(w,h,4);auto padded_a=pad(a),padded_b=pad(b);
        D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=coded_h;desc.MipLevels=1;desc.ArraySize=2;desc.Format=DXGI_FORMAT_NV12;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial[2]={{padded_a.data(),UINT(w),UINT(padded_a.size())},{padded_b.data(),UINT(w),UINT(padded_b.size())}};ComPtr<ID3D11Texture2D> array;check(device->CreateTexture2D(&desc,initial,&array),"Create decoder-like NV12 texture array");
        GpuFrame left{array,0,w,h,0},right{array,1,w,h,400000};
        HANDLE mutex=CreateMutexW(nullptr,FALSE,nullptr);require(mutex!=nullptr,"Create test decoder mutex failed");
        {
            GpuFrucEngine engine(std::filesystem::path(argv[1]),device.Get(),context.Get(),mutex);std::cout<<"DEVICE "<<engine.device_name()<<" SIZE "<<w<<'x'<<h<<'\n';
            const bool motion_only=argc>4 && std::wstring(argv[4])==L"motion-only";
            if(motion_only){moving_color_diagnostic(engine,device.Get(),context.Get(),w,h);}
            else {
            auto owned_a=engine.copy(left),owned_b=engine.copy(right);validate_state(context.Get());
            auto snapshot=readback(device.Get(),context.Get(),owned_a);require(snapshot==a,"GPU snapshot did not preserve NV12 array slice exactly");
            const auto first_start=std::chrono::steady_clock::now();auto middle=engine.midpoint(owned_a,owned_b);const double first_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-first_start).count();
            validate_state(context.Get());require(middle.pts==200000,"GPU midpoint timestamp incorrect");auto result=readback(device.Get(),context.Get(),middle);
            auto mixed=engine.blend(owned_a,owned_b,200000);validate_state(context.Get());auto mixed_data=readback(device.Get(),context.Get(),mixed);
            auto expected_blend=a;for(size_t i=0;i<a.size();++i)expected_blend[i]=uint8_t((int(a[i])+b[i]+1)/2);
            const double motion_error=mse(result,truth,w,h),blend_error=mse(mixed_data,truth,w,h),blend_rounding=mse(mixed_data,expected_blend,w,h);
            std::cout<<"MIDPOINT_MSE "<<motion_error<<" BLEND_MSE "<<blend_error<<" BLEND_ROUNDING_MSE "<<blend_rounding<<" LEFT_DIFF "<<mse(result,a,w,h)<<" RIGHT_DIFF "<<mse(result,b,w,h)<<" FIRST_MIDPOINT_MS "<<first_ms<<'\n';
            require(motion_error<blend_error*0.40,"GPU motion result did not significantly outperform blend");require(blend_rounding<4,"GPU blend did not match ordinary interpolation within color rounding");
            // Recycle the decoder array immediately after copy; owned snapshots must survive.
            std::vector<uint8_t> overwritten(padded_a.size(),16);context->UpdateSubresource(array.Get(),0,nullptr,overwritten.data(),w,0);require(readback(device.Get(),context.Get(),owned_a)==a,"Decoder array recycling corrupted owned snapshot");
            std::vector<double> continuous,blend_times;
            auto previous=owned_b;
            for(int i=2;i<=15;++i){auto next_data=pad(pixels(w,h,i*8));context->UpdateSubresource(array.Get(),1,nullptr,next_data.data(),w,0);GpuFrame next_ref{array,1,w,h,int64_t(i)*400000};auto next=engine.copy(next_ref);
                auto start=std::chrono::steady_clock::now();auto generated=engine.midpoint(previous,next);continuous.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
                start=std::chrono::steady_clock::now();auto output=engine.blend(previous,generated,previous.pts+100000);blend_times.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());previous=next;
            }
            std::cout<<"STEADY_MIDPOINT_MS "<<std::accumulate(continuous.begin(),continuous.end(),0.0)/continuous.size()<<" GPU_BLEND_MS "<<std::accumulate(blend_times.begin(),blend_times.end(),0.0)/blend_times.size()<<'\n';
            for(int i=0;i<5;++i){engine.reset();auto generated=engine.midpoint(owned_a,owned_b);require(generated.pts==200000,"GPU seek/reprime failed");}
            auto colored=a;for(size_t i=size_t(w)*h;i<colored.size();++i)colored[i]=uint8_t(80+i%97);auto padded_color=pad(colored);context->UpdateSubresource(array.Get(),0,nullptr,padded_color.data(),w,0);auto owned_color=engine.copy(left);require(readback(device.Get(),context.Get(),owned_color)==colored,"Padded NV12 chroma crop incorrect");
            // Static colored frames must remain native NV12 through FRUC and resampling.
            const auto bars=color_bars(w,h);const auto padded_bars=pad(bars);
            context->UpdateSubresource(array.Get(),0,nullptr,padded_bars.data(),w,0);
            auto bars_a=engine.copy(left),bars_b=engine.copy(left);bars_b.pts=400000;
            engine.reset();auto bars_mid=engine.midpoint(bars_a,bars_b);
            const auto errors=color_error(readback(device.Get(),context.Get(),bars_mid),bars,w,h);
            std::cout<<"BT709_LIMITED_COLOR_MAX_ABS_YUV "<<errors[0]<<' '<<errors[1]<<' '<<errors[2]<<'\n';
            require(*std::max_element(errors.begin(),errors.end())<=3,"Static colored FRUC round-trip exceeded 3 LSB; range/matrix/channel-order mismatch");
            auto bars_blend=engine.blend(bars_a,bars_b,200000);
            const auto blend_colors=color_error(readback(device.Get(),context.Get(),bars_blend),bars,w,h);
            std::cout<<"BT709_LIMITED_BLEND_MAX_ABS_YUV "<<blend_colors[0]<<' '<<blend_colors[1]<<' '<<blend_colors[2]<<'\n';
            require(*std::max_element(blend_colors.begin(),blend_colors.end())<=3,"Static colored GPU blend round-trip exceeded 3 LSB");
            for(int pattern=0;pattern<4;++pattern){
                const auto oracle=pattern==1?color_grid(w,h):pattern==3?chroma_detail(w,h):gray_ramp(w,h,pattern==0);
                const auto padded=pad(oracle);context->UpdateSubresource(array.Get(),0,nullptr,padded.data(),w,0);
                auto first=engine.copy(left),second=engine.copy(left);second.pts=400000;
                require(readback(device.Get(),context.Get(),first)==oracle,"Color diagnostic GPU copy must remain bit-exact");
                engine.reset();auto generated=engine.midpoint(first,second);auto blended=engine.blend(first,second,200000);
                const char* midpoint_label=pattern==0?"LIMITED_GRAY_FRUC":pattern==1?"COLOR_GRID_FRUC":pattern==2?"OUTSIDE_LIMITED_GRAY_FRUC":"CHROMA_2X2_FRUC";
                const char* blend_label=pattern==0?"LIMITED_GRAY_BLEND":pattern==1?"COLOR_GRID_BLEND":pattern==2?"OUTSIDE_LIMITED_GRAY_BLEND":"CHROMA_2X2_BLEND";
                const auto generated_data=readback(device.Get(),context.Get(),generated),blended_data=readback(device.Get(),context.Get(),blended);
                require(generated_data==oracle,"Native NV12 static FRUC changed luma/chroma/range values");
                require(blended_data==oracle,"Native NV12 static blend changed luma/chroma/range values");
                report_color_difference(midpoint_label,generated_data,oracle,w,h,pattern==1,true);
                report_color_difference(blend_label,blended_data,oracle,w,h,pattern==1,true);
                if(pattern==1){
                    report_color_difference("COLOR_GRID_EDGES_FRUC",generated_data,oracle,w,h,false,false);
                    report_color_difference("COLOR_GRID_EDGES_BLEND",blended_data,oracle,w,h,false,false);
                }
            }
            // Match the CPU resampler byte-for-byte for non-identical, chromatic
            // frames and fractional weights, including values outside TV range.
            const auto blend_source_a=gray_ramp(w,h,false),blend_source_b=color_grid(w,h);
            auto blend_pad_a=pad(blend_source_a),blend_pad_b=pad(blend_source_b);
            context->UpdateSubresource(array.Get(),0,nullptr,blend_pad_a.data(),w,0);
            context->UpdateSubresource(array.Get(),1,nullptr,blend_pad_b.data(),w,0);
            auto reference_a=engine.copy(left),reference_b=engine.copy(right);
            for(int64_t time:{1LL,100000LL,133333LL,200000LL,399999LL}){
                const uint32_t weight=uint32_t(time*65536/400000);
                std::vector<uint8_t> expected(blend_source_a.size());
                for(size_t i=0;i<expected.size();++i)expected[i]=uint8_t((uint32_t(blend_source_a[i])*(65536-weight)+uint32_t(blend_source_b[i])*weight+32768)>>16);
                auto actual=engine.blend(reference_a,reference_b,time);
                require(actual.pts==time&&readback(device.Get(),context.Get(),actual)==expected,"Native NV12 GPU blend differs from CPU fixed-point byte blend");
            }
            // Array slices also remain supported when called directly, not only
            // through the filter's tightly packed owned snapshots.
            auto array_blend=engine.blend(left,right,200000);
            auto owned_blend=engine.blend(reference_a,reference_b,200000);
            require(readback(device.Get(),context.Get(),array_blend)==readback(device.Get(),context.Get(),owned_blend),"Native plane blend of padded array slices differs from owned input");
            std::cout<<"NATIVE_NV12_FIXED_POINT_BLEND exact at5 weights; padded array slice blend=exact\n";
            engine.reset();engine.reset();validate_state(context.Get());
            auto still_old=readback(device.Get(),context.Get(),middle);require(still_old==result,"Later outputs overwrote a retained output texture");
            }
        }
        require(WaitForSingleObject(mutex,0)==WAIT_OBJECT_0,"Borrowed decoder mutex remained locked or was closed");ReleaseMutex(mutex);CloseHandle(mutex);
        if(argc>4 && std::wstring(argv[4])==L"motion-only"){std::cout<<"PASS moving-color diagnostics completed; original source-time frames remained exact.\n";return 0;}
        std::cout<<"PASS GPU-only processing: NV12 padded array capture including chroma, native NV12 NvOFFRUC, exact luma/chroma/range preservation including edges, fixed-point plane blend, context restore, immutable retained outputs, decoder-array reuse, 5 reset/reprime cycles, shutdown. CPU readback used only by test oracle.\n";return 0;
    }catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}


