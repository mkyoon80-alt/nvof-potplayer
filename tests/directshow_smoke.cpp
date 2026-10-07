#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <streams.h>
#include <dvdmedia.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
const CLSID kFilter = {0xedeca044, 0x78cd, 0x40eb, {0x8f, 0x37, 0x63, 0xd9, 0x47, 0xc5, 0x01, 0xa0}};
constexpr int kWidth = 640, kHeight = 360;
constexpr REFERENCE_TIME kDuration = 417083;
void check(HRESULT hr, const char* operation) {
    if (FAILED(hr)) throw std::runtime_error(std::string(operation) + " HRESULT=" + std::to_string(static_cast<unsigned long>(hr)));
}
CMediaType input_type() {
    CMediaType type;
    type.SetType(&MEDIATYPE_Video);
    type.SetSubtype(&MEDIASUBTYPE_NV12);
    type.SetFormatType(&FORMAT_VideoInfo2);
    type.SetTemporalCompression(FALSE);
    auto* video = reinterpret_cast<VIDEOINFOHEADER2*>(type.AllocFormatBuffer(sizeof(VIDEOINFOHEADER2)));
    if (!video) throw std::bad_alloc();
    *video = {};
    video->AvgTimePerFrame = kDuration;
    video->dwPictAspectRatioX = 16;
    video->dwPictAspectRatioY = 9;
    video->rcSource = {0, 0, kWidth, kHeight};
    video->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    video->bmiHeader.biWidth = kWidth;
    video->bmiHeader.biHeight = kHeight;
    video->bmiHeader.biPlanes = 1;
    video->bmiHeader.biBitCount = 12;
    video->bmiHeader.biCompression = MAKEFOURCC('N','V','1','2');
    video->bmiHeader.biSizeImage = kWidth*kHeight*3/2;
    type.SetSampleSize(video->bmiHeader.biSizeImage);
    return type;
}
class SourcePin final : public CBaseOutputPin {
public:
    SourcePin(CBaseFilter* owner, CCritSec* lock, HRESULT* hr)
        : CBaseOutputPin(NAME("Synthetic NV12 output"), owner, lock, hr, L"Output"), type_(input_type()) {}
    HRESULT GetMediaType(int index, CMediaType* type) override {
        if (index < 0) return E_INVALIDARG;
        return index == 0 ? type->Set(type_) : VFW_S_NO_MORE_ITEMS;
    }
    HRESULT CheckMediaType(const CMediaType* type) override {
        return type->majortype == MEDIATYPE_Video && type->subtype == MEDIASUBTYPE_NV12 ? S_OK : VFW_E_TYPE_NOT_ACCEPTED;
    }
    HRESULT DecideBufferSize(IMemAllocator* allocator, ALLOCATOR_PROPERTIES* properties) override {
        properties->cBuffers = 4;
        properties->cbBuffer = kWidth*kHeight*3/2;
        properties->cbAlign = 1;
        properties->cbPrefix = 0;
        ALLOCATOR_PROPERTIES actual{};
        return allocator->SetProperties(properties, &actual);
    }
    HRESULT push(int frame_index, bool discontinuity = false) {
        IMediaSample* raw = nullptr;
        HRESULT hr = GetDeliveryBuffer(&raw, nullptr, nullptr, 0);
        if (hr != S_OK) return hr;
        ComPtr<IMediaSample> sample;
        sample.Attach(raw);
        BYTE* data = nullptr;
        if (FAILED(hr=sample->GetPointer(&data))) return hr;
        // Deterministic moving rectangle with textured background.
        for (int y=0; y<kHeight; ++y) for (int x=0; x<kWidth; ++x)
            data[y*kWidth+x] = BYTE(40 + ((x/12+y/12)%2)*40);
        const int left = ((frame_index*9) % (kWidth-100) + (kWidth-100)) % (kWidth-100);
        for (int y=90; y<230; ++y) for (int x=left; x<left+100; ++x)
            data[y*kWidth+x] = 210;
        std::memset(data+kWidth*kHeight,128,kWidth*kHeight/2);
        REFERENCE_TIME start=frame_index*kDuration, stop=start+kDuration;
        sample->SetTime(&start,&stop);
        sample->SetActualDataLength(kWidth*kHeight*3/2);
        sample->SetDiscontinuity(discontinuity);
        sample->SetPreroll(FALSE);
        sample->SetSyncPoint(TRUE);
        return Deliver(sample.Get());
    }
private:
    CMediaType type_;
};
class Source final : public CBaseFilter {
public:
    Source(HRESULT* hr) : CBaseFilter(NAME("Synthetic source"),nullptr,&lock_,CLSID_NULL), pin_(this,&lock_,hr) {}
    int GetPinCount() override { return 1; }
    CBasePin* GetPin(int n) override { return n==0 ? &pin_ : nullptr; }
    SourcePin& pin() { return pin_; }
private:
    CCritSec lock_;
    SourcePin pin_;
};
class SinkPin final : public CBaseInputPin {
public:
    SinkPin(CBaseFilter* owner, CCritSec* lock, HRESULT* hr)
        : CBaseInputPin(NAME("Validation sink"),owner,lock,hr,L"Input") {
        unblock_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        entered_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!unblock_ || !entered_) *hr = E_OUTOFMEMORY;
    }
    ~SinkPin() override {
        if (unblock_) CloseHandle(unblock_);
        if (entered_) CloseHandle(entered_);
    }
    HRESULT CheckMediaType(const CMediaType* type) override {
        return type->majortype == MEDIATYPE_Video && type->subtype == MEDIASUBTYPE_NV12 ? S_OK : VFW_E_TYPE_NOT_ACCEPTED;
    }
    STDMETHODIMP Receive(IMediaSample* sample) override {
        HRESULT hr=CBaseInputPin::Receive(sample);
        if (hr != S_OK) return hr;
        if (block_.load()) {
            SetEvent(entered_);
            if (WaitForSingleObject(unblock_, 5000) != WAIT_OBJECT_0) return E_ABORT;
            return S_FALSE;
        }
        REFERENCE_TIME start=0,stop=0;
        hr=sample->GetTime(&start,&stop);
        if (hr != S_OK || stop<=start) return E_INVALIDARG;
        std::lock_guard<std::mutex> guard(results_lock_);
        if (!times.empty() && sample->IsDiscontinuity()!=S_OK && start<=times.back()) {
            valid=false;
            return E_UNEXPECTED;
        }
        times.push_back(start);
        return S_OK;
    }
    STDMETHODIMP BeginFlush() override {
        HRESULT hr=CBaseInputPin::BeginFlush();
        SetEvent(unblock_);
        return hr;
    }
    STDMETHODIMP EndFlush() override {
        block_=false;
        return CBaseInputPin::EndFlush();
    }
    HRESULT Inactive() override { SetEvent(unblock_); return CBaseInputPin::Inactive(); }
    void block_next() { ResetEvent(unblock_); ResetEvent(entered_); block_=true; }
    bool wait_entered() { return WaitForSingleObject(entered_,5000)==WAIT_OBJECT_0; }
    size_t count() { std::lock_guard<std::mutex> guard(results_lock_); return times.size(); }
    REFERENCE_TIME last() { std::lock_guard<std::mutex> guard(results_lock_); return times.empty() ? -1 : times.back(); }
    bool valid=true;
    std::vector<REFERENCE_TIME> times;
private:
    std::mutex results_lock_;
    std::atomic<bool> block_{false};
    HANDLE unblock_=nullptr, entered_=nullptr;
};
class Sink final : public CBaseFilter {
public:
    Sink(HRESULT* hr) : CBaseFilter(NAME("Validation sink"),nullptr,&lock_,CLSID_NULL), pin_(this,&lock_,hr) {}
    int GetPinCount() override { return 1; }
    CBasePin* GetPin(int n) override { return n==0 ? &pin_ : nullptr; }
    SinkPin& pin() { return pin_; }
private:
    CCritSec lock_;
    SinkPin pin_;
};

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { std::cerr << "usage: directshow_smoke <filter.ax> [--no-gpu]\n"; return 2; }
    const bool no_gpu=argc>2 && std::wstring(argv[2])==L"--no-gpu";
    HRESULT initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    if (FAILED(initialized)) return 3;
    HMODULE module=LoadLibraryExW(argv[1],nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) { std::cerr << "LoadLibrary failed " << GetLastError() << '\n'; CoUninitialize(); return 4; }
    int result=0;
    try {
        auto factory_function=reinterpret_cast<HRESULT(WINAPI*)(REFCLSID,REFIID,void**)>(GetProcAddress(module,"DllGetClassObject"));
        if (!factory_function) throw std::runtime_error("No COM factory export");
        ComPtr<IClassFactory> factory;
        check(factory_function(kFilter,IID_PPV_ARGS(&factory)),"factory");
        for (int iteration=0;iteration<3;++iteration) {
            ComPtr<IBaseFilter> filter;
            check(factory->CreateInstance(nullptr,IID_PPV_ARGS(&filter)),"create filter");
            ComPtr<ISpecifyPropertyPages> pages;
            check(filter.As(&pages),"property pages interface");
            CAUUID page_ids{};
            check(pages->GetPages(&page_ids),"property pages IDs");
            if (page_ids.cElems!=1 || !page_ids.pElems) throw std::runtime_error("Settings property page missing");
            ComPtr<IClassFactory> page_factory;
            const HRESULT page_factory_result=factory_function(page_ids.pElems[0],IID_PPV_ARGS(&page_factory));
            CoTaskMemFree(page_ids.pElems);
            check(page_factory_result,"property page factory");
            ComPtr<IPropertyPage> page;
            check(page_factory->CreateInstance(nullptr,IID_PPV_ARGS(&page)),"create property page");
            PROPPAGEINFO page_info{};
            page_info.cb=sizeof(page_info);
            check(page->GetPageInfo(&page_info),"property page resources");
            CoTaskMemFree(page_info.pszTitle);
            CoTaskMemFree(page_info.pszDocString);
            CoTaskMemFree(page_info.pszHelpFile);
            ComPtr<IGraphBuilder> graph;
            check(CoCreateInstance(CLSID_FilterGraph,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&graph)),"create graph");
            HRESULT status=S_OK;
            auto* source=new Source(&status);
            check(status,"source");
            auto* sink=new Sink(&status);
            check(status,"sink");
            ComPtr<IBaseFilter> source_ref=source;
            ComPtr<IBaseFilter> sink_ref=sink;
            check(graph->AddFilter(source_ref.Get(),L"Synthetic source"),"add source");
            check(graph->AddFilter(filter.Get(),L"NVIDIA interpolation"),"add interpolation");
            check(graph->AddFilter(sink_ref.Get(),L"Validation sink"),"add sink");
            ComPtr<IPin> input,output;
            check(filter->FindPin(L"In",&input),"find input");
            check(filter->FindPin(L"Out",&output),"find output");
            CMediaType media=input_type();
            check(graph->ConnectDirect(&source->pin(),input.Get(),&media),"connect input");
            check(graph->ConnectDirect(output.Get(),&sink->pin(),nullptr),"connect output");
            ComPtr<IMediaControl> control;
            check(graph.As(&control),"media control");
            check(control->Run(),"run");
            check(source->pin().push(-3,true),"negative preroll 1");
            check(source->pin().push(-2),"negative preroll 2");
            check(source->pin().push(-1),"negative preroll 3");
            if (sink->pin().count()!=0) throw std::runtime_error("Negative preroll was forwarded or retained");
            check(source->pin().push(0,true),"first frame");
            if (sink->pin().count()!=1) throw std::runtime_error("First-frame preview was not immediate");
            if (!no_gpu) {
                for (int i=1;i<10;++i) check(source->pin().push(i),"GPU frame");
                if (sink->pin().count()<20) throw std::runtime_error("Interpolation frame count too small");
            }
            // Backward and forward timeline changes clear every retained frame.
            for (int frame : {100,2,300,1}) {
                check(source->pin().DeliverBeginFlush(),"begin flush");
                check(source->pin().DeliverEndFlush(),"end flush");
                check(source->pin().DeliverNewSegment(frame*kDuration,INT64_MAX,1),"new segment");
                const size_t before=sink->pin().count();
                check(source->pin().push(frame,true),"seek preview");
                if (sink->pin().count()!=before+1 || sink->pin().last()!=frame*kDuration)
                    throw std::runtime_error("Stale frame delivered after seek");
                if (!no_gpu) check(source->pin().push(frame+1),"post-seek GPU pair");
            }
            // A downstream Receive deliberately blocks. BeginFlush must wake it
            // before waiting for the transform's streaming lock.
            check(source->pin().DeliverBeginFlush(),"pre-race begin flush");
            check(source->pin().DeliverEndFlush(),"pre-race end flush");
            sink->pin().block_next();
            std::atomic<HRESULT> push_status{E_PENDING};
            std::thread producer([&] { push_status=source->pin().push(500,true); });
            const bool entered=sink->pin().wait_entered();
            const auto start=std::chrono::steady_clock::now();
            const HRESULT flushed=source->pin().DeliverBeginFlush();
            producer.join();
            check(flushed,"concurrent flush");
            if (!entered || std::chrono::steady_clock::now()-start>std::chrono::seconds(2))
                throw std::runtime_error("Flush failed to unblock Receive promptly");
            check(source->pin().DeliverEndFlush(),"post-race end flush");
            check(source->pin().push(3,true),"after-race preview");
            check(source->pin().DeliverEndOfStream(),"EOS");
            // Test Stop on the transform directly, before the graph stops its
            // renderer. Decommitting the allocator alone cannot unblock this.
            check(source->pin().DeliverBeginFlush(),"pre-stop-race begin flush");
            check(source->pin().DeliverEndFlush(),"pre-stop-race end flush");
            sink->pin().block_next();
            std::thread stop_producer([&] { source->pin().push(700,true); });
            const bool stop_entered=sink->pin().wait_entered();
            const auto stop_start=std::chrono::steady_clock::now();
            const HRESULT stopped=filter->Stop();
            stop_producer.join();
            check(stopped,"direct transform stop");
            if (!stop_entered || std::chrono::steady_clock::now()-stop_start>std::chrono::seconds(2))
                throw std::runtime_error("Stop failed to unblock downstream Receive promptly");
            check(control->Stop(),"graph stop");
            if (!sink->pin().valid) throw std::runtime_error("Non-monotonic output timestamps");
            std::cout << "iteration=" << iteration << " output_samples=" << sink->pin().count()
                      << " property_page=OK preroll=OK first_frame=OK seeks=OK concurrent_flush=OK concurrent_stop=OK shutdown=OK\n";
        }
        std::cout << (no_gpu ? "PASS native DirectShow lifecycle (no GPU)\n" : "PASS native DirectShow + real FRUC lifecycle\n");
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        result=1;
    }
    auto can_unload=reinterpret_cast<HRESULT(WINAPI*)()>(GetProcAddress(module,"DllCanUnloadNow"));
    if (can_unload && can_unload()==S_OK) FreeLibrary(module);
    else { std::cerr << "Filter module still has live COM references\n"; result=1; }
    CoUninitialize();
    return result;
}


