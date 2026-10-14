#include "nvof/playback_timing.hpp"
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <cmath>
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
    trace.shared_wait_calls=6;trace.combined_scene_pairs=3;
    trace.wait_submit.add(1.25);trace.wait_completion.add(3.5);trace.context_resume.add(0.25);
    trace.event_wait_calls=4;trace.query_wait_calls=2;
    std::string output;trace.flush(true,[&](std::string line){output=std::move(line);});
    require(output.find("qualityCount=4000")!=std::string::npos&&output.find("qualityLateMaxMs=40.000000")!=std::string::npos,"Renderer notification report missing");
    require(output.find("outputGapCount=1")!=std::string::npos,"Delivery interval report missing");
    require(output.find("sharedWaitCalls=6 combinedScenePairs=3")!=std::string::npos,"GPU synchronization counts missing");
    require(trace.shared_wait_calls==0&&trace.combined_scene_pairs==0,"GPU synchronization counts were not cleared");
    require(output.find("waitSubmitSumMs=1.250000")!=std::string::npos&&
        output.find("waitCompletionSumMs=3.500000")!=std::string::npos&&
        output.find("contextResumeSumMs=0.250000")!=std::string::npos&&
        output.find("eventWaitCalls=4 queryWaitCalls=2")!=std::string::npos,"Separated wait diagnostics missing or misattributed");
    require(trace.wait_submit.count==0&&trace.wait_completion.count==0&&trace.context_resume.count==0&&
        trace.event_wait_calls==0&&trace.query_wait_calls==0,"Separated wait diagnostics were not cleared");
    require(trace.quality_count==0&&trace.output==0&&trace.quality_proportion_min==1000,"Window counters were not cleared");
    trace.note_quality(true,900000,500);trace.reset();
    require(trace.quality_count==0&&trace.quality_late_max==0&&trace.last_output==nvof::PlaybackTiming::Clock::time_point{},"New playback retained old diagnostics");
    // A 50 ms producer stall followed by a burst changes wall-clock delivery,
    // never the 120 Hz media clock. No sleeps or nondeterministic timer limits.
    const auto origin=nvof::PlaybackTiming::Clock::now();
    for(int i=0;i<120;++i) {
        const int64_t start=int64_t(i)*10000000/120,stop=int64_t(i+1)*10000000/120;
        const int64_t wall_us=i==0?0:i==1?50000:i<7?50000+(i-1)*100:int64_t(i)*1000000/120;
        trace.note_output_at(start,stop,i==0,origin+std::chrono::microseconds(wall_us));
    }
    require(trace.output_duration.count==120&&std::abs(trace.output_duration.sum_ms-1000)<.001,"120 Hz media duration drifted");
    require(trace.output_stalls==1&&trace.output_bursts==5&&trace.output_pts_gaps==0&&trace.output_pts_overlaps==0,"Burst/stall delivery was mistaken for a PTS rate change");
    trace.receive.add(1);trace.flush(true,[&](std::string line){output=std::move(line);});
    require(output.find("outputMediaFps=120.000000")!=std::string::npos,"Media FPS diagnostic is wrong");
    require(trace.output_duration.count==0&&trace.have_output_pts,"Window reset lost PTS continuity");
    trace.note_output_at(0,166666,true,origin+std::chrono::seconds(2));
    require(trace.output_pts_overlaps==0&&trace.output_stalls==0,"Seek boundary counted as an overlap or stall");
    trace.note_output_at(166665,333332,false,origin+std::chrono::seconds(2));
    require(trace.output_pts_overlaps==1,"Actual PTS overlap was not detected");
    trace.reset();require(!trace.have_output_pts,"Playback restart retained the output clock");
    std::cout<<"PASS disabled trace, concurrent quality notifications, signed timing, delivery spacing and resets\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
