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
    Clock::time_point last_output{};
    // Renderer notifications can arrive on another thread. Observe them without
    // suppressing or changing upstream quality control, or taking stream locks.
    std::atomic<uint64_t> quality_count{0},quality_famine{0};
    std::atomic<int64_t> quality_late_sum{0},quality_late_max{0};
    std::atomic<int> quality_proportion_min{1000};
    uint64_t output=0;
    int64_t pts=0;
    void reset() noexcept {
        window=Clock::now();last_exit={};last_output={};clear();
        quality_count=0;quality_famine=0;quality_late_sum=0;quality_late_max=0;quality_proportion_min=1000;
    }
    void clear() noexcept {receive={};upstream_gap={};capture={};synthesis={};allocator={};delivery={};status={};output_gap={};context_wait={};scene={};prepare={};submit={};completion={};shared_wait={};output=0;}
    void note_output() noexcept {
        if(!enabled)return;
        const auto now=Clock::now();
        if(last_output!=Clock::time_point{})output_gap.add(std::chrono::duration<double,std::milli>(now-last_output).count());
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
                context_wait.text("contextWait")+scene.text("scene")+prepare.text("prepare")+submit.text("submit")+completion.text("completion")+
                shared_wait.text("sharedWait")+
                "qualityCount="+std::to_string(quality_count.exchange(0))+" qualityFamine="+std::to_string(quality_famine.exchange(0))+
                " qualityLateSumMs="+std::to_string(quality_late_sum.exchange(0)/10000.0)+
                " qualityLateMaxMs="+std::to_string(quality_late_max.exchange(0)/10000.0)+
                " qualityProportionMin="+std::to_string(quality_proportion_min.exchange(1000)));
            window=Clock::now();clear();
        }catch(...){}
    }
};
}
