// Exercise the actual system-memory adapter with no app-local runtime directory.
#define wmain historical_gpu_test_entry
#include "gpu_engine_smoke.cpp"
#undef wmain
#include "nvof/engine.hpp"
int wmain(int argc,wchar_t** argv){try {
    require(argc==2,"native_cpu_transport <empty-or-absent-runtime>");
    FrucEngine engine(argv[1]);
    for(auto dimensions:{std::pair<int,int>{640,360},{960,540},{640,360}}){
        const int w=dimensions.first,h=dimensions.second;
        Frame a{w,h,0,moving_color(w,h,0,false)},b{w,h,417083,moving_color(w,h,8,false)};
        for(int trial=0;trial<3;++trial){
            engine.reset();
            auto batch=engine.interpolate_pair(a,b,{208541});
            require(batch.frames.size()==1 && batch.quality.native_synthesized_mask==1,"CPU transport did not synthesize natively");
            const auto& f=batch.frames[0];require(f.width==w&&f.height==h&&f.pts==208541,"CPU output layout/timestamp");
            auto truth=moving_color(w,h,4,false);
            double err=0,held=0;for(size_t i=0;i<truth.size();++i){
                double v=int(f.pixels[i])-truth[i],old=int(a.pixels[i])-truth[i];err+=v*v;held+=old*old;
            }
            require(err<held*0.6,"CPU midpoint is not materially better than an original hold");
            auto preserved=f.pixels;auto c=b;c.pts+=417083;c.pixels=moving_color(w,h,16,false);
            auto next=engine.interpolate_pair(b,c,{625624});require(next.frames.size()==1,"CPU adjacent pair missing");
            require(f.pixels==preserved,"Retained CPU output mutated");
        }
        // Every Y/UV value, including out-of-video-range codes, must survive unchanged.
        for(size_t i=0;i<a.pixels.size();++i)a.pixels[i]=uint8_t(i*37);
        b.pixels=a.pixels;engine.reset();
        auto still=engine.interpolate_pair(a,b,{208541});
        require(still.frames.size()==1&&still.frames[0].pixels==a.pixels,"Static CPU Y/UV changed");
        auto invalid=a;invalid.pixels.pop_back();bool rejected=false;
        try {engine.interpolate_pair(invalid,b,{208541});} catch(const std::invalid_argument&){rejected=true;}
        require(rejected,"Malformed system-memory frame accepted");
    }
    for(const auto name:{L"NvOFFRUC.dll",L"NvofFrucBridge.dll",L"NVEncNVOFFRUC.dll",L"cudart64_110.dll",L"cudart64_12.dll"})
        require(GetModuleHandleW(name)==nullptr,"Removed FRUC/CUDA runtime was loaded");
    std::cout<<"PASS native CPU NV12: motion quality, seek/reset, resolution changes, immutable output, exact Y/UV, malformed input, no FRUC/CUDA runtime.\n";
    return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
