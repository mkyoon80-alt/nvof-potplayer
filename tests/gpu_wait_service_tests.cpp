#include "nvof/gpu_wait_service.hpp"
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
struct Event {
    HANDLE value=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    Event(){if(!value)throw std::runtime_error("CreateEvent failed");}
    ~Event(){CloseHandle(value);}
    void signal(){SetEvent(value);}
};
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){try {
    Event completed,returned,serviced;
    int calls=0;
    completed.signal();returned.signal();
    require(nvof::wait_gpu_event(completed.value,[&]{++calls;return returned.value;},100)==WAIT_OBJECT_0&&calls==0,
        "Ready completion must win without servicing stale work");
    ResetEvent(returned.value);
    std::atomic<DWORD> result{WAIT_FAILED};std::atomic<int> count{0};
    std::thread worker([&]{result=nvof::wait_gpu_event(completed.value,[&]{++count;serviced.signal();return returned.value;},2000);});
    bool okay=WaitForSingleObject(serviced.value,1000)==WAIT_OBJECT_0;
    for(int i=0;i<3;++i){returned.signal();okay=okay&&WaitForSingleObject(serviced.value,1000)==WAIT_OBJECT_0;okay=okay&&result==WAIT_FAILED;}
    completed.signal();worker.join();
    require(okay&&result==WAIT_OBJECT_0&&count==4,"Return hints must service work without ending GPU completion wait");
    calls=0;returned.signal();
    require(nvof::wait_gpu_event(completed.value,[&]{++calls;return returned.value;},25)==WAIT_TIMEOUT&&calls==2,
        "Stale auto-reset hint must not spin or bypass completion timeout");
    calls=0;
    require(nvof::wait_gpu_event(completed.value,[&]{++calls;return HANDLE(nullptr);},25)==WAIT_TIMEOUT&&calls==1,
        "Empty or paused output must block solely on GPU completion");
    completed.signal();require(nvof::wait_gpu_event(completed.value,{},0)==WAIT_OBJECT_0,"Plain completion path changed");
    std::cout<<"PASS completion priority, repeated work hints, no premature completion, stale hint/no-work timeout\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
