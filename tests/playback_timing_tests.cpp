#include "nvof/playback_timing.hpp"
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){try{
    nvof::PlaybackTiming trace;
    trace.note_quality(true,400000,800);trace.note_output();
    require(trace.quality_count==0&&trace.output==0,"Disabled tracing changed counters");
    trace.enabled=true;trace.reset();
    std::vector<std::thread> workers;
    for(int i=0;i<4;++i)workers.emplace_back([&]{for(int j=0;j<1000;++j)trace.note_quality(j%2==0,j%2==0?400000:-100000,j%2==0?750:1200);});
    for(auto& worker:workers)worker.join();
    require(trace.quality_count==4000&&trace.quality_famine==2000,"Concurrent renderer notifications lost");
    require(trace.quality_late_sum==600000000&&trace.quality_late_max==400000&&trace.quality_proportion_min==750,"Renderer timing units or extrema changed");
    trace.note_output();trace.note_output();trace.receive.add(1);
    std::string output;trace.flush(true,[&](std::string line){output=std::move(line);});
    require(output.find("qualityCount=4000")!=std::string::npos&&output.find("qualityLateMaxMs=40.000000")!=std::string::npos,"Renderer notification report missing");
    require(output.find("outputGapCount=1")!=std::string::npos,"Delivery interval report missing");
    require(trace.quality_count==0&&trace.output==0&&trace.quality_proportion_min==1000,"Window counters were not cleared");
    trace.note_quality(true,900000,500);trace.reset();
    require(trace.quality_count==0&&trace.quality_late_max==0&&trace.last_output==nvof::PlaybackTiming::Clock::time_point{},"New playback retained old diagnostics");
    std::cout<<"PASS disabled trace, concurrent quality notifications, signed timing, delivery spacing and resets\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
