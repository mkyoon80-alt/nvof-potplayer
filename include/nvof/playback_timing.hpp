#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace nvof {
// Optional wall-clock diagnostics. These measure API/lock/wait time, not GPU
// timestamps or frames actually presented by the renderer. No pixel readback.
struct PlaybackMetric {
    uint64_t count=0;
    double sum_ms=0,max_ms=0;
    void add(double ms) noexcept {++count;sum_ms+=ms;max_ms=(std::max)(max_ms,ms);}
    std::string text(const char* name) const {
        return std::string(name)+"Count="+std::to_string(count)+" "+name+"SumMs="+std::to_string(sum_ms)+
            " "+name+"MaxMs="+std::to_string(max_ms)+" ";
    }
};
struct PlaybackTimer {
    using Clock=std::chrono::steady_clock;
    PlaybackMetric* metric;
    Clock::time_point start;
    PlaybackTimer(bool enabled,PlaybackMetric& value) noexcept:metric(enabled?&value:nullptr),start(enabled?Clock::now():Clock::time_point{}){}
    ~PlaybackTimer(){finish();}
    void finish() noexcept {if(metric){metric->add(std::chrono::duration<double,std::milli>(Clock::now()-start).count());metric=nullptr;}}
};
struct PlaybackTiming {
    using Clock=PlaybackTimer::Clock;
    bool enabled=false;
    Clock::time_point window{},last_exit{};
    PlaybackMetric receive,upstream_gap,capture,synthesis,allocator,delivery,status,output_gap;
    PlaybackMetric context_wait,scene,prepare,submit,completion,shared_wait;
    PlaybackMetric wait_submit,wait_completion,context_resume;
    PlaybackMetric output_duration;
    uint64_t output_bursts=0,output_stalls=0,output_pts_gaps=0,output_pts_overlaps=0;
    int64_t previous_stop=0,previous_duration=0;
    bool have_output_pts=false;
    Clock::time_point last_output{};
    // Renderer notifications can arrive on another thread. Observe them without
    // suppressing or changing upstream quality control, or taking stream locks.
    std::atomic<uint64_t> quality_count{0},quality_famine{0};
    std::atomic<int64_t> quality_late_sum{0},quality_late_max{0};
    std::atomic<int> quality_proportion_min{1000};
    uint64_t output=0;
    uint64_t shared_wait_calls=0,combined_scene_pairs=0,queued_output_flushes=0;
    uint64_t event_wait_calls=0,query_wait_calls=0;
    int64_t pts=0;
    void reset() noexcept {
        window=Clock::now();last_exit={};last_output={};clear();
        previous_stop=previous_duration=0;have_output_pts=false;
        quality_count=0;quality_famine=0;quality_late_sum=0;quality_late_max=0;quality_proportion_min=1000;
    }
    void clear() noexcept {receive={};upstream_gap={};capture={};synthesis={};allocator={};delivery={};status={};output_gap={};context_wait={};scene={};prepare={};submit={};completion={};shared_wait={};wait_submit={};wait_completion={};context_resume={};output=0;shared_wait_calls=0;combined_scene_pairs=0;queued_output_flushes=0;event_wait_calls=0;query_wait_calls=0;output_duration={};output_bursts=output_stalls=output_pts_gaps=output_pts_overlaps=0;}
    void note_output(int64_t start=0,int64_t stop=0,bool discontinuity=false) noexcept {
        if(!enabled)return;
        note_output_at(start,stop,discontinuity,Clock::now());
    }
    // Media cadence and wall-clock delivery are deliberately separate. Fast
    // catch-up after a stall must not look like a change of interpolation rate.
    void note_output_at(int64_t start,int64_t stop,bool discontinuity,Clock::time_point now) noexcept {
        if(!enabled)return;
        if(last_output!=Clock::time_point{}) {
            const double gap=std::chrono::duration<double,std::milli>(now-last_output).count();
            output_gap.add(gap);
            if(!discontinuity&&have_output_pts&&previous_duration>0) {
                const double expected=previous_duration/10000.0;
                output_bursts+=gap<expected*.5;
                output_stalls+=gap>expected*1.5;
            }
        }
        if(start>=0&&stop>start) {
            if(!discontinuity&&have_output_pts) {
                output_pts_gaps+=start>previous_stop;
                output_pts_overlaps+=start<previous_stop;
            }
            previous_duration=stop-start;previous_stop=stop;have_output_pts=true;
            output_duration.add(previous_duration/10000.0);
        }else have_output_pts=false;
        last_output=now;++output;
    }
    void note_quality(bool famine,int64_t late,int proportion) noexcept {
        if(!enabled)return;
        quality_count.fetch_add(1,std::memory_order_relaxed);
        if(famine)quality_famine.fetch_add(1,std::memory_order_relaxed);
        quality_late_sum.fetch_add(late,std::memory_order_relaxed);
        auto maximum=quality_late_max.load(std::memory_order_relaxed);
        while(late>maximum&&!quality_late_max.compare_exchange_weak(maximum,late,std::memory_order_relaxed)){}
        auto minimum=quality_proportion_min.load(std::memory_order_relaxed);
        while(proportion<minimum&&!quality_proportion_min.compare_exchange_weak(minimum,proportion,std::memory_order_relaxed)){}
    }
    template<class Write> void flush(bool force,Write write) noexcept {
        if(!enabled)return;
        try {
            const auto now=Clock::now();
            const double ms=std::chrono::duration<double,std::milli>(now-window).count();
            if((!force&&ms<1000)||!receive.count)return;
            write("performance elapsedMs="+std::to_string(ms)+" pts="+std::to_string(pts)+
                " delivered="+std::to_string(output)+" deliveredFps="+std::to_string(output*1000.0/ms)+" "+
                receive.text("receive")+upstream_gap.text("upstreamGap")+capture.text("capture")+
                synthesis.text("synthesis")+allocator.text("allocator")+delivery.text("delivery")+status.text("status")+
                output_gap.text("outputGap")+
                output_duration.text("outputDuration")+
                "outputMediaFps="+std::to_string(output_duration.sum_ms>0?output_duration.count*1000.0/output_duration.sum_ms:0)+
                " outputBurstCount="+std::to_string(output_bursts)+" outputStallCount="+std::to_string(output_stalls)+
                " outputPtsGaps="+std::to_string(output_pts_gaps)+" outputPtsOverlaps="+std::to_string(output_pts_overlaps)+" "+
                context_wait.text("contextWait")+scene.text("scene")+prepare.text("prepare")+submit.text("submit")+completion.text("completion")+
                shared_wait.text("sharedWait")+
                wait_submit.text("waitSubmit")+wait_completion.text("waitCompletion")+context_resume.text("contextResume")+
                "sharedWaitCalls="+std::to_string(shared_wait_calls)+" combinedScenePairs="+std::to_string(combined_scene_pairs)+" "+
                "queuedOutputFlushes="+std::to_string(queued_output_flushes)+" "+
                "eventWaitCalls="+std::to_string(event_wait_calls)+" queryWaitCalls="+std::to_string(query_wait_calls)+" "+
                "qualityCount="+std::to_string(quality_count.exchange(0))+" qualityFamine="+std::to_string(quality_famine.exchange(0))+
                " qualityLateSumMs="+std::to_string(quality_late_sum.exchange(0)/10000.0)+
                " qualityLateMaxMs="+std::to_string(quality_late_max.exchange(0)/10000.0)+
                " qualityProportionMin="+std::to_string(quality_proportion_min.exchange(1000)));
            window=Clock::now();clear();
        }catch(...){}
    }
};
}
