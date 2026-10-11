#pragma once
#include <algorithm>
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
    PlaybackMetric receive,upstream_gap,capture,synthesis,allocator,delivery,status;
    uint64_t output=0;
    int64_t pts=0;
    void reset() noexcept {window=Clock::now();last_exit={};clear();}
    void clear() noexcept {receive={};upstream_gap={};capture={};synthesis={};allocator={};delivery={};status={};output=0;}
    template<class Write> void flush(bool force,Write write) noexcept {
        if(!enabled)return;
        try {
            const auto now=Clock::now();
            const double ms=std::chrono::duration<double,std::milli>(now-window).count();
            if((!force&&ms<1000)||!receive.count)return;
            write("performance elapsedMs="+std::to_string(ms)+" pts="+std::to_string(pts)+
                " delivered="+std::to_string(output)+" deliveredFps="+std::to_string(output*1000.0/ms)+" "+
                receive.text("receive")+upstream_gap.text("upstreamGap")+capture.text("capture")+
                synthesis.text("synthesis")+allocator.text("allocator")+delivery.text("delivery")+status.text("status"));
            window=Clock::now();clear();
        }catch(...){}
    }
};
}
