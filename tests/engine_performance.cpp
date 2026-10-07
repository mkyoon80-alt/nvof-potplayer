#include "nvof/engine.hpp"
#include "nvof/pipeline.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <numeric>
#include <vector>
using Clock=std::chrono::steady_clock;
using namespace nvof;
namespace {
constexpr int W=1920,H=1080;
Frame frame(int shift,int64_t pts) {
    Frame f{W,H,pts,std::vector<uint8_t>(size_t(W)*H*3/2,128)};
    for(int y=0;y<H;++y) for(int x=0;x<W;++x) {
        const int sx=x-shift+1024;
        f.pixels[size_t(y)*W+x]=uint8_t(24+((sx/12*31+y/9*23+(sx*y)/173)%208));
    }
    return f;
}
double ms(Clock::time_point start) {return std::chrono::duration<double,std::milli>(Clock::now()-start).count();}
void stats(const char* label,std::vector<double> timings) {
    std::sort(timings.begin(),timings.end());
    std::cout<<label<<" count="<<timings.size()<<" mean_ms="<<std::accumulate(timings.begin(),timings.end(),0.0)/timings.size()
             <<" p95_ms="<<timings[size_t((timings.size()-1)*0.95)]<<" max_ms="<<timings.back()<<'\n';
}
}
int wmain(int argc,wchar_t** argv) {
    try {
        if(argc!=2) {std::cerr<<"Usage: engine_performance <runtime directory>\n";return 2;}
        FrucEngine engine{std::filesystem::path(argv[1])};
        std::cout<<"DEVICE "<<engine.device_name()<<" RESOLUTION 1920x1080 SOURCE 24000/1001 TARGET 120000/1001\n";
        // Build once outside timed regions so scene synthesis does not affect measurements.
        std::vector<Frame> inputs;
        for(int i=0;i<64;++i) inputs.push_back(frame(i*4,int64_t(i)*10000000*1001/24000));
        std::vector<double> midpoint_times,pipeline_times,reset_times,first_preview_times,reprime_times;
        uint64_t sink=0; size_t emitted=0;
        const auto consume=[&](const OutputFrame& o){sink+=o.frame.pixels[W*H/2];++emitted;return true;};
        HybridPipeline p({120000,1001},[&](const Frame&a,const Frame&b){
            const auto start=Clock::now();auto out=engine.midpoint(a,b);midpoint_times.push_back(ms(start));return out;
        });
        p.push(inputs[0],false,consume);
        for(size_t i=1;i<inputs.size();++i) {const auto start=Clock::now();p.push(inputs[i],false,consume);pipeline_times.push_back(ms(start));}
        p.finish(10000000LL*1001/24000,consume);
        std::cout<<"EMITTED "<<emitted<<" EXPECTED 320 CHECKSUM "<<sink<<'\n';
        if(emitted!=320) throw std::runtime_error("Performance scenario cadence mismatch");
        const double first_midpoint=midpoint_times.front();midpoint_times.erase(midpoint_times.begin());pipeline_times.erase(pipeline_times.begin());
        std::cout<<"FIRST_MIDPOINT_ms "<<first_midpoint<<'\n';stats("STEADY_GPU_MIDPOINT",midpoint_times);stats("STEADY_PIPELINE_INCLUDING_RESAMPLE",pipeline_times);
        for(int i=0;i<8;++i) {
            auto start=Clock::now();engine.reset();p.reset();reset_times.push_back(ms(start));
            start=Clock::now();p.push(inputs[0],false,consume);first_preview_times.push_back(ms(start));
            start=Clock::now();p.push(inputs[1],false,consume);reprime_times.push_back(ms(start));
        }
        stats("SEEK_RESET",reset_times);stats("SEEK_FIRST_PREVIEW",first_preview_times);stats("SEEK_NEXT_PAIR_WITH_REPRIME",reprime_times);
        std::cout<<"PASS performance probe; excludes renderer waits, decoder time and scene synthesis\n";
        return 0;
    }catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
