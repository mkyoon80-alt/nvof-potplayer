// Explicit NVIDIA hardware comparison. Readback is confined to diagnostics.
// No BFRC/real-video quality claim follows from these synthetic fixtures.
#define wmain original_gpu_smoke_entry
#include "gpu_engine_smoke.cpp"
#undef wmain
#include <d3d10_1.h>
#include <fstream>
#include <iomanip>
#include <string>

namespace {
GpuFrame upload(ID3D11Device* device,const std::vector<uint8_t>& bytes,int w,int h,int64_t pts) {
    D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=1;
    d.Format=DXGI_FORMAT_NV12;d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{bytes.data(),UINT(w),UINT(bytes.size())};
    ComPtr<ID3D11Texture2D> t;check(device->CreateTexture2D(&d,&initial,&t),"Upload fixture");
    return {t,0,w,h,pts};
}
std::vector<uint8_t> fixture(int w,int h,double time,int scene) {
    const double speed=scene==0?0.5:(scene==1?12.0:4.0);
    auto bytes=moving_color(w,h,time*speed,false);
    if(scene>=2) {
        const double left=w*.28+time*7,top=h*.3;
        const double right=left+w*.28,bottom=h*.72;
        // Independently moving foreground occludes the scrolling background.
        for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
            const double coverage=std::clamp((std::min)({x+1-left,right-x,y+1-top,bottom-y}),0.0,1.0);
            if(coverage>0) {
                double value=48+100*color_field(x-time*7,y,987123);
                if(scene==3 && y>h*.46 && y<h*.5)value=32; // fine foreground contour
                auto& v=bytes[size_t(y)*w+x];v=uint8_t(std::lround(v*(1-coverage)+value*coverage));
            }
        }
        for(int y=0;y<h/2;++y)for(int x=0;x<w;x+=2) {
            if(x>left+2&&x<right-2&&y*2>top+2&&y*2<bottom-2) {
                bytes[size_t(w)*h+size_t(y)*w+x]=104;bytes[size_t(w)*h+size_t(y)*w+x+1]=156;
            }
        }
    }
    if(scene==3) {
        // Static subtitle-like fine bars over moving video.
        for(int y=h-70;y<h-40;++y)for(int x=w/4;x<3*w/4;++x)
            if((x/3)%3==0)bytes[size_t(y)*w+x]=220;
    }
    return bytes;
}
struct Error {
    double abs=0,sq=0,uv=0;uint64_t count=0,bad=0,uvcount=0;
    void add(const std::vector<uint8_t>& a,const std::vector<uint8_t>& b,int w,int h) {
        for(int y=16;y<h-16;++y)for(int x=24;x<w-24;++x) {
            int d=std::abs(int(a[size_t(y)*w+x])-b[size_t(y)*w+x]);abs+=d;sq+=d*d;bad+=d>16;++count;
        }
        for(int y=8;y<h/2-8;++y)for(int x=24;x<w-24;++x) {
            uv+=std::abs(int(a[size_t(w)*h+size_t(y)*w+x])-b[size_t(w)*h+size_t(y)*w+x]);++uvcount;
        }
    }
    void report(const char* label,int scene)const {
        std::cout<<"QUALITY backend="<<label<<" scene="<<scene<<" y_mae="<<abs/count
          <<" y_mse="<<sq/count<<" bad16_pct="<<100.0*bad/count<<" uv_mae="<<uv/uvcount<<std::endl;
    }
};
void dump(const std::filesystem::path& root,const std::string& name,const std::vector<uint8_t>& bytes,int w,int h) {
    if(root.empty())return;
    std::ofstream out(root/(name+".pgm"),std::ios::binary);
    out<<"P5\n"<<w<<" "<<h<<"\n255\n";out.write(reinterpret_cast<const char*>(bytes.data()),size_t(w)*h);
    require(bool(out),"Diagnostic image write failed");
}
void drain(ID3D11Device* device,ID3D11DeviceContext* context) {
    D3D11_QUERY_DESC q{D3D11_QUERY_EVENT,0};ComPtr<ID3D11Query> query;
    check(device->CreateQuery(&q,&query),"Benchmark completion");context->End(query.Get());context->Flush();
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    for(;;){auto hr=context->GetData(query.Get(),nullptr,0,0);check(hr,"GPU completion");if(hr==S_OK)break;
        require(std::chrono::steady_clock::now()<deadline,"GPU completion timed out");SwitchToThread();}
}
}
int wmain(int argc,wchar_t** argv){try{
    require(argc>=2,"native_synthesis_compare <runtime> [width height pairs output-dir flow-dimension]");
    int w=argc>2?std::stoi(argv[2]):640,h=argc>3?std::stoi(argv[3]):360;
    int pairs=argc>4?std::stoi(argv[4]):24;
    require(w>=320&&h>=180&&w<=3840&&h<=2160&&!(w&1)&&!(h&1)&&pairs>=12&&pairs<=120,"Invalid test bounds");
    std::filesystem::path output=argc>5?argv[5]:L"";
    if(!output.empty()){require(!std::filesystem::exists(output),"Output directory already exists");std::filesystem::create_directories(output);}
    unsigned flow=argc>6?std::stoul(argv[6]):1920;
    ComPtr<IDXGIFactory1> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI");
    ComPtr<IDXGIAdapter1> adapter;
    for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> a;if(factory->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND)break;
        DXGI_ADAPTER_DESC1 d{};a->GetDesc1(&d);if(d.VendorId==0x10de){adapter=a;break;}}
    require(bool(adapter),"NVIDIA adapter missing");
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
    check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,
          D3D11_CREATE_DEVICE_VIDEO_SUPPORT|D3D11_CREATE_DEVICE_BGRA_SUPPORT,levels,2,D3D11_SDK_VERSION,&device,nullptr,&context),"D3D11");
    ComPtr<ID3D10Multithread> protection;check(context.As(&protection),"Context protection");protection->SetMultithreadProtected(TRUE);
    std::cout<<std::fixed<<std::setprecision(4);
    // Quality fixtures are bounded independently of the performance dimensions.
    const int qw=640,qh=360;
    for(auto backend:{GpuInterpolationBackend::fruc,GpuInterpolationBackend::native_experimental}) {
        const char* name=backend==GpuInterpolationBackend::fruc?"fruc":"native";
        GpuFrucEngine engine(argv[1],device.Get(),context.Get(),nullptr,GpuCompletionMode::context_ordered,true,false,false,backend,flow);
        std::cout<<"DEVICE "<<engine.device_name()<<" backend="<<name<<std::endl;
        for(int scene=0;scene<4;++scene) {
            engine.reset();Error error,held;
            auto first=engine.copy(upload(device.Get(),fixture(qw,qh,0,scene),qw,qh,0));
            for(int i=0;i<12;++i) {
                auto a=fixture(qw,qh,i,scene),b=fixture(qw,qh,i+1,scene),truth=fixture(qw,qh,i+.5,scene);
                auto last=engine.copy(upload(device.Get(),b,qw,qh,int64_t(i+1)*166666));
                auto result=engine.interpolate_pair(first,last,{first.pts+83333});
                require(!result.quality.scene_cut&&result.frames.size()==1,"Unexpected fixture rejection");
                auto actual=readback(device.Get(),context.Get(),result.frames[0]);
                error.add(actual,truth,qw,qh);held.add(a,truth,qw,qh);
                if(scene==1){
                    auto panError=mse(actual,truth,qw,qh);
                    std::cout<<"PAN_PAIR backend="<<name<<" pair="<<i<<" mse="<<panError<<std::endl;
                    require(panError<1.0,"First or subsequent fast-pan midpoint is incorrect");
                }
                require(readback(device.Get(),context.Get(),first)==a,"Original modified");
                first=last;
                if(i==5){dump(output,std::string(name)+"-"+std::to_string(scene),actual,qw,qh);
                    if(backend==GpuInterpolationBackend::fruc)dump(output,"truth-"+std::to_string(scene),truth,qw,qh);}
            }
            error.report(name,scene);held.report("hold",scene);
            if(backend==GpuInterpolationBackend::native_experimental&&scene==3)
                require(error.sq/error.count<10.0,"Fine stationary contours over motion degraded");
            if(scene==1)require(error.sq<held.sq,"Continuous-motion synthesis is worse than holding source");
        }
        if(backend==GpuInterpolationBackend::native_experimental) {
            // A long vertical pan with changing speed exercises successive input
            // allocations. Short, constant-speed horizontal fixtures missed a
            // real-video regression where every other midpoint stayed near A.
            const int sw=960,sh=540;
            const auto vertical=[&](double shift) {
                auto horizontal=moving_color(sh,sw,shift,false);
                std::vector<uint8_t> image(size_t(sw)*sh*3/2);
                for(int y=0;y<sh;++y)for(int x=0;x<sw;++x)
                    image[size_t(y)*sw+x]=horizontal[size_t(x)*sh+y];
                for(int y=0;y<sh/2;++y)for(int x=0;x<sw/2;++x)for(int c=0;c<2;++c)
                    image[size_t(sw)*sh+size_t(y)*sw+x*2+c]=horizontal[size_t(sw)*sh+size_t(x)*sh+y*2+c];
                return image;
            };
            engine.reset();double shift=0,maxError=0;
            auto first=engine.copy(upload(device.Get(),vertical(shift),sw,sh,0));
            for(int i=0;i<64;++i) {
                const double step=8.0+(i%9)*0.5;
                auto next=engine.copy(upload(device.Get(),vertical(shift+step),sw,sh,int64_t(i+1)*166666));
                auto batch=engine.interpolate_pair(first,next,{first.pts+83333});
                if(batch.quality.scene_cut || batch.frames.empty())std::cout<<"SEQUENCE_REJECT pair="<<i<<" cut="<<batch.quality.scene_cut<<std::endl;
                require(!batch.quality.scene_cut && batch.frames.size()==1,"Sequence pan rejected as scene cut");
                auto generated=readback(device.Get(),context.Get(),batch.frames[0]);
                const double error=mse(generated,vertical(shift+step*.5),sw,sh);
                maxError=(std::max)(maxError,error);
                if(error>=8.0) {
                    std::cout<<"SEQUENCE_ERROR pair="<<i<<" mse="<<error<<" shift="<<shift<<" step="<<step<<std::endl;
                    dump(output,"sequence-A",vertical(shift),sw,sh);dump(output,"sequence-B",vertical(shift+step),sw,sh);
                    dump(output,"sequence-mid",generated,sw,sh);dump(output,"sequence-truth",vertical(shift+step*.5),sw,sh);
                }
                require(error<8.0,"Successive vertical-pan midpoint has stale motion");
                shift+=step;first=next;
            }
            std::cout<<"SEQUENCE_VERTICAL count=64 max_mse="<<maxError<<std::endl;
        }
        if(backend==GpuInterpolationBackend::native_experimental) {
            // Opposed foreground/background motion creates steep flow gradients
            // at an occlusion boundary, unlike the constant-pan regressions.
            const int lw=960,lh=540;
            const auto layered=[&](double t) {
                auto image=moving_color(lw,lh,-24*t,false);
                const double left=lw*.28+64*t,right=left+lw*.30;
                for(int y=90;y<450;++y)for(int x=0;x<lw;++x) {
                    double coverage=std::clamp((std::min)(x+1-left,right-x),0.0,1.0);
                    double value=48+100*color_field(x-64*t,y,987123);
                    auto& v=image[size_t(y)*lw+x];v=uint8_t(std::lround(v*(1-coverage)+value*coverage));
                }
                return image;
            };
            engine.reset();Error error,held;
            for(int i=0;i<6;++i) {
                double time=i*.12;
                auto source=layered(time),end=layered(time+1),truth=layered(time+.5);
                auto first=engine.copy(upload(device.Get(),source,lw,lh,0));
                auto next=engine.copy(upload(device.Get(),end,lw,lh,400000));
                auto batch=engine.interpolate_pair(first,next,{200000});
                require(!batch.quality.scene_cut&&batch.frames.size()==1,"Opposed layers rejected");
                auto actual=readback(device.Get(),context.Get(),batch.frames[0]);
                error.add(actual,truth,lw,lh);held.add(source,truth,lw,lh);
            }
            error.report("opposed-layers",5);held.report("opposed-hold",5);
            require(error.sq<held.sq*.5,"Occlusion boundaries fail to improve on holding source");
        }
        if(backend==GpuInterpolationBackend::native_experimental) {
            // A moving surface can change brightness without becoming a cut.
            // Its intermediate position must survive low photometric confidence.
            engine.reset();
            auto source=moving_color(qw,qh,0,false);
            auto end=moving_color(qw,qh,8,false),truth=moving_color(qw,qh,4,false);
            for(size_t i=0;i<size_t(qw)*qh;++i){end[i]+=32;truth[i]+=16;}
            auto first=engine.copy(upload(device.Get(),source,qw,qh,0));
            auto next=engine.copy(upload(device.Get(),end,qw,qh,400000));
            auto batch=engine.interpolate_pair(first,next,{200000});
            require(!batch.quality.scene_cut&&batch.frames.size()==1,"Brightness transition rejected");
            const auto actual=readback(device.Get(),context.Get(),batch.frames[0]);
            const double error=mse(actual,truth,qw,qh);
            std::cout<<"MOVING_EXPOSURE mse="<<error<<std::endl;
            require(error<8.0,"Low-confidence motion snaps to an unwarped source");
        }
        if(backend==GpuInterpolationBackend::native_experimental) {
            engine.reset();
            auto first=engine.copy(upload(device.Get(),moving_color(qw,qh,0,false),qw,qh,0));
            auto next=engine.copy(upload(device.Get(),moving_color(qw,qh,12,false),qw,qh,400000));
            const std::vector<int64_t> times{1000,100000,200000,300000,399000};
            auto batch=engine.interpolate_pair(first,next,times);
            require(batch.frames.size()==times.size()&&batch.quality.native_synthesized_mask==31,"Fractional phases missing");
            double worst=0;
            for(size_t i=0;i<times.size();++i) {
                require(batch.frames[i].pts==times[i],"Fractional output timestamp mismatch");
                const auto actual=readback(device.Get(),context.Get(),batch.frames[i]);
                const auto truth=moving_color(qw,qh,12.0*times[i]/400000.0,false);
                const auto error=mse(actual,truth,qw,qh);worst=(std::max)(worst,error);
                require(error<2.0,"Fractional output has the wrong motion position");
            }
            auto distant=next;distant.pts=100000000;
            auto endpoints=engine.interpolate_pair(first,distant,{1,99999999});
            require(endpoints.frames.size()==2,"Near-endpoint phase rounded outside the interval");
            require(mse(readback(device.Get(),context.Get(),endpoints.frames[0]),moving_color(qw,qh,0,false),qw,qh)<2.0 &&
                    mse(readback(device.Get(),context.Get(),endpoints.frames[1]),moving_color(qw,qh,12,false),qw,qh)<2.0,
                    "Near-endpoint phase has wrong position");
            auto unchanged=first;unchanged.pts=400000;
            auto exact=engine.interpolate_pair(first,unchanged,times);
            require(exact.quality.identical_warp_skipped&&exact.frames.size()==times.size(),"Static multiphase count mismatch");
            auto bytes=readback(device.Get(),context.Get(),first);
            for(size_t i=0;i<times.size();++i)
                require(exact.frames[i].pts==times[i]&&readback(device.Get(),context.Get(),exact.frames[i])==bytes,"Static phase changed Y/UV");
            require(engine.interpolate_pair(first,next,{}).frames.empty(),"Empty phase request produced output");
            std::cout<<"FRACTIONAL_PHASES count=5 max_mse="<<worst<<" static_exact=5 empty=OK near_endpoints=2"<<std::endl;
        }
        auto original=color_bars(qw,qh);auto a=engine.copy(upload(device.Get(),original,qw,qh,0));
        auto b=a;b.pts=166666;
        auto exact=engine.interpolate_pair(a,b,{83333});
        require(exact.quality.identical_warp_skipped&&readback(device.Get(),context.Get(),exact.frames.at(0))==original,"Static Y/UV changed");
        auto changed=original,cutOriginal=original;
        for(int y=0;y<qh;++y)for(int x=0;x<qw;++x){
            cutOriginal[size_t(y)*qw+x]=uint8_t(16+hash((x/32)*733U+(y/32)*193U)%96);
            changed[size_t(y)*qw+x]=uint8_t(176+hash((x/32)*313U+(y/32)*977U+1U)%60);
        }
        b=engine.copy(upload(device.Get(),cutOriginal,qw,qh,166666));
        auto cut=engine.copy(upload(device.Get(),changed,qw,qh,333332));
        require(engine.interpolate_pair(b,cut,{249999}).quality.scene_cut,"Scene cut not protected");
        auto m=engine.copy(upload(device.Get(),fixture(qw,qh,1,1),qw,qh,166666));
        a=engine.copy(upload(device.Get(),fixture(qw,qh,0,1),qw,qh,0));
        GpuFrame retained=engine.midpoint(a,m);auto retainedBytes=readback(device.Get(),context.Get(),retained);
        auto resetTruth=fixture(qw,qh,.5,1);double resetTotal=0,resetMaximum=0;
        for(int i=0;i<100;++i){
            auto begin=std::chrono::steady_clock::now();engine.reset();auto r=engine.midpoint(a,m);
            require(r.pts==83333,"Seek timestamp");
            require(mse(readback(device.Get(),context.Get(),r),resetTruth,qw,qh)<1.0,"First frame after seek has stale/incorrect motion");
            double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
            resetTotal+=ms;resetMaximum=(std::max)(resetMaximum,ms);
        }
        std::cout<<"RESET backend="<<name<<" count=100 mean_ms="<<resetTotal/100<<" max_ms="<<resetMaximum<<" includes_test_readback=1"<<std::endl;
        double reversedMaximum=0;
        for(int i=0;i<20;++i){
            engine.reset();double shift=(i&1)?-12.0:12.0;
            auto first=engine.copy(upload(device.Get(),moving_color(qw,qh,0,false),qw,qh,0));
            auto next=engine.copy(upload(device.Get(),moving_color(qw,qh,shift,false),qw,qh,166666));
            auto generated=readback(device.Get(),context.Get(),engine.midpoint(first,next));
            double error=mse(generated,moving_color(qw,qh,shift*.5,false),qw,qh);
            reversedMaximum=(std::max)(reversedMaximum,error);
            if(error>=1.0)std::cout<<"DIRECTION_ERROR backend="<<name<<" iteration="<<i<<" shift="<<shift<<" mse="<<error<<std::endl;
            require(error<1.0,"Motion direction leaked across seek");
        }
        std::cout<<"RESET_DIRECTION backend="<<name<<" count=20 max_mse="<<reversedMaximum<<std::endl;
        require(readback(device.Get(),context.Get(),retained)==retainedBytes,"Retained output overwritten after seek");
        engine.reset();
        std::vector<GpuFrame> inputs;
        for(int i=0;i<=pairs;++i)inputs.push_back(upload(device.Get(),moving_color(w,h,i*4,false),w,h,int64_t(i)*166666));
        drain(device.Get(),context.Get());
        std::vector<double> submit;GpuFrame previous=engine.copy(inputs[0]),last;
        auto start=std::chrono::steady_clock::now();
        for(int i=1;i<=pairs;++i) {
            if(i==6){drain(device.Get(),context.Get());start=std::chrono::steady_clock::now();}
            auto tick=std::chrono::steady_clock::now();auto next=engine.copy(inputs[i]);
            auto batch=engine.interpolate_pair(previous,next,{previous.pts+83333});
            require(!batch.quality.scene_cut&&batch.frames.size()==1,"Benchmark rejected");
            if(backend==GpuInterpolationBackend::native_experimental)
                require(batch.quality.native_synthesized_mask==1,"Benchmark bypassed native synthesis");
            last=batch.frames[0];previous=next;
            if(i>5)submit.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-tick).count());
        }
        drain(device.Get(),context.Get());
        const double average=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()/submit.size();
        Error full,heldFull;
        auto truth=moving_color(w,h,(pairs-.5)*4,false);
        full.add(readback(device.Get(),context.Get(),last),truth,w,h);
        heldFull.add(moving_color(w,h,(pairs-1)*4,false),truth,w,h);
        full.report(name,4);heldFull.report("hold",4);
        require(full.sq<heldFull.sq,"Full-resolution output does not improve motion");
        std::sort(submit.begin(),submit.end());
        std::cout<<"PERF backend="<<name<<" size="<<w<<"x"<<h<<" flow_max="<<flow
          <<" measured_pairs="<<submit.size()<<" drained_mean_ms="<<average
          <<" submit_p95_ms="<<submit[size_t((submit.size()-1)*.95)]<<std::endl;
    }
    std::cout<<"PASS originals, static YUV, cuts, 100 resets, retained outputs, continuous-motion quality; comparative metrics above\n";
    return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
