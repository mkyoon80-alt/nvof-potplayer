#include "nvof/video_format.hpp"
#include "nvof/playback_limits.hpp"
#include "hevc_color_fixture.hpp"
#include "av1_color_fixture.hpp"
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
#include "gpu_completion_gate.hpp"
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
const CLSID kFilter = {0xedeca044, 0x78cd, 0x40eb, {0x8f, 0x37, 0x63, 0xd9, 0x47, 0xc5, 0x01, 0xa0}};
int kWidth = 640, kHeight = 360;
bool av1_fixture_header=false;
int av1_format=0;
std::wstring vp9_case;
REFERENCE_TIME kDuration = 417083;
REFERENCE_TIME declared_duration=-1;
bool require_services=false;
bool cpu_gpu_output=false;
bool p010_fixture=false;
bool p010_media=false;
bool reject_p010_media=false;
unsigned hdr_transfer=0;
bool dovi_fixture=false;
bool async_lifecycle=false;
bool stream_bench=false;
bool cached_stream=false;
bool cached_seek=false;
bool preview_separation=false;
bool preroll_prepare=false,output_ahead=false,wait_output=false,wait_output_error=false;
uint64_t output_digest=14695981039346656037ULL;
std::function<void(IMediaSample*,ID3D11Device*,ID3D11DeviceContext*)> inspect_gpu_output;
unsigned recover_transfer=0;
int color_fixture=0; // 0 default, 1 legacy VideoInfo, 2 explicitly tagged VideoInfo2.
constexpr DWORD kMetadataTail=0x4e564f46;
DWORD explicit_color_flags() {
    DXVA2_ExtendedFormat color{};color.NominalRange=DXVA2_NominalRange_16_235;
    color.VideoTransferMatrix=DXVA2_VideoTransferMatrix_BT709;
    color.VideoPrimaries=DXVA2_VideoPrimaries_BT709;
    color.VideoTransferFunction=DXVA2_VideoTransFunc_709;
    if(hdr_transfer){color.VideoPrimaries=MFVideoPrimaries_BT2020;color.VideoTransferMatrix=MFVideoTransferMatrix_BT2020_10;color.VideoTransferFunction=hdr_transfer;}
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
    type.SetSubtype(p010_media?&nvof::p010_subtype:&MEDIASUBTYPE_NV12);
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
    video->bmiHeader.biBitCount = p010_media?24:12;
    video->bmiHeader.biCompression = p010_media?MAKEFOURCC('P','0','1','0'):MAKEFOURCC('N','V','1','2');
    video->bmiHeader.biSizeImage = kWidth*kHeight*3/(p010_media?1:2);
    if(hdr_transfer && !recover_transfer)video->dwControlFlags=explicit_color_flags();
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
        if(id==__uuidof(nvof::MediaSideData))return original_->QueryInterface(id,value);
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
        if(reject_p010_media && type->subtype==nvof::p010_subtype)return VFW_E_TYPE_NOT_ACCEPTED;
        return type->majortype == MEDIATYPE_Video && (type->subtype == MEDIASUBTYPE_NV12 || type->subtype==nvof::p010_subtype) ? S_OK : VFW_E_TYPE_NOT_ACCEPTED;
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
        // Avoid CPU construction/upload starving the input worker in the
        // completion-gate test. Two immutable alternating GPU inputs model a
        // decoder that already has pictures ready; no slice is overwritten.
        ComPtr<ID3D11Texture2D> texture=(wait_output||cached_stream||cached_seek)?gate_sources_[index&1]:nullptr;
        if(!texture) {
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
        check(device->CreateTexture2D(&description,nullptr,&texture),"synthetic decoder array texture");
        if(p010_fixture) {
            std::vector<uint16_t> words(pixels.size());
            for(size_t i=0;i<pixels.size();++i)words[i]=uint16_t(pixels[i])<<8;
            context->UpdateSubresource(texture.Get(),1,nullptr,words.data(),kWidth*2,0);
        } else context->UpdateSubresource(texture.Get(),1,nullptr,pixels.data(),kWidth,0);
        if(wait_output||cached_stream||cached_seek)gate_sources_[index&1]=texture;
        }
        REFERENCE_TIME start=index*kDuration,stop=start+kDuration;
        surface->assign({texture,1,kWidth,kHeight,start});
        if(hdr_transfer==MFVideoTransFunc_2084) {
            const double mastering[10]={.265,.15,.68,.69,.06,.32,.3127,.329,1000,.005};
            const unsigned light[2]={1000,400};
            check(surface->SetSideData(nvof::hdr_mastering_id,reinterpret_cast<const BYTE*>(mastering),sizeof(mastering)),"source HDR mastering");
            check(surface->SetSideData(nvof::hdr_light_id,reinterpret_cast<const BYTE*>(light),sizeof(light)),"source HDR light");
        }
        if(dovi_fixture)for(int i=2;i<5;++i) {
            std::vector<BYTE> payload(i==2?128:i==3?5225:9353,BYTE(index));
            check(surface->SetSideData(nvof::hdr_ids[i],payload.data(),payload.size()),"source DV side data");
        }
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
    ComPtr<ID3D11Texture2D> gate_sources_[2];
};
// Models a decoder whose raw output is untagged but its compressed input
// retains codec/container colour information.
class EncodedInfoPin final : public CBaseInputPin {
public:
    EncodedInfoPin(CBaseFilter* owner,CCritSec* lock,HRESULT* hr):CBaseInputPin(NAME("Compressed header"),owner,lock,hr,L"Encoded"){}
    HRESULT CheckMediaType(const CMediaType*) override {return S_OK;}
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* media) override {
        if(!media)return E_POINTER;if(!recover_transfer)return VFW_E_NOT_CONNECTED;
        auto seq=av1_fixture_header?(recover_transfer==18?av1_fixture::hlg:av1_fixture::pq):hevc_fixture::hvcc(recover_transfer);
        CMediaType type;type.SetType(&MEDIATYPE_Video);
        const GUID hvc1={MAKEFOURCC('H','V','C','1'),0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
        GUID codec=hvc1;if(av1_fixture_header)codec.Data1=MAKEFOURCC('A','V','0','1');
        if(!vp9_case.empty()) {
            codec.Data1=MAKEFOURCC('V','P','9','0');
            seq={'v','p','c','C',1,0,0,0,2,51,0xa2,9,uint8_t(recover_transfer),9,0,0};
            if(vp9_case.find(L"truncated")!=std::wstring::npos)seq.resize(15);
            if(vp9_case.find(L"full-range")!=std::wstring::npos)seq[10]|=1;
            if(vp9_case.find(L"unspecified")!=std::wstring::npos)seq[12]=2;
            if(vp9_case.find(L"10bit-sdr")!=std::wstring::npos)seq[11]=seq[12]=seq[13]=1;
            if(vp9_case.find(L"missing")!=std::wstring::npos || vp9_case.find(L"flags-only")!=std::wstring::npos)seq.clear();
            if(vp9_case.find(L"box")!=std::wstring::npos)seq.insert(seq.begin(),{0,0,0,20});
            if(vp9_case.find(L"payload")!=std::wstring::npos)seq.erase(seq.begin(),seq.begin()+4);
        }
        type.SetSubtype(&codec);
        if((av1_fixture_header || !vp9_case.empty()) && av1_format) {
            const size_t header=av1_format==1?sizeof(VIDEOINFOHEADER):sizeof(VIDEOINFOHEADER2);
            auto* data=type.AllocFormatBuffer(ULONG(header+seq.size()));std::memset(data,0,header);
            if(!seq.empty())std::memcpy(data+header,seq.data(),seq.size());
            if(av1_format==2 && (vp9_case.find(L"flags-only")!=std::wstring::npos || vp9_case.find(L"conflict")!=std::wstring::npos)) {
                DXVA2_ExtendedFormat flags{};flags.NominalRange=DXVA2_NominalRange_16_235;
                flags.VideoPrimaries=MFVideoPrimaries_BT2020;flags.VideoTransferMatrix=MFVideoTransferMatrix_BT2020_10;
                flags.VideoTransferFunction=vp9_case.find(L"conflict")!=std::wstring::npos?MFVideoTransFunc_HLG:hdr_transfer;
                reinterpret_cast<VIDEOINFOHEADER2*>(data)->dwControlFlags=flags.value|AMCONTROL_USED|AMCONTROL_COLORINFO_PRESENT;
            }
            type.SetFormatType(av1_format==1?&FORMAT_VideoInfo:&FORMAT_VideoInfo2);return CopyMediaType(media,&type);
        }
        type.SetFormatType(&FORMAT_MPEG2Video);
        const size_t size=offsetof(MPEG2VIDEOINFO,dwSequenceHeader)+seq.size();
        auto* m=reinterpret_cast<MPEG2VIDEOINFO*>(type.AllocFormatBuffer(ULONG(size)));if(!m)return E_OUTOFMEMORY;
        std::memset(m,0,size);m->cbSequenceHeader=DWORD(seq.size());std::memcpy(m->dwSequenceHeader,seq.data(),seq.size());
        return CopyMediaType(media,&type);
    }
};
class Source final : public CBaseFilter {
public:
    Source(HRESULT* hr) : CBaseFilter(NAME("Synthetic source"),nullptr,&lock_,CLSID_NULL), pin_(this,&lock_,hr), encoded_(this,&lock_,hr) {}
    int GetPinCount() override { return recover_transfer?2:1; }
    CBasePin* GetPin(int n) override { return n==0 ? static_cast<CBasePin*>(&pin_) : n==1 && recover_transfer ? &encoded_ : nullptr; }
    SourcePin& pin() { return pin_; }
private:
    CCritSec lock_;
    SourcePin pin_;
    EncodedInfoPin encoded_;
};
UINT renderer_adapter_hint=0;
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
    STDMETHODIMP_(UINT) GetD3D11AdapterIndex() override {return renderer_adapter_hint;}
    bool borrowed_mutex_valid()const {DWORD flags=0;return !borrowed_mutex_ || GetHandleInformation(borrowed_mutex_,&flags)!=FALSE;}
    void hold_all(){std::lock_guard<std::mutex> guard(results_lock_);hold_all_=true;}
    void release_all(){std::lock_guard<std::mutex> guard(results_lock_);hold_all_=false;retained_.clear();}
    void release_one(){std::lock_guard<std::mutex> guard(results_lock_);if(!retained_.empty())retained_.erase(retained_.begin());}
    long allocator_capacity()const{return allocator_capacity_;}
    size_t gpu_count()const{return gpu_count_;}
    size_t cpu_count()const{return cpu_count_;}
    void require_native_allocator(){require_native_allocator_=true;}
    ~SinkPin() override {
        if (unblock_) CloseHandle(unblock_);
        if (entered_) CloseHandle(entered_);
    }
    HRESULT CheckMediaType(const CMediaType* type) override {
        return type->majortype == MEDIATYPE_Video && (type->subtype == MEDIASUBTYPE_NV12 || type->subtype==nvof::p010_subtype) ? S_OK : VFW_E_TYPE_NOT_ACCEPTED;
    }
    STDMETHODIMP NotifyAllocator(IMemAllocator* allocator,BOOL read_only) override {
        // PotPlayer's native renderer needs its device before accepting the
        // first allocator, not just before Run/Receive.
        if(require_native_allocator_ && !native_active_)return VFW_E_NO_ALLOCATOR;
        HRESULT result=CBaseInputPin::NotifyAllocator(allocator,read_only);
        if(FAILED(result))return result;
        ALLOCATOR_PROPERTIES allocation{};
        result=allocator->GetProperties(&allocation);if(FAILED(result))return result;
        allocator_capacity_=allocation.cBuffers;
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
        const HRESULT injected=next_receive_result_.exchange(S_OK);
        if(injected!=S_OK)return injected;
        // Base Receive validates a dynamic type; a concrete renderer adopts it.
        if (m_SampleProps.dwSampleFlags & AM_SAMPLE_TYPECHANGED) {
            hr=SetMediaType(reinterpret_cast<const CMediaType*>(m_SampleProps.pMediaType));
            if(FAILED(hr))return hr;
        }
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
            if(description.Format!=(p010_fixture?DXGI_FORMAT_P010:DXGI_FORMAT_NV12) || description.Width!=kWidth || description.Height!=kHeight || slice!=0)
                return E_UNEXPECTED;
            BYTE* forbidden=nullptr;
            if(sample->GetPointer(&forbidden)!=E_NOTIMPL || forbidden)return E_UNEXPECTED;
            if(m_mt.subtype!=(p010_fixture?nvof::p010_subtype:MEDIASUBTYPE_NV12))return E_UNEXPECTED;
            if(sample->GetActualDataLength()!=kWidth*kHeight*3/(p010_fixture?1:2))return E_UNEXPECTED;
            auto metadata=nvof::capture_hdr(sample);
            if(hdr_transfer==MFVideoTransFunc_2084) {
                const double mastering[10]={.265,.15,.68,.69,.06,.32,.3127,.329,1000,.005};const unsigned light[2]={1000,400};
                if(!metadata || metadata->data[0].size()!=sizeof(mastering) || metadata->data[1].size()!=sizeof(light) ||
                   memcmp(metadata->data[0].data(),mastering,sizeof(mastering)) || memcmp(metadata->data[1].data(),light,sizeof(light)))return E_UNEXPECTED;
            } else if(metadata)return E_UNEXPECTED;
            if(dovi_fixture) {
                REFERENCE_TIME pts=0,end=0;check(sample->GetTime(&pts,&end),"DV output time");
                // A generated frame uses the preceding original's metadata,
                // never a blend of RPU bytes or a stale global cached payload.
                const BYTE expected=BYTE((pts+10000)/kDuration);
                for(int i=2;i<5;++i) {
                    const size_t length=i==2?128:i==3?5225:9353;
                    if(!metadata || metadata->data[i].size()!=length ||
                       !std::all_of(metadata->data[i].begin(),metadata->data[i].end(),[&](BYTE b){return b==expected;}))return E_UNEXPECTED;
                }
            }
            if(inspect_gpu_output)inspect_gpu_output(sample,native_device_.Get(),native_context_.Get());
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
    void fail_next(HRESULT result) {next_receive_result_=result;}
    bool wait_entered() { return WaitForSingleObject(entered_,5000)==WAIT_OBJECT_0; }
    size_t count() { std::lock_guard<std::mutex> guard(results_lock_); return times.size(); }
    REFERENCE_TIME last() { std::lock_guard<std::mutex> guard(results_lock_); return times.empty() ? -1 : times.back(); }
    bool valid=true;
    std::vector<REFERENCE_TIME> times,ends;
    std::vector<bool> discontinuities;
private:
    std::mutex results_lock_;
    bool advertise_=true,reject_=false,native_active_=false,hold_all_=false,custom_allocator_=false;
    long allocator_capacity_=0;
    bool require_native_allocator_=false;
    size_t gpu_count_=0,cpu_count_=0;
    ComPtr<ID3D11Device> native_device_;
    ComPtr<ID3D11DeviceContext> native_context_;
    std::vector<ComPtr<IMediaSample>> retained_;
    std::atomic<bool> block_{false};
    std::atomic<HRESULT> next_receive_result_{S_OK};
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
    std::atomic<bool> producer_done{false};
    std::thread producer([&] {for(int i=0;i<40;++i)if(push(1000+i,i==0)!=S_OK)break;producer_done=true;});
    const auto capacity=size_t(sink->pin().allocator_capacity());
    for(int spin=0;spin<100 && sink->pin().count()<before+capacity;++spin)Sleep(20);
    const bool exhausted=capacity>0 && sink->pin().count()==before+capacity && !producer_done;
    const auto flush_start=std::chrono::steady_clock::now();
    const HRESULT flushed=source->pin().DeliverBeginFlush();producer.join();check(flushed,"exhausted-pool flush");
    if(!exhausted)throw std::runtime_error("Flush test did not exhaust the negotiated output pool");
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
    producer_done=false;
    std::thread pool_producer([&]{for(int i=0;i<40;++i)if(push(10+i,i==0)!=S_OK)break;producer_done=true;});
    for(int spin=0;spin<100 && sink->pin().count()<before_stop+capacity;++spin)Sleep(20);
    const bool stop_exhausted=capacity>0 && sink->pin().count()==before_stop+capacity && !producer_done;
    const auto pool_stop_start=std::chrono::steady_clock::now();
    const HRESULT pool_stopped=filter->Stop();pool_producer.join();check(pool_stopped,"direct Stop during allocator wait");
    if(!stop_exhausted)throw std::runtime_error("Stop test did not exhaust the negotiated output pool");
    if(std::chrono::steady_clock::now()-pool_stop_start>std::chrono::seconds(2))
        throw std::runtime_error("exhausted allocator Stop did not cancel promptly");
    check(control->Stop(),"final graph Stop");
    const bool gpu_output=use_gpu || !reject_renderer;
    if(!sink->pin().valid || (gpu_output ? sink->pin().cpu_count()!=0 || sink->pin().gpu_count()==0 : sink->pin().gpu_count()!=0 || sink->pin().cpu_count()==0))
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
        if(cpu_sink->pin().cpu_count()!=0 || cpu_sink->pin().gpu_count()==0)throw std::runtime_error("software decoder failed to upload to GPU renderer");
        std::cout<<"same-instance native-to-CPU full graph reconnect=OK\n";
    }
}
void run_rate_policy_case(IClassFactory* factory,int64_t source_num,int64_t source_den,
    int64_t expected_num,int64_t expected_den,bool gpu,const std::string& reason,REFERENCE_TIME header_duration=-1,bool scrub_preview=false,bool pause_resume=false) {
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
       color.NominalRange!=DXVA2_NominalRange_16_235 || color.VideoTransferMatrix!=(hdr_transfer?MFVideoTransferMatrix_BT2020_10:DXVA2_VideoTransferMatrix_BT709) ||
       color.VideoPrimaries!=(hdr_transfer?MFVideoPrimaries_BT2020:DXVA2_VideoPrimaries_BT709) || color.VideoTransferFunction!=(hdr_transfer?hdr_transfer:DXVA2_VideoTransFunc_709))
        throw std::runtime_error("negotiated output does not preserve expected color metadata");
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
    if((gpu || cpu_gpu_output) && interpolate && header_duration<0 && source_num>0 &&
       expected_num*source_den%(expected_den*source_num)==0) {
        const int multiple=int(expected_num*source_den/(expected_den*source_num));
        const long capacity=sink->pin().allocator_capacity();
        if(capacity<std::clamp(2L*multiple,4L,32L) || capacity>32)
            throw std::runtime_error("native output cannot retain two bounded source intervals");
        wchar_t expected_capacity[16]{};
        if(GetEnvironmentVariableW(L"NVOF_TEST_EXPECT_OUTPUT_CAPACITY",expected_capacity,_countof(expected_capacity)) &&
            capacity!=_wtoi(expected_capacity))throw std::runtime_error("incorrect negotiated output headroom");
    }
    ComPtr<IMediaControl> control;check(graph.As(&control),"policy control");
    check(preroll_prepare?control->Pause():control->Run(),"policy initial state");
    wchar_t local[32768]{};GetEnvironmentVariableW(L"LOCALAPPDATA",local,_countof(local));
    std::ifstream state(std::filesystem::path(local)/L"NvofPotPlayer"/L"status.json");
    std::ostringstream captured;captured<<state.rdbuf();const auto json=captured.str();
    // Release the Windows read handle before playback replaces status.json.
    state.close();
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
    const int frame_count=header_duration>=0?144:cached_stream?360:stream_bench?72:6;
    auto await_outputs=[&](size_t count) {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
        while(sink->pin().count()<count && std::chrono::steady_clock::now()<deadline)Sleep(1);
        if(sink->pin().count()<count)throw std::runtime_error("timed out waiting for queued output");
    };
    if(preroll_prepare || output_ahead) {
        const long multiple=long(expected_num*source_den/(expected_den*source_num));
        auto trace_contains=[&](const char* marker) {
            std::ifstream trace(std::filesystem::path(local)/L"NvofPotPlayer"/L"filter.log");
            std::ostringstream text;text<<trace.rdbuf();return text.str().find(marker)!=std::string::npos;
        };
        auto wait_marker=[&](const char* marker) {
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
            while(!trace_contains(marker) && std::chrono::steady_clock::now()<deadline)Sleep(5);
            return trace_contains(marker);
        };
        if(output_ahead)sink->pin().hold_all();
        check(push(0),"first original before any preparation");
        if(sink->pin().count()!=1)throw std::runtime_error("first original was buffered");
        int input_count=2;
        if(preroll_prepare) {
            check(push(1),"queue first real pair during preroll");
            Sleep(50);
            const bool prepared=trace_contains("startup pair prepared after first original");
            const bool held=sink->pin().count()==1;
            check(control->Run(),"Run after first pair preparation");
            if(!held)throw std::runtime_error("preparation advanced the paused renderer");
            std::cout<<"PREROLL prepared="<<prepared<<" firstOriginalImmediate=1 pausedOutputs=1\n";
        }else {
            input_count=24;
            std::atomic<HRESULT> produced{E_PENDING};
            std::thread producer([&]{HRESULT hr=S_OK;for(int i=1;i<input_count && hr==S_OK;++i)hr=push(i);produced=hr;});
            bool prepared=false,pool_held=false;
            try {
                await_outputs(size_t(sink->pin().allocator_capacity()));
                prepared=wait_marker("output ahead prepared pending=");
                Sleep(50);pool_held=sink->pin().count()==size_t(sink->pin().allocator_capacity());
                wchar_t shrink[8]{};
                if(GetEnvironmentVariableW(L"NVOF_TEST_LIVE_SHRINK",shrink,_countof(shrink)) && shrink[0]==L'1') {
                    std::cout<<"READY_SHRINK"<<std::endl;
                    std::string proceed;std::getline(std::cin,proceed);
                    if(proceed!="continue")throw std::runtime_error("Missing live shrink command");
                }
                if(wait_output) {
                    Sleep(200); // Let the bounded prepared queue fill as well.
                    GpuCompletionGate gate(device.Get());gate.arm(context.Get());
                    const auto before=sink->pin().count();
                    bool injected=false;
                    for(int i=0;i<14;++i){
                        sink->pin().release_one();Sleep(8);
                        if(wait_output_error&&!injected&&trace_contains("ready outputs delivered during GPU wait")){
                            sink->pin().fail_next(VFW_E_TIMEOUT);injected=true;
                        }
                    }
                    const bool serviced=trace_contains("ready outputs delivered during GPU wait");
                    const auto during=sink->pin().count()-before;
                    if(!wait_output_error) {
                        check(control->Pause(),"Pause during GPU completion service");
                        const auto paused=sink->pin().count();
                        sink->pin().release_one();Sleep(20);
                        gate.release();Sleep(20);
                        if(sink->pin().count()!=paused)throw std::runtime_error("completion service emitted while paused");
                        check(control->Run(),"Resume after serviced GPU completion");
                    } else gate.release();
                    std::cout<<"WAIT_OUTPUT servicedBeforeGpuRelease="<<serviced<<" delivered="<<during<<'\n';
                }
                sink->pin().release_all();
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
                while(produced==E_PENDING && std::chrono::steady_clock::now()<deadline)Sleep(1);
                if(produced==E_PENDING)throw std::runtime_error("output headroom producer stalled after release");
            }catch(...) {source->pin().DeliverBeginFlush();producer.join();throw;}
            producer.join();
            if(wait_output_error) {
                if(produced!=VFW_E_TIMEOUT || !trace_contains("output during GPU wait failure="))
                    throw std::runtime_error("Renderer failure inside completion service was not propagated");
                const auto count=sink->pin().count();
                if(source->pin().DeliverEndOfStream()!=VFW_E_TIMEOUT || sink->pin().count()!=count)
                    throw std::runtime_error("Completion service retried failed renderer output");
                check(control->Stop(),"Stop after completion service failure");
                std::cout<<"PASS renderer timeout during GPU completion service, no retry, EOS error/Stop\n";
                return;
            }
            check(produced,"output headroom producer");
            if(!pool_held)throw std::runtime_error("allocator capacity was bypassed");
            std::cout<<"AHEAD preparedWithFullPool="<<prepared<<" capacity="<<sink->pin().allocator_capacity()<<'\n';
        }
        check(source->pin().DeliverEndOfStream(),"headroom EOS drains all prepared output");
        if(sink->pin().count()!=size_t(input_count*multiple))throw std::runtime_error("headroom lost an output");
        for(size_t i=0;i<sink->pin().times.size();++i) {
            if(sink->pin().times[i]!=int64_t(i)*10000000LL*expected_den/expected_num ||
               sink->pin().ends[i]!=int64_t(i+1)*10000000LL*expected_den/expected_num)
                throw std::runtime_error("headroom changed output cadence");
        }
        check(control->Stop(),"headroom Stop");
        std::cout<<"HEADROOM output="<<sink->pin().count()<<" digest="<<output_digest<<'\n';
        if(preroll_prepare)for(bool stop:{false,true}) {
            check(control->Pause(),"cancel preparation initial Pause");
            const size_t before=sink->pin().count();
            check(push(0),"cancel preparation immediate first");check(push(1),"cancel preparation pair");
            Sleep(250);
            const bool held=sink->pin().count()==before+1;
            const auto begin=std::chrono::steady_clock::now();
            check(stop?filter->Stop():source->pin().DeliverBeginFlush(),"cancel preparation without Run");
            if(std::chrono::steady_clock::now()-begin>std::chrono::seconds(2) || !held)
                throw std::runtime_error("initial preparation blocked cancellation or advanced the paused output");
            if(stop)check(control->Stop(),"preparation graph Stop");
            else check(source->pin().DeliverEndFlush(),"preparation EndFlush");
            check(control->Run(),"preparation restart");
            check(push(100),"preparation restart first original");
            if(sink->pin().count()!=before+2 || sink->pin().last()!=100*kDuration)
                throw std::runtime_error("cancelled initial pair leaked into new segment");
            check(control->Stop(),"preparation final Stop");
        }
        return;
    }
    if(pause_resume)check(control->Pause(),"policy initial preroll");
    const auto stream_begin=std::chrono::steady_clock::now();double first_ms=0;
    auto steady_begin=stream_begin;size_t steady_outputs=0;
    for(int i=0;i<frame_count;++i) {
        check(push(i),"policy source frame");
        if(cached_stream && i==7) {
            await_outputs(1+size_t(i*expected_num*source_den/(expected_den*source_num)));
            steady_outputs=sink->pin().count();steady_begin=std::chrono::steady_clock::now();
        }
        if(i==0)first_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-stream_begin).count();
        if(pause_resume&&i==0)check(control->Run(),"policy run after preroll");
        if(i==0 && (sink->pin().count()!=1 || !sink->pin().discontinuities.back()))
            throw std::runtime_error("policy first original delayed or missing discontinuity");
        if(!interpolate && (sink->pin().count()!=size_t(i+1) || sink->pin().times.back()!=i*kDuration || sink->pin().ends.back()!=(i+1)*kDuration))
            throw std::runtime_error("bypass changed original frame count or timestamps");
        if(pause_resume && (i==1 || i==3 || i==5)) {
            await_outputs(1+size_t(i*expected_num*source_den/(expected_den*source_num)));
            const size_t before_pause=sink->pin().count();
            check(control->Pause(),"policy Pause");
            OAFilterState paused=State_Stopped;
            check(control->GetState(1000,&paused),"policy paused state");
            if(paused!=State_Paused)throw std::runtime_error("graph did not enter paused state");
            Sleep(80);
            if(sink->pin().count()!=before_pause)throw std::runtime_error("pause generated output without new input");
            check(control->Run(),"policy resume");
            // The normal rational-clock/count checks below also cover these
            // boundaries. Pause must retain the adjacent interpolation pair.
        }
    }
    check(push(frame_count,true),"policy missing stop timestamp");
    if(!interpolate && (sink->pin().count()!=size_t(frame_count+1) || sink->pin().ends.back()!=(frame_count+1)*kDuration))
        throw std::runtime_error("bypass synthesized invalid missing stop duration");
    const size_t before_eos=sink->pin().count();check(source->pin().DeliverEndOfStream(),"policy EOS");
    if(interpolate && sink->pin().count()<=size_t(frame_count))throw std::runtime_error("selected source did not interpolate");
    if(interpolate) {
        for(size_t i=0;i<sink->pin().times.size();++i) {
            const int64_t tick=int64_t(i)*10000000LL*expected_den/expected_num;
            const int64_t next=int64_t(i+1)*10000000LL*expected_den/expected_num;
            if(sink->pin().times[i]!=tick || sink->pin().ends[i]!=next)
                throw std::runtime_error("negotiated target does not match delivered rational clock");
        }
    }
    if(pause_resume)std::cout<<"pause/resume=3, held output and continuous rational timestamps=OK\n";
    if(interpolate && header_duration<0 && expected_num*source_den%(expected_den*source_num)==0) {
        const int multiple=int(expected_num*source_den/(expected_den*source_num));
        if(sink->pin().count()!=size_t((frame_count+1)*multiple))throw std::runtime_error("Integer output count including EOS is incorrect");
    }
    if(!interpolate && sink->pin().count()!=before_eos)throw std::runtime_error("bypass emitted an interpolated EOS tail");
    if(stream_bench) {
        if(cached_stream) {
            const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-steady_begin).count();
            std::cout<<"CACHED steadyMs="<<ms<<" output="<<sink->pin().count()-steady_outputs
                <<" fps="<<(sink->pin().count()-steady_outputs)*1000.0/ms<<" excludes=decode,present,cold-start\n";
        }
        const double total_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-stream_begin).count();
        check(control->Stop(),"stream benchmark Stop");
        std::cout<<"STREAM firstMs="<<first_ms<<" totalMs="<<total_ms<<" source="<<frame_count+1<<" output="<<sink->pin().count()<<" digest="<<output_digest<<'\n';
        return;
    }
    check(source->pin().DeliverBeginFlush(),"policy seek flush");check(source->pin().DeliverEndFlush(),"policy seek end");
    check(source->pin().DeliverNewSegment(100*kDuration,INT64_MAX,1),"policy seek segment");
    const size_t before_seek=sink->pin().count();check(push(100),"policy seek original");
    if(sink->pin().count()!=before_seek+1 || sink->pin().times.back()!=100*kDuration || !sink->pin().discontinuities.back())
        throw std::runtime_error("policy seek did not deliver immediate clean original");
    if(preview_separation) {
        sink->pin().block_next();
        std::atomic<bool> returned{false};
        std::thread producer([&]{push(101);returned=true;});
        const bool entered=sink->pin().wait_entered();
        Sleep(30);
        const bool separated=returned;
        // Always release the renderer before joining, including the old
        // synchronous implementation. A regression must fail, not deadlock.
        const HRESULT flushed=source->pin().DeliverBeginFlush();producer.join();
        check(flushed,"preview separation cleanup");
        check(source->pin().DeliverEndFlush(),"preview separation cleanup end");
        check(control->Stop(),"preview separation Stop");
        std::cout<<"PREVIEW rendererBlocked="<<entered<<" producerReturned="<<separated<<'\n';
        if(!entered || !separated)throw std::runtime_error("seek preview blocked the decoder on renderer output");
        return;
    }
    if(scrub_preview) {
        const REFERENCE_TIME preview_span=REFERENCE_TIME(nvof::playback::seek_preview_ms)*10000;
        // Repeated slider requests must not start a new batch of phases
        // between requests. Also cover backward seeks and segment-relative PTS.
        for(int position:{102,104,3,1,0}) {
            Sleep(120); // Each new request must extend the settling window.
            check(source->pin().DeliverBeginFlush(),"scrub flush");
            check(source->pin().DeliverEndFlush(),"scrub end flush");
            check(source->pin().DeliverNewSegment(position*kDuration,INT64_MAX,1),"scrub segment");
            const size_t before=sink->pin().count();
            check(push(-1),"scrub preroll");
            if(sink->pin().count()!=before)throw std::runtime_error("scrub preroll leaked");
            for(int i=0;i<3;++i) {
                check(push(position+i),"scrub original");
                await_outputs(before+size_t(i+1));
                if(sink->pin().count()!=before+size_t(i+1) ||
                   sink->pin().times.back()!=(position+i)*kDuration ||
                   sink->pin().ends.back()!=(position+i+1)*kDuration)
                    throw std::runtime_error("scrub started interpolation before seeking settled");
            }
        }
        Sleep(240);
        size_t before=sink->pin().count();
        const REFERENCE_TIME preview_end=sink->pin().ends.back();
        check(push(3),"scrub resume original");
        await_outputs(before+1);
        if(sink->pin().count()!=before+1 || sink->pin().times.back()!=preview_end)
            throw std::runtime_error("scrub resume reused a stale endpoint or left a clock gap");
        before=sink->pin().count();
        check(push(4),"scrub resume pair");
        await_outputs(before+size_t(expected_num*source_den/(expected_den*source_num)));
        if(sink->pin().count()<=before+1 || !sink->pin().valid)
            throw std::runtime_error("scrub failed to resume interpolation on a fresh adjacent pair");
        for(size_t i=before;i<sink->pin().count();++i)
            if(sink->pin().times[i]!=sink->pin().ends[i-1])
                throw std::runtime_error("scrub resume timestamps overlap or leave a gap");
        std::cout<<"scrub previews=15 originals, preroll=0, fresh resume/continuous timestamps=OK\n";
        // Decoder bursts must not fill output headroom with a long run of
        // source-rate originals. Test both segment-relative and absolute PTS.
        for(int origin:{0,100}) {
            check(source->pin().DeliverBeginFlush(),"burst seek flush");
            check(source->pin().DeliverEndFlush(),"burst seek end");
            check(source->pin().DeliverNewSegment(origin*kDuration,INT64_MAX,1),"burst seek segment");
            const size_t begin=sink->pin().count();
            const int preview_frames=int((preview_span+kDuration-1)/kDuration);
            for(int i=0;i<preview_frames+4;++i)check(push(origin+i),"burst seek input");
            check(source->pin().DeliverEndOfStream(),"burst seek drain");
            size_t first_short=begin;
            while(first_short<sink->pin().count() &&
                  sink->pin().ends[first_short]-sink->pin().times[first_short]>=kDuration-1)++first_short;
            if(first_short==sink->pin().count() ||
               sink->pin().times[first_short]-origin*kDuration>preview_span+kDuration)
                throw std::runtime_error("burst seek queued too much source-rate video before interpolation");
            if(sink->pin().times[begin]!=origin*kDuration || !sink->pin().discontinuities[begin])
                throw std::runtime_error("burst seek lost the immediate clean original");
            for(size_t i=begin+1;i<sink->pin().count();++i)
                if(sink->pin().times[i]!=sink->pin().ends[i-1])
                    throw std::runtime_error("burst seek output timestamps overlap or leave a gap");
            std::cout<<"SEEK_BURST origin="<<origin<<" originals="<<first_short-begin
                     <<" firstShortMs="<<(sink->pin().times[first_short]-origin*kDuration)/10000.0<<'\n';
        }
    }
    if((gpu || cpu_gpu_output) && interpolate && expected_num>200*expected_den) {
        // Exercise the larger high-rate pool, not only the x2/default pool.
        // Both seek and Stop must cancel an upstream producer even when the
        // renderer retains every negotiated sample and blocks further output.
        const size_t capacity=size_t(sink->pin().allocator_capacity());
        for(bool stop_pool:{false,true}) {
            check(source->pin().DeliverBeginFlush(),"high-rate pool reset");
            check(source->pin().DeliverEndFlush(),"high-rate pool reset end");
            const size_t before=sink->pin().count();sink->pin().hold_all();
            std::atomic<bool> done{false};
            std::thread producer([&]{for(int i=0;i<40;++i)if(push(200+i)!=S_OK)break;done=true;});
            for(int spin=0;spin<100 && sink->pin().count()<before+capacity;++spin)Sleep(20);
            const bool exhausted=sink->pin().count()==before+capacity && !done;
            const auto begin=std::chrono::steady_clock::now();
            const HRESULT cancelled=stop_pool?filter->Stop():source->pin().DeliverBeginFlush();
            producer.join();check(cancelled,"high-rate pool cancellation");
            if(!exhausted || std::chrono::steady_clock::now()-begin>std::chrono::seconds(2))
                throw std::runtime_error("high-rate pool did not exhaust or cancel promptly");
            if(stop_pool) {check(control->Stop(),"high-rate graph Stop");check(control->Run(),"high-rate graph restart");}
            else check(source->pin().DeliverEndFlush(),"high-rate pool end flush");
            const size_t resumed=sink->pin().count();check(push(100),"high-rate pool resume original");
            if(sink->pin().count()!=resumed+1 || sink->pin().times.back()!=100*kDuration || !sink->pin().discontinuities.back())
                throw std::runtime_error("high-rate pool did not resume with an immediate clean original");
        }
        std::cout<<"high-rate retained pool="<<capacity<<" cancellation/seek/restart=OK\n";
    }
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
    if(async_lifecycle) {
        const size_t multiple=size_t(expected_num*source_den/(expected_den*source_num));
        // Queue two samples while paused. A third must block until Run, and
        // neither queued sample may reach the renderer while paused.
        check(source->pin().DeliverBeginFlush(),"queued pause reset");
        check(source->pin().DeliverEndFlush(),"queued pause reset end");Sleep(220);
        size_t before=sink->pin().count();check(push(0),"queued first original");
        if(sink->pin().count()!=before+1)throw std::runtime_error("async first picture delayed");
        check(control->Pause(),"queued Pause");
        check(push(1),"paused queue one");check(push(2),"paused queue two");
        std::atomic<bool> done{false};HRESULT pushed=S_OK;
        std::thread blocked([&]{pushed=push(3);done=true;});
        Sleep(80);const bool bounded=!done && sink->pin().count()==before+1;
        check(control->Run(),"queued resume");blocked.join();check(pushed,"queued producer resume");
        await_outputs(before+1+3*multiple);
        if(!bounded)throw std::runtime_error("paused queue was unbounded or emitted output");
        for(size_t i=before+1;i<sink->pin().count();++i)
            if(sink->pin().times[i]!=sink->pin().ends[i-1])throw std::runtime_error("queued resume changed output clock");
        for(bool stop:{false,true}) {
            check(source->pin().DeliverBeginFlush(),"blocked worker reset");
            check(source->pin().DeliverEndFlush(),"blocked worker reset end");
            check(push(0),"blocked worker first");
            sink->pin().block_next();
            // This call must return even though its output is held inside
            // the renderer, including inside the 200 ms preview window.
            // It proves decoder/output separation directly.
            check(push(1),"async source returns before renderer");
            if(!sink->pin().wait_entered())throw std::runtime_error("worker did not enter renderer");
            check(push(2),"blocked worker queue one");check(push(3),"blocked worker queue two");
            done=false;std::thread producer([&]{push(4);done=true;});
            Sleep(80);const bool full=!done;
            const auto begin=std::chrono::steady_clock::now();
            const HRESULT hr=stop?filter->Stop():source->pin().DeliverBeginFlush();
            producer.join();check(hr,"cancel full input queue and blocked renderer");
            if(!full || std::chrono::steady_clock::now()-begin>std::chrono::seconds(2))
                throw std::runtime_error("input queue cancellation was not bounded/prompt");
            if(stop){check(control->Stop(),"queued graph stop");check(control->Run(),"queued restart");}
            else check(source->pin().DeliverEndFlush(),"queued seek end");
            before=sink->pin().count();check(push(100),"queued seek immediate original");
            if(sink->pin().count()!=before+1 || sink->pin().last()!=100*kDuration)
                throw std::runtime_error("old queued samples leaked after seek/restart");
        }
        check(source->pin().DeliverBeginFlush(),"deferred failure reset");
        check(source->pin().DeliverEndFlush(),"deferred failure reset end");Sleep(220);
        check(push(0),"deferred failure first");before=sink->pin().count();
        check(source->pin().push_gpu(1,device.Get(),context.Get(),false,true,true),"enqueue invalid MF sample");
        if(SUCCEEDED(source->pin().DeliverEndOfStream()) || sink->pin().count()!=before)
            throw std::runtime_error("deferred GPU sample failure was lost or fell back to CPU");
        if(SUCCEEDED(push(2)))throw std::runtime_error("worker failure did not propagate to the next producer");
        check(source->pin().DeliverBeginFlush(),"renderer timeout reset");
        check(source->pin().DeliverEndFlush(),"renderer timeout reset end");Sleep(220);
        check(push(0),"renderer timeout first");before=sink->pin().count();
        sink->pin().fail_next(VFW_E_TIMEOUT);check(push(1),"renderer timeout enqueue");
        if(source->pin().DeliverEndOfStream()!=VFW_E_TIMEOUT || sink->pin().count()!=before)
            throw std::runtime_error("renderer timeout was mistaken for a full allocator and retried");
        std::cout<<"PASS async decoder return while renderer blocked, bounded paused queue, resume clock, full-queue flush/Stop/restart, deferred error propagation\n";
    }
    check(control->Stop(),"policy Stop");
    if((gpu || cpu_gpu_output) ? sink->pin().cpu_count()!=0 : sink->pin().gpu_count()!=0)throw std::runtime_error("rate policy changed frame transport");
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
        if(incompatible==3)color.VideoTransferFunction=MFVideoTransFunc_2084; // PQ with incompatible BT709 primaries must be rejected.
        header->dwControlFlags=color.value|AMCONTROL_COLORINFO_PRESENT;
        check(graph->ConnectDirect(&source->pin(),input.Get(),&media),"color connect");
        ComPtr<nvof::transport::DecoderConfiguration> native;check(input.As(&native),"color native contract");
        if(native->ActivateD3D11Decoding(device,context,nullptr,0)!=VFW_E_TYPE_NOT_ACCEPTED)
            throw std::runtime_error("unsupported native color metadata was accepted");
    }
    std::cout<<"explicit full-range/601/primaries/PQ color rejection=OK\n";
}
void check_hdr_storage() {
    HRESULT hr=S_OK;auto* raw=new nvof::transport::SurfaceAllocator(&hr,true);check(hr,"metadata allocator");
    ComPtr<IMemAllocator> allocator;check(raw->QueryInterface(IID_PPV_ARGS(&allocator)),"metadata allocator QI");
    ALLOCATOR_PROPERTIES requested{1,1024,1,0},actual{};check(allocator->SetProperties(&requested,&actual),"metadata pool");check(allocator->Commit(),"metadata commit");
    ComPtr<IMediaSample> sample;check(allocator->GetBuffer(&sample,nullptr,nullptr,0),"metadata sample");
    ComPtr<nvof::MediaSideData> side;check(sample.As(&side),"metadata QI");
    BYTE mastering[80]{};mastering[0]=41;unsigned light[2]={1000,400};
    check(side->SetSideData(nvof::hdr_mastering_id,mastering,80),"metadata set");
    const BYTE* retained=nullptr;size_t size=0;check(side->GetSideData(nvof::hdr_mastering_id,&retained,&size),"metadata get");
    auto snapshot=nvof::capture_hdr(sample.Get());mastering[0]=72;
    check(side->SetSideData(nvof::hdr_mastering_id,mastering,80),"metadata replace");
    check(side->SetSideData(nvof::hdr_light_id,reinterpret_cast<BYTE*>(light),8),"metadata light");
    if(size!=80 || retained[0]!=41 || snapshot->data[0][0]!=41 || nvof::capture_hdr(sample.Get())->data[0][0]!=72)throw std::runtime_error("HDR snapshot/pointer lifetime failure");
    if(side->SetSideData(nvof::hdr_mastering_id,mastering,79)!=E_INVALIDARG || side->SetSideData(nvof::hdr_mastering_id,nullptr,80)!=E_POINTER)throw std::runtime_error("Malformed HDR accepted");
    for(int i=2;i<5;++i) {
        const size_t length=i==2?128:i==3?5225:9353;
        std::vector<BYTE> payload(length,37);
        check(side->SetSideData(nvof::hdr_ids[i],payload.data(),length),"DV metadata set");
        auto copy=nvof::capture_hdr(sample.Get());payload.assign(length,81);
        check(side->GetSideData(nvof::hdr_ids[i],&retained,&size),"DV metadata get");
        check(side->SetSideData(nvof::hdr_ids[i],payload.data(),length),"DV metadata replace");
        if(retained[0]!=37 || copy->data[i][0]!=37 || nvof::capture_hdr(sample.Get())->data[i][0]!=81)throw std::runtime_error("DV snapshot or pointer lifetime failure");
        if(side->SetSideData(nvof::hdr_ids[i],payload.data(),65537)!=E_INVALIDARG ||
           side->SetSideData(nvof::hdr_ids[i],payload.data(),0)!=E_INVALIDARG)throw std::runtime_error("Invalid DV size accepted");
    }
    side.Reset();sample.Reset();check(allocator->GetBuffer(&sample,nullptr,nullptr,0),"recycled metadata sample");
    if(nvof::capture_hdr(sample.Get()))throw std::runtime_error("Recycled sample leaked previous HDR metadata");
    if(snapshot->data[0][0]!=41)throw std::runtime_error("Released source corrupted HDR snapshot");
    sample.Reset();allocator->Decommit();std::cout<<"HDR metadata deepcopy/pointer lifetime/pool reuse/invalid size=OK\n";
}
int wmain(int argc,wchar_t** argv) {
    if(argc<2){std::cerr<<"usage: directshow_gpu_smoke <NativeD3D11=1 filter.ax>\n";return 2;}
    if(argc>10) {
        kHeight=_wtoi(argv[10]);
        if(kHeight<240 || kHeight>8192 || (kHeight&1)){std::cerr<<"invalid fixture height\n";return 2;}
    }
    check_hdr_storage();
    p010_fixture=(argc>9 && wcscmp(argv[9],L"p010")==0) || (argc>2 && (wcscmp(argv[2],L"--p010-input")==0 || wcscmp(argv[2],L"--rounded-duration-p010")==0));
    p010_media=argc>9 && wcscmp(argv[9],L"p010-media")==0;
    if(argc>9 && wcscmp(argv[9],L"dovi")==0){dovi_fixture=true;p010_fixture=true;p010_media=true;hdr_transfer=MFVideoTransFunc_2084;}
    if(argc>9 && (wcscmp(argv[9],L"hdr10")==0 || wcscmp(argv[9],L"hlg")==0)) {p010_media=true;hdr_transfer=wcscmp(argv[9],L"hdr10")==0?MFVideoTransFunc_2084:MFVideoTransFunc_HLG;}
    if(argc>9 && (wcscmp(argv[9],L"hdr10-legacy")==0 || wcscmp(argv[9],L"hlg-legacy")==0)) {
        recover_transfer=wcscmp(argv[9],L"hdr10-legacy")==0?16:18;
        hdr_transfer=recover_transfer==16?MFVideoTransFunc_2084:MFVideoTransFunc_HLG;p010_fixture=true;color_fixture=1;
    }
    if(argc>9 && std::wstring(argv[9]).find(L"av1-")==0) {
        av1_fixture_header=true;recover_transfer=std::wstring(argv[9]).find(L"hlg")!=std::wstring::npos?18:16;
        hdr_transfer=recover_transfer==16?MFVideoTransFunc_2084:MFVideoTransFunc_HLG;
        p010_fixture=true;color_fixture=1;
        if(std::wstring(argv[9]).find(L"sdr")!=std::wstring::npos){hdr_transfer=0;color_fixture=2;}
        if(std::wstring(argv[9]).find(L"vi1")!=std::wstring::npos)av1_format=1;
        if(std::wstring(argv[9]).find(L"vi2")!=std::wstring::npos)av1_format=2;
    }
    if(argc>9 && std::wstring(argv[9]).find(L"vp9-")==0) {
        vp9_case=argv[9];recover_transfer=vp9_case.find(L"hlg")!=std::wstring::npos?18:16;
        hdr_transfer=recover_transfer==16?MFVideoTransFunc_2084:MFVideoTransFunc_HLG;
        p010_fixture=true;color_fixture=1;
        if(vp9_case.find(L"override")!=std::wstring::npos){hdr_transfer=0;color_fixture=2;}
        for(const auto* negative:{L"truncated",L"full-range",L"unspecified",L"10bit-sdr",L"missing",L"conflict"})
            if(vp9_case.find(negative)!=std::wstring::npos)hdr_transfer=0;
        if(vp9_case.find(L"vi1")!=std::wstring::npos)av1_format=1;
        if(vp9_case.find(L"vi2")!=std::wstring::npos)av1_format=2;
    }
    if(argc>11){kWidth=std::stoi(argv[10]);kHeight=std::stoi(argv[11]);}
    if(argc>9 && wcscmp(argv[9],L"sdr-override")==0){recover_transfer=16;p010_fixture=true;color_fixture=2;}
    if(p010_media)p010_fixture=true;
    require_services=argc>2 && wcscmp(argv[2],L"--probe-services")==0;
    check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"CoInitialize");
    HMODULE module=LoadLibraryExW(argv[1],nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
    cpu_gpu_output=GetPrivateProfileIntW(L"Nvof",L"NativeD3D11",0,
        (std::filesystem::path(argv[1]).parent_path()/L"NvofPotPlayer.ini").c_str())!=0;
    if(!module){std::cerr<<"load failed "<<GetLastError()<<'\n';return 3;}
    int result=0;
    try {
        // Optional bounded first-pair readback for cross-build precision checks.
        // Test host only: never enabled in player binaries or timing measurements.
        wchar_t reference_path[32768]{},reference_limit[16]{};
        const DWORD reference_length=GetEnvironmentVariableW(L"NVOF_TEST_PIXEL_REFERENCE",reference_path,32768);
        GetEnvironmentVariableW(L"NVOF_TEST_PIXEL_FRAMES",reference_limit,16);
        const unsigned reference_frames=unsigned((std::max)(1,(std::min)(16,_wtoi(reference_limit))));
        const bool compare_reference=GetEnvironmentVariableW(L"NVOF_TEST_PIXEL_COMPARE",nullptr,0)!=0;
        if(reference_length>=32768)throw std::runtime_error("reference path too long");
        const std::filesystem::path reference_root(reference_path);
        if(reference_length && !reference_root.is_absolute())throw std::runtime_error("reference path must be absolute");
        if(reference_length && !compare_reference)std::filesystem::create_directories(reference_root);
        if(reference_length || GetEnvironmentVariableW(L"NVOF_TEST_HASH_OUTPUT",nullptr,0))inspect_gpu_output=[=,index=0u,total=uint64_t(0)](IMediaSample* sample,ID3D11Device* device,ID3D11DeviceContext* context) mutable {
            const unsigned frame=index++;
            if(reference_length && frame>=reference_frames)return;
            ComPtr<nvof::transport::TextureSample> surface;check(sample->QueryInterface(IID_PPV_ARGS(&surface)),"digest surface");
            ComPtr<ID3D11Texture2D> texture;UINT slice=0;check(surface->GetD3D11Texture(0,&texture,&slice),"digest texture");
            D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
            desc.ArraySize=1;desc.BindFlags=0;desc.MiscFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;check(device->CreateTexture2D(&desc,nullptr,&staging),"digest staging");
            context->CopySubresourceRegion(staging.Get(),0,0,0,0,texture.Get(),slice,nullptr);
            D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"digest map");
            const size_t bytes=desc.Width*(desc.Format==DXGI_FORMAT_P010?2:1);
            if(reference_length) {
                std::vector<BYTE> pixels(bytes*desc.Height*3/2);
                for(UINT y=0;y<desc.Height*3/2;++y)std::memcpy(pixels.data()+y*bytes,static_cast<const BYTE*>(mapped.pData)+y*mapped.RowPitch,bytes);
                context->Unmap(staging.Get(),0);
                total+=pixels.size();if(total>512ULL*1024*1024)throw std::runtime_error("reference exceeds 512 MiB budget");
                const auto path=reference_root/(std::to_wstring(frame)+L".pixels");
                const uint64_t header[]={desc.Width,desc.Height,uint64_t(desc.Format),pixels.size()};
                if(!compare_reference) {
                    if(std::filesystem::exists(path))throw std::runtime_error("reference already exists");
                    std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(header),sizeof(header));
                    file.write(reinterpret_cast<const char*>(pixels.data()),std::streamsize(pixels.size()));
                    if(!file)throw std::runtime_error("reference write failed");
                } else {
                    std::ifstream file(path,std::ios::binary);uint64_t saved[4]{};file.read(reinterpret_cast<char*>(saved),sizeof(saved));
                    if(!file || std::memcmp(saved,header,sizeof(header)))throw std::runtime_error("reference format mismatch");
                    std::vector<BYTE> previous(pixels.size());file.read(reinterpret_cast<char*>(previous.data()),std::streamsize(previous.size()));
                    if(!file)throw std::runtime_error("reference read failed");
                    uint64_t changed=0;unsigned maximum=0;const bool tenbit=desc.Format==DXGI_FORMAT_P010;
                    const size_t count=pixels.size()/(tenbit?2:1);
                    for(size_t i=0;i<count;++i) {
                        const unsigned a=tenbit?(unsigned(pixels[i*2])|(unsigned(pixels[i*2+1])<<8))>>6:pixels[i];
                        const unsigned b=tenbit?(unsigned(previous[i*2])|(unsigned(previous[i*2+1])<<8))>>6:previous[i];
                        const unsigned delta=a>b?a-b:b-a;changed+=delta!=0;maximum=(std::max)(maximum,delta);
                    }
                    std::cout<<"PIXELS frame="<<frame<<" components="<<count<<" changed="<<changed<<" maxDelta="<<maximum<<'\n';
                }
                return;
            }
            for(UINT y=0;y<desc.Height*3/2;++y)for(size_t x=0;x<bytes;++x) {
                output_digest^=static_cast<const BYTE*>(mapped.pData)[y*mapped.RowPitch+x];output_digest*=1099511628211ULL;
            }
            context->Unmap(staging.Get(),0);
        };
        auto get_factory=reinterpret_cast<HRESULT(WINAPI*)(REFCLSID,REFIID,void**)>(GetProcAddress(module,"DllGetClassObject"));
        ComPtr<IClassFactory> factory;check(get_factory(kFilter,IID_PPV_ARGS(&factory)),"factory");
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
        check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_VIDEO_SUPPORT|D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"D3D11 device");
        ComPtr<ID3D10Multithread> multithread;
        if(SUCCEEDED(context.As(&multithread)))multithread->SetMultithreadProtected(TRUE);
        if(argc>2 && wcscmp(argv[2],L"--scrub-preview")==0) {
            // Model a decoder burst with prepared GPU surfaces. Generating and
            // uploading each 8K fixture on the CPU can exceed the preview window,
            // legitimately expiring the wall deadline before three inputs.
            cached_seek=true;
            run_rate_policy_case(factory.Get(),argc>=9?_wtoi64(argv[3]):24000,argc>=9?_wtoi64(argv[4]):1001,
                argc>=9?_wtoi64(argv[5]):120000,argc>=9?_wtoi64(argv[6]):1001,
                argc<=7 || wcscmp(argv[7],L"cpu")!=0,"selected",-1,true);
        } else if(argc>2 && (wcscmp(argv[2],L"--rounded-duration")==0 || wcscmp(argv[2],L"--rounded-duration-p010")==0)) {
            run_rate_policy_case(factory.Get(),24000,1001,1000,21,true,"selected",420000);
            std::cout<<"PASS 42ms header with 23.976 PTS through MF GPU transport; P010="<<p010_fixture<<std::endl;
        } else if(argc>=9 && (wcscmp(argv[2],L"--wait-output")==0 || wcscmp(argv[2],L"--preroll-prepare")==0 || wcscmp(argv[2],L"--output-ahead")==0 || wcscmp(argv[2],L"--rate-policy")==0 || wcscmp(argv[2],L"--preview-separation")==0 || wcscmp(argv[2],L"--stream-bench")==0 || wcscmp(argv[2],L"--async-lifecycle")==0 || wcscmp(argv[2],L"--pause-resume")==0 || wcscmp(argv[2],L"--metadata-legacy")==0 || wcscmp(argv[2],L"--metadata-tagged")==0)) {
            preroll_prepare=wcscmp(argv[2],L"--preroll-prepare")==0;
            wait_output=wcscmp(argv[2],L"--wait-output")==0;
            wait_output_error=wait_output&&GetEnvironmentVariableW(L"NVOF_TEST_WAIT_OUTPUT_ERROR",nullptr,0)>0;
            output_ahead=wait_output || wcscmp(argv[2],L"--output-ahead")==0;
            preview_separation=wcscmp(argv[2],L"--preview-separation")==0;
            async_lifecycle=wcscmp(argv[2],L"--async-lifecycle")==0;
            stream_bench=wcscmp(argv[2],L"--stream-bench")==0;
            wchar_t flag[8]{};
            cached_stream=stream_bench && GetEnvironmentVariableW(L"NVOF_TEST_CACHED_STREAM",flag,_countof(flag)) && flag[0]==L'1';
            if(!recover_transfer)color_fixture=wcscmp(argv[2],L"--metadata-legacy")==0?1:wcscmp(argv[2],L"--metadata-tagged")==0?2:0;
            if(color_fixture && !recover_transfer){check_malformed_media(factory.Get());check_explicit_metadata_preservation(factory.Get());}
            const std::wstring wide_reason=argv[8];
            run_rate_policy_case(factory.Get(),_wtoi64(argv[3]),_wtoi64(argv[4]),_wtoi64(argv[5]),_wtoi64(argv[6]),
                wcscmp(argv[7],L"mf")==0,std::string(wide_reason.begin(),wide_reason.end()),-1,false,wcscmp(argv[2],L"--pause-resume")==0);
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
