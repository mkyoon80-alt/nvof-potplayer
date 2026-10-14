#pragma once
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace nvof {
struct StatusSnapshot {
    uint64_t sequence=0;
    std::filesystem::path directory;
    std::string text;
};
namespace status_detail {
inline std::atomic<uint64_t> next_sequence{1};
inline std::mutex file_mutex;
inline uint64_t published_sequence=0;
inline void write_file(const StatusSnapshot& snapshot) {
    // A last-owner teardown can overlap acquisition of a new publisher. Keep
    // file replacement ordered even across that short-lived overlap.
    std::lock_guard<std::mutex> guard(file_mutex);
    if(snapshot.sequence<=published_sequence)return;
    std::error_code error;
    std::filesystem::create_directories(snapshot.directory,error);
    const auto destination=snapshot.directory/L"status.json";
    const auto temporary=snapshot.directory/(L"status."+std::to_wstring(GetCurrentProcessId())+L"."+
        std::to_wstring(GetCurrentThreadId())+L".tmp");
    {
        std::ofstream stream(temporary,std::ios::binary|std::ios::trunc);
        stream.write(snapshot.text.data(),std::streamsize(snapshot.text.size()));stream.flush();
        if(!stream)return;
    }
    if(MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        published_sequence=snapshot.sequence;
    else std::filesystem::remove(temporary,error);
}
}

// Optional telemetry only. One immutable in-flight snapshot and one newest
// pending snapshot; file I/O never holds the publication mutex. No filter,
// GPU resource, sample, or COM interface is accessed by this worker.
class StatusWriter {
public:
    using Sink=std::function<void(const StatusSnapshot&)>;
    explicit StatusWriter(Sink sink=status_detail::write_file):sink_(std::move(sink)),thread_([this]{run();}){}
    StatusWriter(const StatusWriter&)=delete;
    StatusWriter& operator=(const StatusWriter&)=delete;
    ~StatusWriter() {
        {std::lock_guard<std::mutex> guard(mutex_);stopping_=true;}
        changed_.notify_all();thread_.join();
    }
    void publish(std::filesystem::path directory,std::string text) {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if(stopping_)return;
            pending_=StatusSnapshot{status_detail::next_sequence.fetch_add(1),std::move(directory),std::move(text)};
        }
        changed_.notify_all();
    }
    // Used for state transitions/control teardown, never periodic playback
    // telemetry. It also keeps a completed Stop visible to status readers.
    void drain() {
        std::unique_lock<std::mutex> guard(mutex_);
        changed_.wait(guard,[this]{return !busy_&&!pending_;});
    }
private:
    void run() noexcept {
        for(;;) {
            StatusSnapshot snapshot;
            {
                std::unique_lock<std::mutex> guard(mutex_);
                changed_.wait(guard,[this]{return stopping_||pending_.has_value();});
                if(!pending_)break;
                snapshot=std::move(*pending_);pending_.reset();busy_=true;
            }
            try {sink_(snapshot);}catch(...){} // Diagnostics must not break playback.
            {std::lock_guard<std::mutex> guard(mutex_);busy_=false;}
            changed_.notify_all();
        }
    }
    Sink sink_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<StatusSnapshot> pending_;
    bool busy_=false,stopping_=false;
    std::thread thread_;
};

inline std::shared_ptr<StatusWriter> acquire_status_writer() noexcept {
    // Share one disk worker across filter probes/instances. Last owner joins
    // before its filter's COM lifetime ends, so DLL unload cannot race code.
    static std::mutex mutex;
    static std::weak_ptr<StatusWriter> shared;
    try {
        std::lock_guard<std::mutex> guard(mutex);
        auto result=shared.lock();
        if(!result){result=std::make_shared<StatusWriter>();shared=result;}
        return result;
    }catch(...){return {};}
}
}
