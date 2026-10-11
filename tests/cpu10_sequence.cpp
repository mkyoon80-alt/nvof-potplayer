// Stream decoded frames from stdin. No source video or raw frames are committed.
#include "nvof/cpu_upload.hpp"
#include "nvof/mf_bridge.hpp"
#include <io.h>
#include <fcntl.h>
#include <cstdio>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <optional>
int wmain(int argc,wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    try {
        if(argc<4)throw std::runtime_error("cpu10_sequence width height p010|p010-fields|p010-auto|y210-fields [fps=25] [flow-dimension=1920] [medium|slow] [grid=0]");
        const int w=std::stoi(argv[1]),h=std::stoi(argv[2]);const std::wstring mode=argv[3];
        const bool forceFields=mode.find(L"-fields")!=std::wstring::npos,y210=mode.find(L"y210")==0,autoFields=mode==L"p010-auto";
        const int fps=argc>4?std::stoi(argv[4]):25;const int64_t duration=10000000/fps;
        const nvof::CpuVideoLayout layout{w,h,w*(y210?4:2),y210?nvof::CpuVideoFormat::y210:nvof::CpuVideoFormat::p010};
        nvof::MfD3d11Bridge bridge;
        nvof::CpuUpload upload(bridge.device(),bridge.context(),bridge.mutex());
        nvof::MotionFlowOptions flow;
        flow.quality=argc>6 && std::wstring(argv[6])==L"slow"?nvof::MotionFlowQuality::slow:nvof::MotionFlowQuality::medium;
        flow.output_grid=argc>7?std::stoul(argv[7]):0;
        nvof::GpuFrucEngine engine(L"bin/runtime",bridge.device(),bridge.context(),bridge.mutex(),
            nvof::GpuCompletionMode::blocking,true,false,false,nvof::GpuInterpolationBackend::native_experimental,
            argc>5?std::stoul(argv[5]):1920,nvof::MotionCostMode::disabled,flow);
        std::vector<uint8_t> raw(nvof::cpu_video_bytes(layout));std::optional<nvof::GpuFrame> previous;
        _setmode(_fileno(stdin),_O_BINARY);int index=0;
        std::cout<<"index,pts,cut,hold,identical,risk,ms,fields,prepare_ms\n"<<std::fixed<<std::setprecision(4);
        for(;;++index) {
            size_t read=0;
            while(read<raw.size()) {size_t n=fread(raw.data()+read,1,raw.size()-read,stdin);if(!n)break;read+=n;}
            if(!read)break;if(read!=raw.size())throw std::runtime_error("Partial source frame");
            const auto prepare_start=std::chrono::steady_clock::now();
            upload.begin(raw.data(),raw.size(),layout,index==0);
            const bool fields=forceFields || (autoFields && upload.needs_field_reconstruction());
            const double prepare_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-prepare_start).count();
            for(int f=0;f<(fields?2:1);++f) {
                const auto start=std::chrono::steady_clock::now();
                auto input=upload.render(fields?f:-1,index*duration+(fields?f*duration/2:0));
                auto current=engine.copy(input);
                if(previous) {
                    std::vector<int64_t> times;
                    for(int64_t t=previous->pts+100000;t<current.pts;t+=100000)times.push_back(t);
                    const auto batch=engine.interpolate_pair(*previous,current,times);const auto info=engine.analysis_info();
                    const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
                    std::cout<<index<<','<<double(current.pts)/1e7<<','<<batch.quality.scene_cut<<','
                        <<(batch.quality.repeated_mask!=0)<<','<<batch.quality.identical_warp_skipped<<','
                        <<double(info.inconsistent_samples)/(128*72*2)<<','<<ms<<','<<fields<<','<<(f==0?prepare_ms:0)<<'\n';
                }
                previous=std::move(current);
            }
        }
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
