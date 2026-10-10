#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <streams.h>
#include <dvdmedia.h>
#include <d3d10_1.h>
#include <d3d9.h>
#include <dxva2api.h>
#include <mfidl.h>
#include <mfapi.h>
#include <mferror.h>
#include <evr.h>
#include "nvof/d3d11_transport.hpp"
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
const CLSID kFilter = {0xedeca044, 0x78cd, 0x40eb, {0x8f, 0x37, 0x63, 0xd9, 0x47, 0xc5, 0x01, 0xa0}};
constexpr int kWidth = 640, kHeight = 360;
REFERENCE_TIME kDuration = 417083;
REFERENCE_TIME declared_duration=-1;
bool require_services=false;
bool p010_fixture=false;
int color_fixture=0; // 0 default, 1 legacy VideoInfo, 2 explicitly tagged VideoInfo2.
constexpr DWORD kMetadataTail=0x4e564f46;
DWORD explicit_color_flags() {
    DXVA2_ExtendedFormat color{};color.NominalRange=DXVA2_NominalRange_16_235;
    color.VideoTransferMatrix=DXVA2_VideoTransferMatrix_BT709;
    color.VideoPrimaries=DXVA2_VideoPrimaries_BT709;
    color.VideoTransferFunction=DXVA2_VideoTransFunc_709;
    color.VideoChromaSubsampling=DXVA2_VideoChromaSubsampling_MPEG2;
    color.VideoLighting=DXVA2_VideoLighting_dim;
    return color.value | AMCONTROL_USED | AMCONTROL_COLORINFO_PRESENT | AMCONTROL_PAD_TO_16x9;
}
ComPtr<IMFDXGIDeviceManager> retained_manager;
void check(HRESULT hr, const char* operation) {
    if (FAILED(hr)) throw std::runtime_error(std::string(operation) + " HRESULT=" + std::to_string(static_cast<unsigned long>(hr)));
}
void check_diagnostic_services(IUnknown* object) {
    ComPtr<IMFGetService> services;
    const HRESULT queried=object->QueryInterface(IID_PPV_ARGS(&services));
    check(queried,"native service contract enabled");
    void* exposed=reinterpret_cast<void*>(1);
    if(services->GetService(CLSID_NULL,IID_IUnknown,&exposed)!=E_NOINTERFACE || exposed)
        throw std::runtime_error("diagnostic facade exposed a service");
    if(services->GetService(CLSID_NULL,IID_IUnknown,nullptr)!=E_POINTER)
        throw std::runtime_error("diagnostic facade failed null validation");
}
CMediaType input_type() {
    CMediaType type;
    type.SetType(&MEDIATYPE_Video);
    type.SetSubtype(&MEDIASUBTYPE_NV12);
    type.SetFormatType(&FORMAT_VideoInfo2);
    type.SetTemporalCompression(FALSE);
    auto* video = reinterpret_cast<VIDEOINFOHEADER2*>(type.AllocFormatBuffer(sizeof(VIDEOINFOHEADER2)+(color_fixture?sizeof(DWORD):0)));
    if (!video) throw std::bad_alloc();
    *video = {};
    video->AvgTimePerFrame = declared_duration>=0?declared_duration:kDuration;
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
    if(color_fixture) {
        std::memcpy(type.pbFormat+sizeof(VIDEOINFOHEADER2),&kMetadataTail,sizeof(kMetadataTail));
        if(color_fixture==2) {
            video->dwControlFlags=explicit_color_flags();
            video->dwPictAspectRatioX=64;video->dwPictAspectRatioY=27;
        } else {
            const VIDEOINFOHEADER2 original=*video;
            auto* legacy=reinterpret_cast<VIDEOINFOHEADER*>(type.AllocFormatBuffer(sizeof(VIDEOINFOHEADER)+sizeof(DWORD)));
            *legacy={};legacy->rcSource=original.rcSource;legacy->rcTarget=original.rcTarget;
            legacy->dwBitRate=original.dwBitRate;legacy->dwBitErrorRate=original.dwBitErrorRate;
            legacy->AvgTimePerFrame=original.AvgTimePerFrame;legacy->bmiHeader=original.bmiHeader;
            std::memcpy(type.pbFormat+sizeof(VIDEOINFOHEADER),&kMetadataTail,sizeof(kMetadataTail));
            type.SetFormatType(&FORMAT_VideoInfo);
        }
    }
    return type;
}
// A DirectShow sample that exposes its DXGI buffer through the MF service
// contract, with no LAV TextureSample interface and no host pixel buffer.
class MfSampleWrapper final : public IMediaSample, public IMFGetService {
public:
    MfSampleWrapper(IMediaSample* original,ID3D11Texture2D* texture,UINT slice,bool broken=false)
        : original_(original),broken_(broken) {
        check(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D),texture,slice,FALSE,&buffer_),"MF DXGI sample buffer");
    }
    STDMETHODIMP QueryInterface(REFIID id,void** value) override {
        if(!value)return E_POINTER;*value=nullptr;
        if(id==IID_IUnknown || id==IID_IMediaSample)*value=static_cast<IMediaSample*>(this);
        else if(id==__uuidof(IMFGetService))*value=static_cast<IMFGetService*>(this);
        else return E_NOINTERFACE;
        AddRef();return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override {return ++refs_;}
    STDMETHODIMP_(ULONG) Release() override {const auto count=--refs_;if(!count)delete this;return count;}
    STDMETHODIMP GetService(REFGUID service,REFIID requested,LPVOID* value) override {
        if(!value)return E_POINTER;*value=nullptr;
        if(broken_ || service!=MR_BUFFER_SERVICE)return E_NOINTERFACE;
        return buffer_->QueryInterface(requested,value);
    }
    STDMETHODIMP GetPointer(BYTE** value) override {if(!value)return E_POINTER;*value=nullptr;return E_NOTIMPL;}
    STDMETHODIMP_(LONG) GetSize() override {return original_->GetSize();}
    STDMETHODIMP GetTime(REFERENCE_TIME* a,REFERENCE_TIME* b) override {return original_->GetTime(a,b);}
    STDMETHODIMP SetTime(REFERENCE_TIME* a,REFERENCE_TIME* b) override {return original_->SetTime(a,b);}
    STDMETHODIMP IsSyncPoint() override {return original_->IsSyncPoint();}
    STDMETHODIMP SetSyncPoint(BOOL v) override {return original_->SetSyncPoint(v);}
    STDMETHODIMP IsPreroll() override {return original_->IsPreroll();}
    STDMETHODIMP SetPreroll(BOOL v) override {return original_->SetPreroll(v);}
    STDMETHODIMP_(LONG) GetActualDataLength() override {return original_->GetActualDataLength();}
    STDMETHODIMP SetActualDataLength(LONG v) override {return original_->SetActualDataLength(v);}
    STDMETHODIMP GetMediaType(AM_MEDIA_TYPE** v) override {return original_->GetMediaType(v);}
    STDMETHODIMP SetMediaType(AM_MEDIA_TYPE* v) override {return original_->SetMediaType(v);}
    STDMETHODIMP IsDiscontinuity() override {return original_->IsDiscontinuity();}
    STDMETHODIMP SetDiscontinuity(BOOL v) override {return original_->SetDiscontinuity(v);}
    STDMETHODIMP GetMediaTime(LONGLONG* a,LONGLONG* b) override {return original_->GetMediaTime(a,b);}
    STDMETHODIMP SetMediaTime(LONGLONG* a,LONGLONG* b) override {return original_->SetMediaTime(a,b);}
private:
    std::atomic<ULONG> refs_{1};
    ComPtr<IMediaSample> original_;
    ComPtr<IMFMediaBuffer> buffer_;
    bool broken_=false;
};
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
    HRESULT DecideAllocator(IMemInputPin* receiver,IMemAllocator** allocator) override {
        HRESULT status=S_OK;
        auto* raw=new nvof::transport::SurfaceAllocator(&status);
        if(FAILED(status)){delete raw;return status;}
        ComPtr<IMemAllocator> allocated;
        check(raw->QueryInterface(IID_PPV_ARGS(&allocated)),"test source allocator");
        ALLOCATOR_PROPERTIES properties{};
        check(DecideBufferSize(allocated.Get(),&properties),"test source buffer size");
        check(receiver->NotifyAllocator(allocated.Get(),FALSE),"test source notify allocator");
        *allocator=allocated.Detach();return S_OK;
    }
    HRESULT renotify_allocator() {return m_pInputPin->NotifyAllocator(m_pAllocator,FALSE);}
    HRESULT set_gpu_mode(bool gpu) {
        auto* allocated=dynamic_cast<nvof::transport::SurfaceAllocator*>(m_pAllocator);
        return allocated ? allocated->set_gpu_mode(gpu) : E_UNEXPECTED;
    }
    HRESULT push_gpu(int index,ID3D11Device* device,ID3D11DeviceContext* context,bool discontinuity=false,bool mf=false,bool broken=false,bool no_stop=false,AM_MEDIA_TYPE* changed=nullptr) {
        IMediaSample* raw=nullptr;
        HRESULT result=GetDeliveryBuffer(&raw,nullptr,nullptr,0);
        if(result!=S_OK)return result;
        ComPtr<IMediaSample> sample;sample.Attach(raw);
        auto* surface=dynamic_cast<nvof::transport::SurfaceSample*>(raw);
        if(!surface)return E_UNEXPECTED;
        ComPtr<nvof::transport::TextureSample> before_assignment;
        check(sample.As(&before_assignment),"stable GPU sample interface before assignment");
        const int coded_height=kHeight+8;
        std::vector<BYTE> pixels(kWidth*coded_height*3/2,128);
        for(int y=0;y<coded_height;++y)for(int x=0;x<kWidth;++x)
            pixels[y*kWidth+x]=BYTE(40+((x/12+y/12)%2)*40);
        const int left=((index*9)%(kWidth-100)+(kWidth-100))%(kWidth-100);
        for(int y=90;y<230;++y)for(int x=left;x<left+100;++x)pixels[y*kWidth+x]=210;
        D3D11_TEXTURE2D_DESC description{};
        description.Width=kWidth;description.Height=coded_height;description.MipLevels=1;description.ArraySize=2;
        description.Format=p010_fixture?DXGI_FORMAT_P010:DXGI_FORMAT_NV12;description.SampleDesc.Count=1;
        description.Usage=D3D11_USAGE_DEFAULT;description.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> texture;
        check(device->CreateTexture2D(&description,nullptr,&texture),"synthetic decoder array texture");
        if(p010_fixture) {
            std::vector<uint16_t> words(pixels.size());
            for(size_t i=0;i<pixels.size();++i)words[i]=uint16_t(pixels[i])<<8;
            context->UpdateSubresource(texture.Get(),1,nullptr,words.data(),kWidth*2,0);
        } else context->UpdateSubresource(texture.Get(),1,nullptr,pixels.data(),kWidth,0);
        REFERENCE_TIME start=index*kDuration,stop=start+kDuration;
        surface->assign({texture,1,kWidth,kHeight,start});
        sample->SetTime(&start,no_stop?nullptr:&stop);sample->SetMediaType(changed);sample->SetActualDataLength(kWidth*kHeight*3/2);
        sample->SetDiscontinuity(discontinuity);sample->SetPreroll(FALSE);sample->SetSyncPoint(TRUE);
        if(mf) {
            ComPtr<IMediaSample> wrapped;wrapped.Attach(new MfSampleWrapper(sample.Get(),texture.Get(),1,broken));
            return Deliver(wrapped.Get());
        }
        return Deliver(sample.Get());
    }
    HRESULT push(int frame_index, bool discontinuity = false,bool no_stop=false,AM_MEDIA_TYPE* changed=nullptr) {
        IMediaSample* raw = nullptr;
        HRESULT hr = GetDeliveryBuffer(&raw, nullptr, nullptr, 0);
        if (hr != S_OK) return hr;
        ComPtr<IMediaSample> sample;
        sample.Attach(raw);
        ComPtr<IMediaSample2> properties_sample;check(sample.As(&properties_sample),"CPU sample properties contract");
        AM_SAMPLE2_PROPERTIES properties{};check(properties_sample->GetProperties(sizeof(properties),reinterpret_cast<BYTE*>(&properties)),"CPU sample properties");
        if(!properties.pbBuffer || properties.cbBuffer<kWidth*kHeight*3/2)return E_UNEXPECTED;
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
        sample->SetTime(&start,no_stop?nullptr:&stop);sample->SetMediaType(changed);
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
class SinkPin final : public CBaseInputPin, public nvof::transport::DecoderConfiguration {
public:
    DECLARE_IUNKNOWN;
    SinkPin(CBaseFilter* owner, CCritSec* lock, HRESULT* hr,bool advertise,bool reject)
        : CBaseInputPin(NAME("Validation sink"),owner,lock,hr,L"Input"),advertise_(advertise),reject_(reject) {
        unblock_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        entered_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!unblock_ || !entered_) *hr = E_OUTOFMEMORY;
    }
    STDMETHODIMP NonDelegatingQueryInterface(REFIID id,void** value) override {
        if(id==__uuidof(nvof::transport::DecoderConfiguration) && advertise_)
            return GetInterface(static_cast<nvof::transport::DecoderConfiguration*>(this),value);
        return CBaseInputPin::NonDelegatingQueryInterface(id,value);
    }
    STDMETHODIMP ActivateD3D11Decoding(ID3D11Device* device,ID3D11DeviceContext* context,HANDLE mutex,UINT) override {
        if(reject_)return E_FAIL;
        if(!device || !context)return E_POINTER;
        native_device_=device;native_context_=context;borrowed_mutex_=mutex;native_active_=true;return S_OK;
    }
    STDMETHODIMP_(UINT) GetD3D11AdapterIndex() override {return 0;}
    bool borrowed_mutex_valid()const {DWORD flags=0;return !borrowed_mutex_ || GetHandleInformation(borrowed_mutex_,&flags)!=FALSE;}
    void hold_all(){std::lock_guard<std::mutex> guard(results_lock_);hold_all_=true;}
    size_t gpu_count()const{return gpu_count_;}
    size_t cpu_count()const{return cpu_count_;}
    ~SinkPin() override {
        if (unblock_) CloseHandle(unblock_);
        if (entered_) CloseHandle(entered_);
    }
    HRESULT CheckMediaType(const CMediaType* type) override {
        return type->majortype == MEDIATYPE_Video && type->subtype == MEDIASUBTYPE_NV12 ? S_OK : VFW_E_TYPE_NOT_ACCEPTED;
    }
    STDMETHODIMP NotifyAllocator(IMemAllocator* allocator,BOOL read_only) override {
        HRESULT result=CBaseInputPin::NotifyAllocator(allocator,read_only);
        if(FAILED(result))return result;
        result=allocator->Commit();if(FAILED(result))return result;
        ComPtr<IMediaSample> sample;
        result=allocator->GetBuffer(&sample,nullptr,nullptr,0);
        if(SUCCEEDED(result)) {
            ComPtr<nvof::transport::TextureSample> texture;
            custom_allocator_=SUCCEEDED(sample.As(&texture));
            ComPtr<IMediaSample2> extended;
            if(SUCCEEDED(result=sample.As(&extended))) {
                AM_SAMPLE2_PROPERTIES properties{};
                result=extended->GetProperties(sizeof(properties),reinterpret_cast<BYTE*>(&properties));
                if(SUCCEEDED(result) && (custom_allocator_ ? properties.pbBuffer!=nullptr : properties.pbBuffer==nullptr))result=E_UNEXPECTED;
            }
        }
        sample.Reset();allocator->Decommit();return result;
    }
    STDMETHODIMP Receive(IMediaSample* sample) override {
        HRESULT hr=CBaseInputPin::Receive(sample);
        if (hr != S_OK) return hr;
        if (block_.load()) {
            SetEvent(entered_);
            if (WaitForSingleObject(unblock_, 5000) != WAIT_OBJECT_0) return E_ABORT;
            return S_FALSE;
        }
        ComPtr<nvof::transport::TextureSample> texture_sample;
        const HRESULT gpu=sample->QueryInterface(IID_PPV_ARGS(&texture_sample));
        if(native_active_) {
            if(!custom_allocator_)return E_UNEXPECTED;
            if(FAILED(gpu))return E_NOINTERFACE;
            ComPtr<ID3D11Texture2D> texture;UINT slice=0;
            hr=texture_sample->GetD3D11Texture(0,&texture,&slice);
            if(FAILED(hr))return hr;
            D3D11_TEXTURE2D_DESC description{};texture->GetDesc(&description);
            if(description.Format!=DXGI_FORMAT_NV12 || description.Width!=kWidth || description.Height!=kHeight || slice!=0)
                return E_UNEXPECTED;
            BYTE* forbidden=nullptr;
            if(sample->GetPointer(&forbidden)!=E_NOTIMPL || forbidden)return E_UNEXPECTED;
            ++gpu_count_;
        } else {
            BYTE* data=nullptr;
            if(custom_allocator_ || FAILED(sample->GetPointer(&data)) || !data || SUCCEEDED(gpu))return E_UNEXPECTED;
            ++cpu_count_;
        }
        REFERENCE_TIME start=0,stop=0;
        hr=sample->GetTime(&start,&stop);
        if (hr != S_OK || stop<=start) return E_INVALIDARG;
        std::lock_guard<std::mutex> guard(results_lock_);
        if (!times.empty() && sample->IsDiscontinuity()!=S_OK && start<=times.back()) {
            valid=false;
            return E_UNEXPECTED;
        }
        times.push_back(start);ends.push_back(stop);discontinuities.push_back(sample->IsDiscontinuity()==S_OK);
        if(hold_all_)retained_.emplace_back(sample);
        return S_OK;
    }
    STDMETHODIMP BeginFlush() override {
        HRESULT hr=CBaseInputPin::BeginFlush();
        {std::lock_guard<std::mutex> guard(results_lock_);hold_all_=false;retained_.clear();}
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
    std::vector<REFERENCE_TIME> times,ends;
    std::vector<bool> discontinuities;
private:
    std::mutex results_lock_;
    bool advertise_=true,reject_=false,native_active_=false,hold_all_=false,custom_allocator_=false;
    size_t gpu_count_=0,cpu_count_=0;
    ComPtr<ID3D11Device> native_device_;
    ComPtr<ID3D11DeviceContext> native_context_;
    std::vector<ComPtr<IMediaSample>> retained_;
    std::atomic<bool> block_{false};
    HANDLE unblock_=nullptr, entered_=nullptr,borrowed_mutex_=nullptr;
};
class Sink final : public CBaseFilter {
public:
    Sink(HRESULT* hr,bool advertise,bool reject) : CBaseFilter(NAME("Validation sink"),nullptr,&lock_,CLSID_NULL), pin_(this,&lock_,hr,advertise,reject) {}
    int GetPinCount() override { return 1; }
    CBasePin* GetPin(int n) override { return n==0 ? &pin_ : nullptr; }
    SinkPin& pin() { return pin_; }
private:
    CCritSec lock_;
    SinkPin pin_;
};

void run_case(IClassFactory* factory,ID3D11Device* device,ID3D11DeviceContext* context,
              bool use_gpu,bool activate_early,bool reject_renderer,bool advertise_renderer,bool mf_route=false,bool broken_sample=false,bool manager_only=false) {
    ComPtr<IBaseFilter> filter;check(factory->CreateInstance(nullptr,IID_PPV_ARGS(&filter)),"create filter");
    ComPtr<IGraphBuilder> graph;check(CoCreateInstance(CLSID_FilterGraph,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&graph)),"graph");
    HRESULT status=S_OK;
    auto* source=new Source(&status);check(status,"source");
    auto* sink=new Sink(&status,advertise_renderer,reject_renderer);check(status,"sink");
    ComPtr<IBaseFilter> source_ref=source,sink_ref=sink;
    check(graph->AddFilter(source_ref.Get(),L"Source"),"add source");
    check(graph->AddFilter(filter.Get(),L"Filter"),"add filter");
    check(graph->AddFilter(sink_ref.Get(),L"Sink"),"add sink");
    ComPtr<IPin> input,output;check(filter->FindPin(L"In",&input),"input");check(filter->FindPin(L"Out",&output),"output");
    check_diagnostic_services(filter.Get());check_diagnostic_services(input.Get());
    CMediaType media=input_type();check(graph->ConnectDirect(&source->pin(),input.Get(),&media),"connect input");
    ComPtr<nvof::transport::DecoderConfiguration> native;
    ComPtr<ID3D11Device> mf_device;ComPtr<ID3D11DeviceContext> mf_context;
    auto activate=[&]() -> HRESULT {
        if(!mf_route)return native->ActivateD3D11Decoding(device,context,nullptr,0);
        ComPtr<IMFGetService> services;check(input.As(&services),"MF input service");
        ComPtr<IMFDXGIDeviceManager> manager;
        HRESULT result=services->GetService(MR_VIDEO_ACCELERATION_SERVICE,IID_PPV_ARGS(&manager));
        if(FAILED(result))return result;
        HANDLE handle=nullptr;check(manager->OpenDeviceHandle(&handle),"MF manager handle");
        result=manager->LockDevice(handle,IID_PPV_ARGS(&mf_device),TRUE);
        if(SUCCEEDED(result))check(manager->UnlockDevice(handle,FALSE),"MF unlock");
        check(manager->CloseDeviceHandle(handle),"MF close handle");check(result,"MF manager device");
        mf_device->GetImmediateContext(&mf_context);device=mf_device.Get();context=mf_context.Get();
        ComPtr<IMFDXGIDeviceManager> repeated;
        check(services->GetService(MR_VIDEO_ACCELERATION_SERVICE,IID_PPV_ARGS(&repeated)),"MF repeated manager");
        if(manager.Get()!=repeated.Get())throw std::runtime_error("MF manager lifetime not stable");
        ComPtr<IMFGetService> filter_services;check(filter.As(&filter_services),"MF filter-level service");
        ComPtr<IMFDXGIDeviceManager> filter_manager;
        check(filter_services->GetService(MR_VIDEO_ACCELERATION_SERVICE,IID_PPV_ARGS(&filter_manager)),"MF filter-level manager");
        if(filter_manager.Get()!=manager.Get())throw std::runtime_error("pin and filter returned different MF managers");
        retained_manager=manager;
        ComPtr<IMemInputPin> memory_input;check(input.As(&memory_input),"native input allocator contract");
        ComPtr<IMemAllocator> ordinary;
        check(memory_input->GetAllocator(&ordinary),"manager discovery retains ordinary input allocator");
        if(manager_only)return S_OK;
        ComPtr<IDirectXVideoMemoryConfiguration> configuration;
        check(services->GetService(MR_VIDEO_ACCELERATION_SERVICE,IID_PPV_ARGS(&configuration)),"surface configuration service");
        DXVA2_SurfaceType surface=DXVA2_SurfaceType_ProcessorRenderTarget;
        check(configuration->GetAvailableSurfaceTypeByIndex(0,&surface),"surface type enumeration");
        if(surface!=DXVA2_SurfaceType_DecoderRenderTarget ||
           configuration->GetAvailableSurfaceTypeByIndex(1,&surface)!=MF_E_NO_MORE_TYPES ||
           configuration->GetAvailableSurfaceTypeByIndex(0,nullptr)!=E_POINTER)
            throw std::runtime_error("surface type enumeration contract mismatch");
        if(SUCCEEDED(configuration->SetSurfaceType(DXVA2_SurfaceType_ProcessorRenderTarget)))
            throw std::runtime_error("unsupported surface type was accepted");
        const HRESULT selected=configuration->SetSurfaceType(DXVA2_SurfaceType_DecoderRenderTarget);
        if(FAILED(selected))return selected;
        ordinary.Reset();
        if(memory_input->GetAllocator(&ordinary)!=E_NOTIMPL || ordinary)
            throw std::runtime_error("committed decoder must supply its own GPU allocator");
        check(source->pin().set_gpu_mode(true),"decoder GPU allocator mode");
        check(source->pin().renotify_allocator(),"decoder supplied GPU allocator");
        return S_OK;
    };
    if(use_gpu && !mf_route)check(input.As(&native),"native input contract; NativeD3D11=1 required");
    if((use_gpu || manager_only) && activate_early)check(activate(),"pending native activation");
    const HRESULT connected=graph->ConnectDirect(output.Get(),&sink->pin(),nullptr);
    if(!advertise_renderer || (use_gpu && activate_early && reject_renderer)) {
        if(SUCCEEDED(connected))throw std::runtime_error("Pending native mode accepted an incompatible renderer");
        std::cout<<"pending incompatible output rejected=OK cleanup=OK\n";return;
    }
    check(connected,"connect output");
    if((use_gpu || manager_only) && !activate_early) {
        const HRESULT activated=activate();
        if(reject_renderer) {
            if(SUCCEEDED(activated))throw std::runtime_error("Renderer activation rejection ignored");
            use_gpu=false;
        } else check(activated,"late native activation");
    }
    check(source->pin().set_gpu_mode(use_gpu),"source transport mode");
    ComPtr<IMediaControl> control;check(graph.As(&control),"control");check(control->Run(),"run");
    auto push=[&](int index,bool discontinuity=false){return use_gpu ? source->pin().push_gpu(index,device,context,discontinuity,mf_route,broken_sample) : source->pin().push(index,discontinuity);};
    if(broken_sample) {
        const HRESULT received=push(0,true);
        if(SUCCEEDED(received) || sink->pin().count()!=0)throw std::runtime_error("invalid MF texture sample was silently accepted");
        check(control->Stop(),"broken MF sample Stop");
        std::cout<<"invalid MF sample rejected without CPU fallback; shutdown=OK\n";return;
    }
    check(push(-2,true),"preroll");check(push(-1),"preroll");
    if(sink->pin().count()!=0)throw std::runtime_error("preroll leaked");
    check(push(0,true),"first frame");if(sink->pin().count()!=1)throw std::runtime_error("first frame delayed");
    for(int i=1;i<12;++i)check(push(i),"interpolated frame");
    for(int position:{100,2,400,1}) {
        check(source->pin().DeliverBeginFlush(),"seek flush");check(source->pin().DeliverEndFlush(),"seek end flush");
        check(source->pin().DeliverNewSegment(position*kDuration,INT64_MAX,1),"segment");
        check(push(position,true),"seek original");check(push(position+1),"seek next");
    }
    // Retain all renderer samples until the bounded output pool is exhausted.
    check(source->pin().DeliverBeginFlush(),"pool reset");check(source->pin().DeliverEndFlush(),"pool reset end");
    const size_t before=sink->pin().count();sink->pin().hold_all();
    std::thread producer([&] {push(1000,true);push(1001);push(1002);});
    for(int spin=0;spin<100 && sink->pin().count()<before+4;++spin)Sleep(20);
    const auto flush_start=std::chrono::steady_clock::now();
    const HRESULT flushed=source->pin().DeliverBeginFlush();producer.join();check(flushed,"exhausted-pool flush");
    if(std::chrono::steady_clock::now()-flush_start>std::chrono::seconds(2))throw std::runtime_error("GPU allocator cancellation timeout");
    check(source->pin().DeliverEndFlush(),"exhausted-pool end flush");check(push(1,true),"resume after pool cancellation");
    check(source->pin().DeliverEndOfStream(),"EOS");
    check(source->pin().DeliverBeginFlush(),"blocked-stop reset");check(source->pin().DeliverEndFlush(),"blocked-stop reset end");
    sink->pin().block_next();
    std::thread blocked_delivery([&]{push(4,true);});
    const bool entered=sink->pin().wait_entered();
    const auto stop_start=std::chrono::steady_clock::now();
    const HRESULT stopped=filter->Stop();blocked_delivery.join();check(stopped,"direct Stop during renderer Receive");
    if(!entered || std::chrono::steady_clock::now()-stop_start>std::chrono::seconds(2))
        throw std::runtime_error("blocked renderer Stop did not cancel promptly");
    check(control->Stop(),"graph Stop");
    check(control->Run(),"restart after direct Stop");
    const size_t before_stop=sink->pin().count();sink->pin().hold_all();
    std::thread pool_producer([&]{push(10,true);push(11);push(12);});
    for(int spin=0;spin<100 && sink->pin().count()<before_stop+4;++spin)Sleep(20);
    const auto pool_stop_start=std::chrono::steady_clock::now();
    const HRESULT pool_stopped=filter->Stop();pool_producer.join();check(pool_stopped,"direct Stop during allocator wait");
    if(std::chrono::steady_clock::now()-pool_stop_start>std::chrono::seconds(2))
        throw std::runtime_error("exhausted allocator Stop did not cancel promptly");
    check(control->Stop(),"final graph Stop");
    if(!sink->pin().valid || (use_gpu ? sink->pin().cpu_count()!=0 || sink->pin().gpu_count()==0 : sink->pin().gpu_count()!=0 || sink->pin().cpu_count()==0))
        throw std::runtime_error("mixed or invalid transport");
    std::cout<<"mode="<<(use_gpu?(mf_route?"MF-DXGI-GPU":"D3D11-GPU"):"CPU")<<" early="<<activate_early<<" reject="<<reject_renderer
        <<" samples="<<sink->pin().count()<<" timestamps=OK native-pointer-no-readback=OK pool-flush=OK blocked-stop=OK pool-stop=OK shutdown=OK\n";
    if(use_gpu) {
        check(graph->Disconnect(&source->pin()),"reconnect source disconnect");check(graph->Disconnect(input.Get()),"reconnect input disconnect");
        if(!sink->pin().borrowed_mutex_valid())throw std::runtime_error("renderer borrowed mutex closed by input disconnect");
        check(graph->Disconnect(output.Get()),"reconnect output disconnect");check(graph->Disconnect(&sink->pin()),"reconnect sink disconnect");
        check(graph->RemoveFilter(sink_ref.Get()),"remove old native renderer");
        auto* cpu_sink=new Sink(&status,true,false);check(status,"replacement CPU sink");ComPtr<IBaseFilter> cpu_sink_ref=cpu_sink;
        check(graph->AddFilter(cpu_sink_ref.Get(),L"Replacement renderer"),"add replacement renderer");
        check(graph->ConnectDirect(&source->pin(),input.Get(),&media),"CPU reconnect input");
        check(graph->ConnectDirect(output.Get(),&cpu_sink->pin(),nullptr),"CPU reconnect output");
        ComPtr<IMemInputPin> reconnected_input;check(input.As(&reconnected_input),"reconnected input memory");
        ComPtr<IMemAllocator> ordinary_reconnected;check(reconnected_input->GetAllocator(&ordinary_reconnected),"disconnect clears surface selection");
        check(source->pin().set_gpu_mode(false),"CPU reconnect source mode");check(control->Run(),"CPU reconnect run");
        check(source->pin().push(0,true),"CPU reconnect first");check(source->pin().push(1),"CPU reconnect interpolation");
        check(control->Stop(),"CPU reconnect Stop");
        if(cpu_sink->pin().cpu_count()==0 || cpu_sink->pin().gpu_count()!=0)throw std::runtime_error("native-to-CPU reconnect transport remained native");
        std::cout<<"same-instance native-to-CPU full graph reconnect=OK\n";
    }
}
void run_rate_policy_case(IClassFactory* factory,int64_t source_num,int64_t source_den,
    int64_t expected_num,int64_t expected_den,bool gpu,const std::string& reason,REFERENCE_TIME header_duration=-1) {
    if(source_num>0)kDuration=(10000000LL*source_den+source_num/2)/source_num;
    declared_duration=header_duration>=0?header_duration:source_num>0?kDuration:0;
    const REFERENCE_TIME expected_duration=expected_num>0?(10000000LL*expected_den+expected_num/2)/expected_num:0;
    const bool interpolate=reason=="selected";
    ComPtr<IBaseFilter> filter;check(factory->CreateInstance(nullptr,IID_PPV_ARGS(&filter)),"policy filter");
    ComPtr<IGraphBuilder> graph;check(CoCreateInstance(CLSID_FilterGraph,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&graph)),"policy graph");
    HRESULT status=S_OK;auto* source=new Source(&status);check(status,"policy source");
    auto* sink=new Sink(&status,true,false);check(status,"policy sink");
    ComPtr<IBaseFilter> source_ref=source,sink_ref=sink;
    check(graph->AddFilter(source_ref.Get(),L"Source"),"policy add source");
    check(graph->AddFilter(filter.Get(),L"Filter"),"policy add filter");
    check(graph->AddFilter(sink_ref.Get(),L"Sink"),"policy add sink");
    ComPtr<IPin> input,output;check(filter->FindPin(L"In",&input),"policy input");check(filter->FindPin(L"Out",&output),"policy output");
    CMediaType media=input_type();check(graph->ConnectDirect(&source->pin(),input.Get(),&media),"policy connect input");
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    ComPtr<IMFDXGIDeviceManager> manager;
    if(gpu) {
        ComPtr<IMFGetService> services;check(input.As(&services),"policy MF service");
        check(services->GetService(MR_VIDEO_ACCELERATION_SERVICE,IID_PPV_ARGS(&manager)),"policy manager");
        HANDLE handle=nullptr;check(manager->OpenDeviceHandle(&handle),"policy device handle");
        const HRESULT locked=manager->LockDevice(handle,IID_PPV_ARGS(&device),TRUE);
        if(SUCCEEDED(locked))check(manager->UnlockDevice(handle,FALSE),"policy unlock");
        check(manager->CloseDeviceHandle(handle),"policy close handle");check(locked,"policy device");
        device->GetImmediateContext(&context);
        ComPtr<IDirectXVideoMemoryConfiguration> configuration;
        check(services->GetService(MR_VIDEO_ACCELERATION_SERVICE,IID_PPV_ARGS(&configuration)),"policy surface configuration");
        check(configuration->SetSurfaceType(DXVA2_SurfaceType_DecoderRenderTarget),"policy surface selection");
        check(source->pin().set_gpu_mode(true),"policy decoder GPU allocator");check(source->pin().renotify_allocator(),"policy notify allocator");
    }
    check(graph->ConnectDirect(output.Get(),&sink->pin(),nullptr),"policy connect renderer");
    CMediaType negotiated;check(output->ConnectionMediaType(&negotiated),"policy output type");
    if(negotiated.formattype!=FORMAT_VideoInfo2 || negotiated.cbFormat<sizeof(VIDEOINFOHEADER2))
        throw std::runtime_error("native output lacks VIDEOINFOHEADER2 color contract");
    const auto* tagged=reinterpret_cast<const VIDEOINFOHEADER2*>(negotiated.pbFormat);
    DXVA2_ExtendedFormat color{};color.value=tagged->dwControlFlags;
    if((tagged->dwControlFlags&(AMCONTROL_USED|AMCONTROL_COLORINFO_PRESENT))!=(AMCONTROL_USED|AMCONTROL_COLORINFO_PRESENT) ||
       color.NominalRange!=DXVA2_NominalRange_16_235 || color.VideoTransferMatrix!=DXVA2_VideoTransferMatrix_BT709 ||
       color.VideoPrimaries!=DXVA2_VideoPrimaries_BT709 || color.VideoTransferFunction!=DXVA2_VideoTransFunc_709)
        throw std::runtime_error("negotiated output does not describe limited BT709 SDR");
    if(color_fixture) {
        DWORD tail=0;
        if(negotiated.cbFormat!=sizeof(VIDEOINFOHEADER2)+sizeof(DWORD))throw std::runtime_error("format tail size changed");
        std::memcpy(&tail,negotiated.pbFormat+sizeof(VIDEOINFOHEADER2),sizeof(tail));
        if(tail!=kMetadataTail)throw std::runtime_error("legacy bitmap payload tail lost");
        if(color_fixture==2 && (tagged->dwControlFlags!=explicit_color_flags() ||
            tagged->dwPictAspectRatioX!=64 || tagged->dwPictAspectRatioY!=27))
            throw std::runtime_error("explicit color/control/aspect metadata changed");
        if(color_fixture==1 && (tagged->dwPictAspectRatioX!=16 || tagged->dwPictAspectRatioY!=9))
            throw std::runtime_error("legacy display aspect changed during promotion");
        CMediaType untagged=negotiated;reinterpret_cast<VIDEOINFOHEADER2*>(untagged.pbFormat)->dwControlFlags=0;
        if(output->QueryAccept(&untagged)==S_OK)throw std::runtime_error("native output silently accepted missing color metadata");
    }
    const auto actual_duration=tagged->AvgTimePerFrame;
    if(std::llabs(actual_duration-expected_duration)>1)throw std::runtime_error("rate policy negotiated incorrect output FPS");
    ComPtr<IMediaControl> control;check(graph.As(&control),"policy control");check(control->Run(),"policy Run");
    wchar_t local[32768]{};GetEnvironmentVariableW(L"LOCALAPPDATA",local,_countof(local));
    std::ifstream state(std::filesystem::path(local)/L"NvofPotPlayer"/L"status.json");
    std::ostringstream captured;captured<<state.rdbuf();const auto json=captured.str();
    const std::string expected_reason=interpolate?"":reason;
    if(json.find("\"bypassReason\":\""+expected_reason+"\"")==std::string::npos ||
       json.find(interpolate?"\"state\":\"waiting\"":"\"state\":\"bypass\"")==std::string::npos)
        throw std::runtime_error("rate policy status reason/state mismatch");
    if(reason=="source-rate" && json.find("\"inputRateSelected\":false")==std::string::npos)
        throw std::runtime_error("rate policy selection status mismatch");
    auto push=[&](int index,bool no_stop=false,AM_MEDIA_TYPE* changed=nullptr) {
        return gpu?source->pin().push_gpu(index,device.Get(),context.Get(),false,true,false,no_stop,changed):
            source->pin().push(index,false,no_stop,changed);
    };
    const int frame_count=header_duration>=0?144:6;
    for(int i=0;i<frame_count;++i) {
        check(push(i),"policy source frame");
        if(i==0 && (sink->pin().count()!=1 || !sink->pin().discontinuities.back()))
            throw std::runtime_error("policy first original delayed or missing discontinuity");
        if(!interpolate && (sink->pin().count()!=size_t(i+1) || sink->pin().times.back()!=i*kDuration || sink->pin().ends.back()!=(i+1)*kDuration))
            throw std::runtime_error("bypass changed original frame count or timestamps");
    }
    if(interpolate && sink->pin().count()<=size_t(frame_count))throw std::runtime_error("selected source did not interpolate");
    check(push(frame_count,true),"policy missing stop timestamp");
    if(!interpolate && (sink->pin().count()!=size_t(frame_count+1) || sink->pin().ends.back()!=(frame_count+1)*kDuration))
        throw std::runtime_error("bypass synthesized invalid missing stop duration");
    if(interpolate) {
        for(size_t i=0;i<sink->pin().times.size();++i) {
            const int64_t tick=int64_t(i)*10000000LL*expected_den/expected_num;
            const int64_t next=int64_t(i+1)*10000000LL*expected_den/expected_num;
            if(sink->pin().times[i]!=tick || sink->pin().ends[i]!=next)
                throw std::runtime_error("negotiated target does not match delivered rational clock");
        }
    }
    const size_t before_eos=sink->pin().count();check(source->pin().DeliverEndOfStream(),"policy EOS");
    if(interpolate && header_duration<0 && expected_num*source_den%(expected_den*source_num)==0) {
        const int multiple=int(expected_num*source_den/(expected_den*source_num));
        if(sink->pin().count()!=size_t((frame_count+1)*multiple))throw std::runtime_error("Integer output count including EOS is incorrect");
    }
    if(!interpolate && sink->pin().count()!=before_eos)throw std::runtime_error("bypass emitted an interpolated EOS tail");
    check(source->pin().DeliverBeginFlush(),"policy seek flush");check(source->pin().DeliverEndFlush(),"policy seek end");
    check(source->pin().DeliverNewSegment(100*kDuration,INT64_MAX,1),"policy seek segment");
    const size_t before_seek=sink->pin().count();check(push(100),"policy seek original");
    if(sink->pin().count()!=before_seek+1 || sink->pin().times.back()!=100*kDuration || !sink->pin().discontinuities.back())
        throw std::runtime_error("policy seek did not deliver immediate clean original");
    // The tested 24-only mask must reject 24<->25 dynamic changes instead of
    // silently retaining incompatible output-FPS negotiation.
    if(!color_fixture && ((reason=="source-rate" && source_num==25 && source_den==1) ||
       (interpolate && (source_num==24 || source_num==24000)))) {
        CMediaType changed=input_type();
        reinterpret_cast<VIDEOINFOHEADER2*>(changed.pbFormat)->AvgTimePerFrame=
            reason=="source-rate"?416667:400000;
        if(SUCCEEDED(push(101,false,&changed)))throw std::runtime_error("rate-policy dynamic change accepted without renegotiation");
    }
    if(color_fixture) {
        CMediaType equivalent=negotiated;
        reinterpret_cast<VIDEOINFOHEADER2*>(equivalent.pbFormat)->AvgTimePerFrame=kDuration;
        check(push(101,false,&equivalent),"equivalent explicit color metadata accepted");
        const int saved=color_fixture;color_fixture=2;CMediaType changed=input_type();color_fixture=saved;
        auto* changed_header=reinterpret_cast<VIDEOINFOHEADER2*>(changed.pbFormat);
        DXVA2_ExtendedFormat changed_color{};changed_color.value=changed_header->dwControlFlags;
        changed_color.VideoChromaSubsampling=DXVA2_VideoChromaSubsampling_MPEG1;
        changed_header->dwControlFlags=changed_color.value;
        const HRESULT rejected=push(102,false,&changed);
        if(SUCCEEDED(rejected))throw std::runtime_error("dynamic color change retained stale output metadata");
        std::cout<<"equivalent metadata accepted; changed color rejected HRESULT="<<std::hex<<rejected<<std::dec<<'\n';
    }
    if(interpolate && source_num==10000000 && source_den==416999) {
        CMediaType changed=input_type();
        // 416999/2 and 417000/2 round to the same 100-ns duration, but
        // they are different exact output clocks and need renegotiation.
        reinterpret_cast<VIDEOINFOHEADER2*>(changed.pbFormat)->AvgTimePerFrame=417000;
        if(SUCCEEDED(push(101,false,&changed)))throw std::runtime_error("x2 rational clock changed without renegotiation");
    }
    check(control->Stop(),"policy Stop");
    if(gpu ? sink->pin().cpu_count()!=0 : sink->pin().gpu_count()!=0)throw std::runtime_error("rate policy changed frame transport");
    std::cout<<"PASS rate="<<source_num<<'/'<<source_den<<" output="<<expected_num<<'/'<<expected_den
        <<" mode="<<(gpu?"MF-DXGI-GPU":"CPU")<<" reason="<<reason
        <<" metadata="<<color_fixture<<" negotiated-FPS/timestamps/no-stop/EOS/seek/transport=OK\n";
}
void check_malformed_media(IClassFactory* factory) {
    const int saved=color_fixture;
    ComPtr<IBaseFilter> filter;check(factory->CreateInstance(nullptr,IID_PPV_ARGS(&filter)),"malformed filter");
    ComPtr<IPin> input;check(filter->FindPin(L"In",&input),"malformed input");
    for(int legacy=0;legacy<2;++legacy) {
        color_fixture=legacy?1:0;
        CMediaType truncated=input_type();
        truncated.cbFormat=(legacy?sizeof(VIDEOINFOHEADER):sizeof(VIDEOINFOHEADER2))-1;
        if(input->QueryAccept(&truncated)==S_OK)throw std::runtime_error("truncated video header accepted");
        CMediaType invalid_bitmap=input_type();
        auto* bitmap=legacy?&reinterpret_cast<VIDEOINFOHEADER*>(invalid_bitmap.pbFormat)->bmiHeader:
            &reinterpret_cast<VIDEOINFOHEADER2*>(invalid_bitmap.pbFormat)->bmiHeader;
        bitmap->biSize=1024;
        if(input->QueryAccept(&invalid_bitmap)==S_OK)throw std::runtime_error("oversized bitmap header accepted");
    }
    color_fixture=saved;
    std::cout<<"truncated VideoInfo/VideoInfo2 and oversized bitmap safely rejected=OK\n";
}
void check_explicit_metadata_preservation(IClassFactory* factory) {
    const int saved=color_fixture;color_fixture=2;
    HRESULT status=S_OK;auto* source=new Source(&status);check(status,"explicit source");ComPtr<IBaseFilter> source_ref=source;
    ComPtr<IBaseFilter> filter;check(factory->CreateInstance(nullptr,IID_PPV_ARGS(&filter)),"explicit filter");
    ComPtr<IGraphBuilder> graph;check(CoCreateInstance(CLSID_FilterGraph,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&graph)),"explicit graph");
    check(graph->AddFilter(source_ref.Get(),L"Source"),"explicit add source");check(graph->AddFilter(filter.Get(),L"Filter"),"explicit add filter");
    ComPtr<IPin> input,output;check(filter->FindPin(L"In",&input),"explicit input");check(filter->FindPin(L"Out",&output),"explicit output");
    CMediaType media=input_type();auto* header=reinterpret_cast<VIDEOINFOHEADER2*>(media.pbFormat);
    DXVA2_ExtendedFormat color{};color.value=header->dwControlFlags;
    color.NominalRange=DXVA2_NominalRange_0_255;color.VideoTransferMatrix=DXVA2_VideoTransferMatrix_BT601;
    color.VideoPrimaries=DXVA2_VideoPrimaries_SMPTE170M;
    header->dwControlFlags=color.value;
    check(graph->ConnectDirect(&source->pin(),input.Get(),&media),"explicit metadata connect");
    ComPtr<IEnumMediaTypes> types;check(output->EnumMediaTypes(&types),"explicit output enumeration");
    AM_MEDIA_TYPE* raw=nullptr;if(types->Next(1,&raw,nullptr)!=S_OK || !raw)throw std::runtime_error("missing explicit output type");
    CMediaType result(*raw);DeleteMediaType(raw);
    if(result.formattype!=FORMAT_VideoInfo2 || reinterpret_cast<VIDEOINFOHEADER2*>(result.pbFormat)->dwControlFlags!=color.value)
        throw std::runtime_error("explicit full-range/601 metadata was overwritten by defaults");
    color_fixture=saved;
    std::cout<<"explicit full-range/601 output metadata retained (no unsupported GPU activation)=OK\n";
}
void check_color_rejection(IClassFactory* factory,ID3D11Device* device,ID3D11DeviceContext* context) {
    for(int incompatible=0;incompatible<4;++incompatible) {
        ComPtr<IBaseFilter> filter;check(factory->CreateInstance(nullptr,IID_PPV_ARGS(&filter)),"color filter");
        ComPtr<IGraphBuilder> graph;check(CoCreateInstance(CLSID_FilterGraph,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&graph)),"color graph");
        HRESULT status=S_OK;auto* source=new Source(&status);check(status,"color source");ComPtr<IBaseFilter> source_ref=source;
        check(graph->AddFilter(source_ref.Get(),L"Source"),"color add source");check(graph->AddFilter(filter.Get(),L"Filter"),"color add filter");
        ComPtr<IPin> input;check(filter->FindPin(L"In",&input),"color input");
        CMediaType media=input_type();auto* header=reinterpret_cast<VIDEOINFOHEADER2*>(media.pbFormat);
        DXVA2_ExtendedFormat color{};color.NominalRange=DXVA2_NominalRange_16_235;
        color.VideoTransferMatrix=DXVA2_VideoTransferMatrix_BT709;color.VideoPrimaries=DXVA2_VideoPrimaries_BT709;
        color.VideoTransferFunction=DXVA2_VideoTransFunc_709;
        if(incompatible==0)color.NominalRange=DXVA2_NominalRange_0_255;
        if(incompatible==1)color.VideoTransferMatrix=DXVA2_VideoTransferMatrix_BT601;
        if(incompatible==2)color.VideoPrimaries=DXVA2_VideoPrimaries_SMPTE170M;
        if(incompatible==3)color.VideoTransferFunction=16; // PQ ST2084 modern DXVA extension.
        header->dwControlFlags=color.value|AMCONTROL_COLORINFO_PRESENT;
        check(graph->ConnectDirect(&source->pin(),input.Get(),&media),"color connect");
        ComPtr<nvof::transport::DecoderConfiguration> native;check(input.As(&native),"color native contract");
        if(native->ActivateD3D11Decoding(device,context,nullptr,0)!=VFW_E_TYPE_NOT_ACCEPTED)
            throw std::runtime_error("unsupported native color metadata was accepted");
    }
    std::cout<<"explicit full-range/601/primaries/PQ color rejection=OK\n";
}
int wmain(int argc,wchar_t** argv) {
    if(argc<2){std::cerr<<"usage: directshow_gpu_smoke <NativeD3D11=1 filter.ax>\n";return 2;}
    p010_fixture=(argc>9 && wcscmp(argv[9],L"p010")==0) || (argc>2 && (wcscmp(argv[2],L"--p010-input")==0 || wcscmp(argv[2],L"--rounded-duration-p010")==0));
    require_services=argc>2 && wcscmp(argv[2],L"--probe-services")==0;
    check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"CoInitialize");
    HMODULE module=LoadLibraryExW(argv[1],nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
    if(!module){std::cerr<<"load failed "<<GetLastError()<<'\n';return 3;}
    int result=0;
    try {
        auto get_factory=reinterpret_cast<HRESULT(WINAPI*)(REFCLSID,REFIID,void**)>(GetProcAddress(module,"DllGetClassObject"));
        ComPtr<IClassFactory> factory;check(get_factory(kFilter,IID_PPV_ARGS(&factory)),"factory");
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
        check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_VIDEO_SUPPORT|D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"D3D11 device");
        ComPtr<ID3D10Multithread> multithread;
        if(SUCCEEDED(context.As(&multithread)))multithread->SetMultithreadProtected(TRUE);
        if(argc>2 && (wcscmp(argv[2],L"--rounded-duration")==0 || wcscmp(argv[2],L"--rounded-duration-p010")==0)) {
            run_rate_policy_case(factory.Get(),24000,1001,1000,21,true,"selected",420000);
            std::cout<<"PASS 42ms header with 23.976 PTS through MF GPU transport; P010="<<p010_fixture<<std::endl;
        } else if(argc>=9 && (wcscmp(argv[2],L"--rate-policy")==0 || wcscmp(argv[2],L"--metadata-legacy")==0 || wcscmp(argv[2],L"--metadata-tagged")==0)) {
            color_fixture=wcscmp(argv[2],L"--metadata-legacy")==0?1:wcscmp(argv[2],L"--metadata-tagged")==0?2:0;
            if(color_fixture){check_malformed_media(factory.Get());check_explicit_metadata_preservation(factory.Get());}
            const std::wstring wide_reason=argv[8];
            run_rate_policy_case(factory.Get(),_wtoi64(argv[3]),_wtoi64(argv[4]),_wtoi64(argv[5]),_wtoi64(argv[6]),
                wcscmp(argv[7],L"mf")==0,std::string(wide_reason.begin(),wide_reason.end()));
        } else {
        check_color_rejection(factory.Get(),device.Get(),context.Get());
        run_case(factory.Get(),device.Get(),context.Get(),false,false,false,true); // Native renderer + CPU decoder.
        run_case(factory.Get(),device.Get(),context.Get(),true,false,true,true);   // Rejected activation + CPU fallback.
        run_case(factory.Get(),device.Get(),context.Get(),true,false,false,true);  // Renderer connected first.
        run_case(factory.Get(),device.Get(),context.Get(),true,true,false,true);   // Decoder activation first.
        run_case(factory.Get(),device.Get(),context.Get(),true,true,true,true);    // Pending activation rejected.
        run_case(factory.Get(),device.Get(),context.Get(),false,false,false,false); // Refuse D3D9 even if decoder stayed CPU.
        run_case(factory.Get(),device.Get(),context.Get(),true,true,false,false);  // Renderer lacks native contract.
        run_case(factory.Get(),device.Get(),context.Get(),false,true,false,true,true,false,true); // Manager probe only, before output.
        run_case(factory.Get(),device.Get(),context.Get(),false,false,false,true,true,false,true); // Manager probe only, after output.
        run_case(factory.Get(),device.Get(),context.Get(),true,false,true,true,true); // MF surface activation rejection restores CPU.
        run_case(factory.Get(),device.Get(),context.Get(),true,false,false,true,true);
        run_case(factory.Get(),device.Get(),context.Get(),true,true,false,true,true);
        run_case(factory.Get(),device.Get(),context.Get(),true,true,false,true,true,true);
        std::cout<<"PASS DirectShow native D3D11/MF negotiation/transport/lifetime regression\n";
        }
    } catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';result=1;}
    auto unload=reinterpret_cast<HRESULT(WINAPI*)()>(GetProcAddress(module,"DllCanUnloadNow"));
    if(retained_manager) {
        if(!unload || unload()!=S_FALSE){std::cerr<<"MF manager did not prevent premature DLL unload\n";result=1;}
        retained_manager.Reset();
    }
    if(unload && unload()==S_OK)FreeLibrary(module);else{std::cerr<<"live COM references\n";result=1;}
    CoUninitialize();return result;
}
