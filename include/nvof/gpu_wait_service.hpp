#pragma once
#include <windows.h>
#include <functional>

namespace nvof {
// The completion event is authoritative and consumes the first wait slot.
// Work notifications may coalesce or be stale; they never complete GPU work.
// The callback runs after device locks have been released, on the same thread.
inline DWORD wait_gpu_event(HANDLE completion,const std::function<HANDLE()>& service,DWORD timeout) {
    if(!service)return WaitForSingleObject(completion,timeout);
    const ULONGLONG deadline=GetTickCount64()+timeout;
    for(;;) {
        const DWORD ready=WaitForSingleObject(completion,0);
        if(ready!=WAIT_TIMEOUT)return ready;
        if(GetTickCount64()>=deadline)return WAIT_TIMEOUT;
        HANDLE work=service();
        const ULONGLONG now=GetTickCount64();
        HANDLE events[]={completion,work};
        const DWORD remaining=now<deadline?DWORD(deadline-now):0;
        const DWORD result=WaitForMultipleObjects(work?2:1,events,FALSE,remaining);
        if(result!=WAIT_OBJECT_0+1)return result;
    }
}
}
