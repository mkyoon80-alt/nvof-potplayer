#include "nvof/video_format.hpp"
#include "nvof/cpu_video.hpp"
#include "nvof/cpu_upload.hpp"
#include "nvof/avc_depth.hpp"
#include "nvof/hevc_color.hpp"
#include "nvof/av1_color.hpp"
#include "nvof/vp9_color.hpp"
#include "nvof/input_height_policy.hpp"
#include "nvof/resolution_policy.hpp"
#include "nvof/playback_timing.hpp"
#include "nvof/seek_preview.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "nvof/status_writer.hpp"
#include <streams.h>
#include <dvdmedia.h>
#include <mfidl.h>
#include <mfapi.h>
#include <evr.h>
#include <mferror.h>
#include <d3d9.h>
#include <dxva2api.h>
#include <dxgi1_2.h>
#include <shellapi.h>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <limits>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <thread>
#include <new>
#include <numeric>
#include <cstddef>
#include <vector>
#include <optional>
#include <stdexcept>
#include <string>
#include "nvof/engine.hpp"
#include "nvof/pipeline.hpp"
#include "nvof/gpu_phase_pipeline.hpp"
#include "nvof/phase_pipeline.hpp"
#include "nvof/d3d11_transport.hpp"
#include "nvof/mf_bridge.hpp"
#include "nvof/input_rate_policy.hpp"
#include "nvof/output_rate_policy.hpp"
#include "nvof/output_buffer_policy.hpp"
#include "nvof/adapter_selection.hpp"

// Independent identity: never replaces the Smootter/VapourSynth filter.
// {EDECA044-78CD-40EB-8F37-63D947C501A0}
const CLSID CLSID_NvofPotPlayer =
{0xedeca044, 0x78cd, 0x40eb, {0x8f, 0x37, 0x63, 0xd9, 0x47, 0xc5, 0x01, 0xa0}};

// {DE1F386E-626B-442A-98C5-59C7713D21BA}
const CLSID CLSID_NvofPropertyPage =
{0xde1f386e, 0x626b, 0x442a, {0x98, 0xc5, 0x59, 0xc7, 0x71, 0x3d, 0x21, 0xba}};

namespace {
constexpr REFERENCE_TIME kUnits = 10000000;
constexpr char kFilterBuild[]="0.4.11";
// Public LAV/renderer COM contracts. Probing them never advertises support.
// https://github.com/Nevcairiel/LAVFilters/blob/master/include/ID3DVideoMemoryConfiguration.h
const IID kD3D11DecoderConfiguration =
{0x2bb66002,0x46b7,0x4f13,{0x90,0x36,0x70,0x53,0x32,0x87,0x42,0xbe}};
const IID kD3D11DecoderTextureConfiguration =
{0x1f084c92,0x2a5a,0x472c,{0xbf,0xa9,0xd3,0x0e,0x40,0xb8,0x0c,0x00}};
const IID kMediaSampleD3D11 =
{0xbc8753f5,0x0ac8,0x4806,{0x8e,0x5f,0xa1,0x2b,0x2a,0xfe,0x15,0x3e}};
HMODULE module_handle = nullptr;
std::mutex log_mutex;
std::atomic<unsigned> next_filter_instance{1};

std::filesystem::path module_directory() {
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(module_handle, path, _countof(path));
    if (!length || length >= _countof(path))
        throw std::runtime_error("Cannot locate filter runtime directory");
    return std::filesystem::path(path).parent_path();
}

// No source path, media title, or decoder sample contents are logged.
void write_log(const std::string& message) noexcept {
    try {
        std::lock_guard<std::mutex> lock(log_mutex);
        wchar_t local[32768]{};
        const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, _countof(local));
        if (!length || length >= _countof(local)) return;
        const auto directory = std::filesystem::path(local) / L"NvofPotPlayer";
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        const auto path = directory / L"filter.log";
        if (std::filesystem::exists(path, error) &&
            std::filesystem::file_size(path, error) > 2 * 1024 * 1024) {
            // Keep only two bounded chunks so a short startup/seek comparison
            // does not lose its initial control events when the file rolls.
            const auto previous=directory/L"filter.previous.log";
            std::filesystem::remove(previous,error);error.clear();
            std::filesystem::rename(path,previous,error);
            if(error){std::ofstream truncate(path,std::ios::trunc);}
        }
        SYSTEMTIME now{};
        GetLocalTime(&now);
        char prefix[96]{};
        sprintf_s(prefix, "%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu ",
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
            now.wSecond, now.wMilliseconds, GetCurrentProcessId());
        std::ofstream stream(path, std::ios::app);
        stream << prefix << message;
        if(message.compare(0,12,"performance ")==0) {
            // Sample focus only once per diagnostic window; never log the
            // other application's identity or title. -1 means unavailable.
            DWORD foreground=0;
            GetWindowThreadProcessId(GetForegroundWindow(),&foreground);
            stream << " playerForeground=" << (foreground?int(foreground==GetCurrentProcessId()):-1);
        }
        stream << '\n';
    } catch (...) { /* Diagnostics must never break a player shutdown. */ }
}

std::string guid_text(REFGUID id) {
    wchar_t value[40]{};StringFromGUID2(id,value,_countof(value));
    std::string text;for(const auto c:value){if(!c)break;text+=char(c);}return text;
}
std::string component_name(const wchar_t* text) {
    if(!text)return "unknown";
    std::wstring value(text);
    if(value.size()>128 || value.find_first_of(L"/\\:.\r\n")!=std::wstring::npos)return "[redacted]";
    const int size=WideCharToMultiByte(CP_UTF8,0,value.c_str(),int(value.size()),nullptr,0,nullptr,nullptr);
    if(size<=0)return "unknown";
    std::string result(size,' ');WideCharToMultiByte(CP_UTF8,0,value.c_str(),int(value.size()),result.data(),size,nullptr,nullptr);
    return result;
}
void log_endpoint(unsigned instance,const char* phase,PIN_DIRECTION direction,IPin* connected) noexcept {
    if(!connected)return;
    try {
        PIN_INFO info{};
        const HRESULT info_result=connected->QueryPinInfo(&info);
        Microsoft::WRL::ComPtr<IBaseFilter> filter;filter.Attach(info.pFilter);
        CLSID clsid=CLSID_NULL;FILTER_INFO details{};
        if(filter) {filter->GetClassID(&clsid);filter->QueryFilterInfo(&details);}
        if(details.pGraph)details.pGraph->Release();
        std::string line="topology instance="+std::to_string(instance)+" phase="+phase+
            " endpoint="+(direction==PINDIR_INPUT?"upstream":"downstream")+
            " filter="+component_name(details.achName)+" clsid="+guid_text(clsid)+
            " pin="+component_name(info.achName)+" pinDirection="+
            (SUCCEEDED(info_result)?(info.dir==PINDIR_INPUT?"input":"output"):"unknown");
        CMediaType media;
        if(SUCCEEDED(connected->ConnectionMediaType(&media)))line+=" major="+guid_text(media.majortype)+
            " subtype="+guid_text(media.subtype)+" format="+guid_text(media.formattype);
        if(media.formattype==FORMAT_VideoInfo2 && media.cbFormat>=sizeof(VIDEOINFOHEADER2) && media.pbFormat) {
            const auto* video=reinterpret_cast<const VIDEOINFOHEADER2*>(media.pbFormat);
            DXVA2_ExtendedFormat color{};color.value=video->dwControlFlags;
            line+=" colorInfoPresent="+std::to_string(bool(video->dwControlFlags & AMCONTROL_COLORINFO_PRESENT))+
                " range="+std::to_string(color.NominalRange)+" matrix="+std::to_string(color.VideoTransferMatrix)+
                " primaries="+std::to_string(color.VideoPrimaries)+" transfer="+std::to_string(color.VideoTransferFunction);
        } else line+=" colorInfoPresent=0";
        write_log(line);
    }catch(...){}
}
class DiagnosticServiceProbe {
public:
    HRESULT request(unsigned instance,const char* location,REFGUID service,REFIID requested,void** value) noexcept {
        if(!value)return E_POINTER;
        *value=nullptr;
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            for(const auto& pair:seen_)if(pair.first==service && pair.second==requested)return E_NOINTERFACE;
            if(seen_.size()<16) {
                seen_.emplace_back(service,requested);
                write_log("service probe instance="+std::to_string(instance)+" location="+location+
                    " service="+guid_text(service)+" requestedIID="+guid_text(requested)+
                    " observed request; service result handled separately");
            }
        }catch(...){}
        return E_NOINTERFACE;
    }
private:
    std::mutex mutex_;
    std::vector<std::pair<GUID,GUID>> seen_;
};
std::string json_string(const std::string& value) {
    std::string result="\"";
    for (unsigned char character : value) {
        if (character=='"' || character=='\\') {
            result+='\\'; result+=char(character);
        } else if (character<32) {
            char escaped[8]{};
            sprintf_s(escaped,"\\u%04x",unsigned(character));
            result+=escaped;
        } else result+=char(character);
    }
    return result+'"';
}

struct Layout {
    int width = 0;
    int height = 0;
    int pitch = 0;
    long size = 0;
    REFERENCE_TIME duration = 0;
    bool native_color_supported = true;
    bool ten_bit = false;
    unsigned hdr_transfer = 0;
    nvof::CpuVideoFormat cpu_format=nvof::CpuVideoFormat::p010;
    bool interlaced=false,top_first=false;
    REFERENCE_TIME sample_duration=0;
};

bool parse_layout(const CMediaType& media, Layout& layout) noexcept {
    const bool nv12=media.subtype==MEDIASUBTYPE_NV12;
    const bool packed=media.subtype==nvof::y210_subtype;
    const bool full_chroma=packed || media.subtype==nvof::p210_subtype;
    if (media.majortype != MEDIATYPE_Video || (!nv12 && media.subtype!=nvof::p010_subtype &&
        media.subtype!=nvof::p016_subtype && !full_chroma))return false;
    const bool ten_bit=!nv12;
    const auto cpu_format=packed?nvof::CpuVideoFormat::y210:full_chroma?nvof::CpuVideoFormat::p210:
        media.subtype==nvof::p016_subtype?nvof::CpuVideoFormat::p016:nvof::CpuVideoFormat::p010;
    bool interlaced=false,top_first=false;
    const BITMAPINFOHEADER* bitmap = nullptr;
    size_t bitmap_offset=0;
    RECT source{};
    bool native_color_supported = true;
    unsigned hdr_transfer=0;
    REFERENCE_TIME duration = 0;
    if (media.formattype == FORMAT_VideoInfo && media.cbFormat >= sizeof(VIDEOINFOHEADER)) {
        const auto* video = reinterpret_cast<const VIDEOINFOHEADER*>(media.pbFormat);
        if (!video) return false;
        bitmap = &video->bmiHeader;
        bitmap_offset=offsetof(VIDEOINFOHEADER,bmiHeader);
        source = video->rcSource;
        duration = video->AvgTimePerFrame;
    } else if (media.formattype == FORMAT_VideoInfo2 && media.cbFormat >= sizeof(VIDEOINFOHEADER2)) {
        const auto* video = reinterpret_cast<const VIDEOINFOHEADER2*>(media.pbFormat);
        if (!video)return false;
        interlaced=(video->dwInterlaceFlags & AMINTERLACE_IsInterlaced)!=0;
        top_first=(video->dwInterlaceFlags & AMINTERLACE_Field1First)!=0;
        // Two fields per sample only. Single-field samples need their own cadence.
        if(interlaced && (nv12 || (video->dwInterlaceFlags & AMINTERLACE_1FieldPerSample)))return false;
        if(video->dwControlFlags & AMCONTROL_COLORINFO_PRESENT) {
            DXVA2_ExtendedFormat color{};color.value=video->dwControlFlags;
            native_color_supported=(color.NominalRange==DXVA2_NominalRange_Unknown || color.NominalRange==DXVA2_NominalRange_16_235) &&
                (color.VideoTransferMatrix==DXVA2_VideoTransferMatrix_Unknown || color.VideoTransferMatrix==DXVA2_VideoTransferMatrix_BT709) &&
                (color.VideoPrimaries==DXVA2_VideoPrimaries_Unknown || color.VideoPrimaries==DXVA2_VideoPrimaries_BT709) &&
                (color.VideoTransferFunction==DXVA2_VideoTransFunc_Unknown || color.VideoTransferFunction==DXVA2_VideoTransFunc_709);
            // MF enum extensions are also carried in DirectShow DXVA color bits.
            // Windows mfobjects.h: PQ=15, HLG=16, BT2020 primaries=9/matrix=4.
            if(color.VideoTransferFunction==MFVideoTransFunc_2084 || color.VideoTransferFunction==MFVideoTransFunc_HLG) {
                hdr_transfer=color.VideoTransferFunction;
                native_color_supported=(color.NominalRange==DXVA2_NominalRange_Unknown || color.NominalRange==DXVA2_NominalRange_16_235) &&
                    color.VideoPrimaries==MFVideoPrimaries_BT2020 && color.VideoTransferMatrix==MFVideoTransferMatrix_BT2020_10;
            }
        }
        bitmap = &video->bmiHeader;
        bitmap_offset=offsetof(VIDEOINFOHEADER2,bmiHeader);
        source = video->rcSource;
        duration = video->AvgTimePerFrame;
    } else return false;
    if (bitmap->biSize < sizeof(BITMAPINFOHEADER) || bitmap->biSize>media.cbFormat-bitmap_offset || bitmap->biWidth <= 0 ||
        bitmap->biHeight == std::numeric_limits<LONG>::min() ||
        bitmap->biCompression != media.subtype.Data1 ||
        bitmap->biPlanes != 1 || bitmap->biBitCount != (full_chroma?32:ten_bit?24:12)) return false;
    const int storage_height = std::abs(bitmap->biHeight);
    if (storage_height <= 0 || storage_height > 8192 || bitmap->biWidth > 32768)
        return false;
    int width = bitmap->biWidth;
    int height = storage_height;
    if (source.right || source.bottom || source.left || source.top) {
        // Cropped offsets require different UV alignment. Accept the common
        // right/bottom visible rectangle used for decoder/renderer padding.
        if (source.left != 0 || source.top != 0 || source.right <= 0 ||
            source.bottom != storage_height || source.right > bitmap->biWidth)
            return false;
        width = source.right;
        height = source.bottom;
    }
    if (width <= 0 || width > 8192 || (width & 1) || (height & 1)) return false;
    if(interlaced && ((!full_chroma && (height%4)) || duration<=1))return false;
    int pitch = bitmap->biWidth*(packed?4:ten_bit?2:1);
    const int64_t rows = packed?storage_height:full_chroma?int64_t(storage_height)*2:int64_t(storage_height)*3/2;
    if (bitmap->biSizeImage) {
        if (bitmap->biSizeImage < rows * pitch) return false;
        // A larger integral row size is a padded surface; do not infer pitch
        // from allocator capacity, which may include unrelated spare bytes.
        if (bitmap->biSizeImage % rows == 0) {
            const int64_t advertised_pitch = bitmap->biSizeImage / rows;
            if (advertised_pitch < pitch || advertised_pitch > 65536) return false;
            pitch = static_cast<int>(advertised_pitch);
        }
    }
    if ((pitch & 1) || pitch < width) return false;
    const int64_t size = rows * pitch;
    if (size > std::numeric_limits<long>::max()) return false;
    layout = {width, height, pitch, static_cast<long>(size), interlaced?duration/2:duration, native_color_supported,
        ten_bit, hdr_transfer,cpu_format,interlaced,top_first,duration};
    return true;
}

// DirectShow carries nominal range in VIDEOINFOHEADER2, not on the NV12
// texture. The native path's supported fallback is BT.709 limited-range SDR.
// Preserve decoder-provided fields; only make the existing fallback explicit.
DWORD native_output_color_flags(const CMediaType& media) noexcept {
    DWORD controls=0;
    if(media.formattype==FORMAT_VideoInfo2 && media.pbFormat && media.cbFormat>=sizeof(VIDEOINFOHEADER2))
        controls=reinterpret_cast<const VIDEOINFOHEADER2*>(media.pbFormat)->dwControlFlags;
    DXVA2_ExtendedFormat color{};
    if(controls & AMCONTROL_COLORINFO_PRESENT)color.value=controls & 0xffffff00u;
    if(color.NominalRange==DXVA2_NominalRange_Unknown)color.NominalRange=DXVA2_NominalRange_16_235;
    if(color.VideoTransferMatrix==DXVA2_VideoTransferMatrix_Unknown)color.VideoTransferMatrix=DXVA2_VideoTransferMatrix_BT709;
    if(color.VideoPrimaries==DXVA2_VideoPrimaries_Unknown)color.VideoPrimaries=DXVA2_VideoPrimaries_BT709;
    if(color.VideoTransferFunction==DXVA2_VideoTransFunc_Unknown)color.VideoTransferFunction=DXVA2_VideoTransFunc_709;
    // Low eight bits are AMCONTROL flags, not DXVA SampleFormat.
    return (color.value & 0xffffff00u) | (controls & 0xffu) | AMCONTROL_USED | AMCONTROL_COLORINFO_PRESENT;
}
HRESULT tag_native_output_color(CMediaType& media,const Layout& layout,DWORD controls) noexcept {
    try {
        if(media.formattype==FORMAT_VideoInfo) {
            if(!media.pbFormat || media.cbFormat<sizeof(VIDEOINFOHEADER))return VFW_E_TYPE_NOT_ACCEPTED;
            const std::vector<BYTE> original(media.pbFormat,media.pbFormat+media.cbFormat);
            const auto* source=reinterpret_cast<const VIDEOINFOHEADER*>(original.data());
            const size_t bitmap_bytes=original.size()-offsetof(VIDEOINFOHEADER,bmiHeader);
            const size_t new_size=offsetof(VIDEOINFOHEADER2,bmiHeader)+bitmap_bytes;
            if(new_size>(std::numeric_limits<ULONG>::max)())return E_OUTOFMEMORY;
            auto* target=reinterpret_cast<VIDEOINFOHEADER2*>(media.AllocFormatBuffer(static_cast<ULONG>(new_size)));
            if(!target)return E_OUTOFMEMORY;
            std::memset(target,0,new_size);
            target->rcSource=source->rcSource;target->rcTarget=source->rcTarget;
            target->dwBitRate=source->dwBitRate;target->dwBitErrorRate=source->dwBitErrorRate;
            target->AvgTimePerFrame=source->AvgTimePerFrame;
            const int divisor=std::gcd(layout.width,layout.height);
            target->dwPictAspectRatioX=layout.width/divisor;
            target->dwPictAspectRatioY=layout.height/divisor;
            std::memcpy(&target->bmiHeader,&source->bmiHeader,bitmap_bytes);
            media.SetFormatType(&FORMAT_VideoInfo2);
        }
        if(media.formattype!=FORMAT_VideoInfo2 || !media.pbFormat || media.cbFormat<sizeof(VIDEOINFOHEADER2))
            return VFW_E_TYPE_NOT_ACCEPTED;
        reinterpret_cast<VIDEOINFOHEADER2*>(media.pbFormat)->dwControlFlags=controls;
        return S_OK;
    } catch(...) {return E_OUTOFMEMORY;}
}

REFERENCE_TIME rate_duration(nvof::Rate rate) {
    return (kUnits * rate.den + rate.num / 2) / rate.num;
}

struct ReleaseSample { void operator()(IMediaSample* p) const { if (p) p->Release(); } };
using SamplePtr = std::unique_ptr<IMediaSample, ReleaseSample>;

class NvofFilter;
class ProbeInputPin final : public CTransformInputPin, public nvof::transport::DecoderConfiguration,
    public nvof::transport::TextureConfiguration, public IMFGetService {
public:
    DECLARE_IUNKNOWN;
    ProbeInputPin(NvofFilter* owner,HRESULT* result);
    STDMETHODIMP NonDelegatingQueryInterface(REFIID id,void** value) override;
    STDMETHODIMP ActivateD3D11Decoding(ID3D11Device*,ID3D11DeviceContext*,HANDLE,UINT) override;
    STDMETHODIMP GetService(REFGUID service,REFIID requested,LPVOID* value) override;
    STDMETHODIMP GetAllocator(IMemAllocator** allocator) override;
    STDMETHODIMP NotifyAllocator(IMemAllocator* allocator,BOOL read_only) override;
    STDMETHODIMP ReceiveConnection(IPin* connector,const AM_MEDIA_TYPE* type) override;
    HRESULT BreakConnect() override;
    IPin* candidate=nullptr; // borrowed only during the synchronous base call
    STDMETHODIMP_(UINT) GetD3D11AdapterIndex() override;
    STDMETHODIMP_(UINT) GetD3D11TextureBindFlags() override {return D3D11_BIND_SHADER_RESOURCE;}
    STDMETHODIMP_(UINT) GetD3D11TextureMiscFlags() override {return D3D11_RESOURCE_MISC_SHARED;}
private:
    NvofFilter* owner_;
    std::atomic<unsigned> observed_queries_{0};
    DiagnosticServiceProbe service_probe_;
    Microsoft::WRL::ComPtr<IMemAllocator> offered_host_allocator_;
    bool first_allocator_pending_=false;
};

class NvofFilter final : public CTransformFilter, public ISpecifyPropertyPages, public IMFGetService,
    public IDirectXVideoMemoryConfiguration {
public:
    DECLARE_IUNKNOWN;

    STDMETHODIMP NonDelegatingQueryInterface(REFIID id, void** value) override {
        if(id==kD3D11DecoderConfiguration || id==kD3D11DecoderTextureConfiguration ||
           id==__uuidof(IMFGetService) || id==__uuidof(IDirect3DDeviceManager9)) {
            const unsigned bit=id==kD3D11DecoderConfiguration?1:id==kD3D11DecoderTextureConfiguration?2:
                id==__uuidof(IMFGetService)?4:8;
            if(!(filter_probe_queries_.fetch_or(bit)&bit))
                write_log("native probe instance="+std::to_string(instance_id_)+" filter-level QI "+(bit==1?"ID3D11DecoderConfiguration":bit==2?
                    "ID3D11DecoderTextureConfiguration":bit==4?"IMFGetService":"IDirect3DDeviceManager9"));
            if(bit<=2)native_input_requested_.store(true,std::memory_order_release);
        }
        if(id==__uuidof(IMFGetService) && service_probe_enabled())
            return GetInterface(static_cast<IMFGetService*>(this),value);
        if(id==__uuidof(IDirectXVideoMemoryConfiguration) && native_feature_enabled_)
            return GetInterface(static_cast<IDirectXVideoMemoryConfiguration*>(this),value);
        if (id == IID_ISpecifyPropertyPages)
            return GetInterface(static_cast<ISpecifyPropertyPages*>(this), value);
        return CTransformFilter::NonDelegatingQueryInterface(id, value);
    }

    bool service_probe_enabled() const noexcept {return probe_services_ || native_feature_enabled_;}
    unsigned instance_id() const noexcept {return instance_id_;}
    void trace_input_allocator(const char* stage,bool offered=false) {
        if(!probe_services_)return;
        write_log(std::string("input allocator stage=")+stage+" nativeCommitted="+std::to_string(native_input_committed())+
            " nativeRequested="+std::to_string(native_input_requested_.load())+" cpuUpload="+std::to_string(cpu_upload_active_)+
            " state="+std::to_string(m_State)+" input10="+std::to_string(input_.ten_bit)+
            " outputConnected="+std::to_string(m_pOutput && m_pOutput->IsConnected())+
            " offered="+std::to_string(offered));
    }
    HRESULT prefer_confirmed_cpu_hdr_input() {
        // Called once from NotifyAllocator, only when this new connection
        // selected the ordinary host allocator that our input pin created.
        // Native selection precedes this callback in PotPlayer. Unknown or
        // decoder-owned allocators never enter here, even with deferred HW.
        if(native_input_committed() || m_State!=State_Stopped || !m_pOutput ||
           !m_pOutput->IsConnected() || !output_.ten_bit || !output_.hdr_transfer || input_.ten_bit)return S_OK;
        CMediaType preferred;
        if(!cpu_hdr_input_type(preferred,true))return S_OK;
        Microsoft::WRL::ComPtr<IFilterGraph2> graph;
        if(!m_pGraph || FAILED(m_pGraph->QueryInterface(IID_PPV_ARGS(&graph))))return E_NOINTERFACE;
        write_log("CPU allocator confirmed; requesting P010 decoder output for retained HDR renderer");
        // Still on the decoder's connection thread, before streaming. Never
        // reconnect from Receive/Run or infer SW from a missing native probe.
        return graph->ReconnectEx(m_pInput,&preferred);
    }
    STDMETHODIMP GetService(REFGUID service,REFIID requested,LPVOID* value) override {
        return get_native_service("filter",service,requested,value);
    }
    bool native_input_committed() const noexcept {return !cpu_upload_active_ && (native_gpu_active_ || native_pending_);}
    HRESULT get_native_service(const char* location,REFGUID service,REFIID requested,void** value) noexcept {
        if(!value)return E_POINTER;
        *value=nullptr;
        if(probe_services_)service_probe_.request(instance_id_,location,service,requested,value);
        if(!native_feature_enabled_ || service!=MR_VIDEO_ACCELERATION_SERVICE)return E_NOINTERFACE;
        if(requested!=__uuidof(IMFDXGIDeviceManager) && requested!=__uuidof(IDirectXVideoMemoryConfiguration))return E_NOINTERFACE;
        native_input_requested_.store(true,std::memory_order_release);
        try {
            if(!GetPin(0))return E_OUTOFMEMORY;
            std::lock_guard<std::mutex> service_lock(mf_service_mutex_);
            {
                CAutoLock receive(&m_csReceive);
                if(requested==__uuidof(IDirectXVideoMemoryConfiguration)) {
                    // This service belongs to the D3D11 manager handshake.
                    // Do not implicitly opt a legacy D3D9 caller into D3D11.
                    if(!mf_bridge_)return E_NOINTERFACE;
                    return GetInterface(static_cast<IDirectXVideoMemoryConfiguration*>(this),value);
                }
                if(mf_bridge_)return mf_bridge_->manager()->QueryInterface(requested,value);
            }
            if(m_State!=State_Stopped)return VFW_E_NOT_STOPPED;
            auto bridge=std::make_shared<nvof::MfD3d11Bridge>(native_adapter());
            CAutoLock receive(&m_csReceive);
            mf_bridge_=std::move(bridge);
            write_log("MF DXGI device manager discovered instance="+std::to_string(instance_id_)+
                "; awaiting decoder surface selection, CPU transport retained");
            return mf_bridge_->manager()->QueryInterface(requested,value);
        }catch(const std::exception& error) {
            write_log(std::string("MF DXGI service unavailable: ")+error.what());return E_FAIL;
        }catch(...) {return E_FAIL;}
    }
    STDMETHODIMP GetAvailableSurfaceTypeByIndex(DWORD index,DXVA2_SurfaceType* type) override {
        if(!type)return E_POINTER;
        *type=DXVA2_SurfaceType_DecoderRenderTarget;
        CAutoLock receive(&m_csReceive);
        if(!native_feature_enabled_ || !mf_bridge_)return VFW_E_WRONG_STATE;
        return index==0?S_OK:MF_E_NO_MORE_TYPES;
    }
    STDMETHODIMP SetSurfaceType(DXVA2_SurfaceType type) override {
        if(type!=DXVA2_SurfaceType_DecoderRenderTarget)return E_INVALIDARG;
        if(m_State!=State_Stopped)return VFW_E_NOT_STOPPED;
        try {
            std::lock_guard<std::mutex> service_lock(mf_service_mutex_);
            std::shared_ptr<nvof::MfD3d11Bridge> bridge;
            {
                CAutoLock receive(&m_csReceive);
                if(!native_feature_enabled_ || !mf_bridge_)return VFW_E_WRONG_STATE;
                if(mf_surface_selected_ && native_input_committed())return S_OK;
                bridge=mf_bridge_;
            }
            const HRESULT result=activate_native(bridge->device(),bridge->context(),bridge->mutex(),0);
            if(FAILED(result)) {
                write_log("MF decoder surface selection rejected HRESULT="+std::to_string(static_cast<unsigned long>(result)));
                return result;
            }
            CAutoLock receive(&m_csReceive);
            if(native_device_.Get()!=bridge->device() || !native_input_committed())return VFW_E_WRONG_STATE;
            mf_bridge_=std::move(bridge);mf_surface_selected_=true;
            write_log("MF decoder surface selected instance="+std::to_string(instance_id_)+
                "; decoder must supply GPU allocator; waiting for valid GPU sample");
            return S_OK;
        }catch(const std::exception& error) {
            write_log(std::string("MF surface selection failed: ")+error.what());return E_FAIL;
        }catch(...) {return E_FAIL;}
    }
    STDMETHODIMP GetPages(CAUUID* pages) override {
        if (!pages) return E_POINTER;
        pages->cElems=0;
        pages->pElems=static_cast<GUID*>(CoTaskMemAlloc(sizeof(GUID)));
        if (!pages->pElems) return E_OUTOFMEMORY;
        pages->pElems[0]=CLSID_NvofPropertyPage;
        pages->cElems=1;
        return S_OK;
    }

    NvofFilter(LPUNKNOWN outer, HRESULT* result)
        : CTransformFilter(NAME("NVIDIA Optical Flow for PotPlayer"), outer, CLSID_NvofPotPlayer) {
        *result = S_OK;
        status_writer_=nvof::acquire_status_writer();
        try {
            directory_ = module_directory();
            const auto ini = directory_ / L"NvofPotPlayer.ini";
            probe_services_ = GetPrivateProfileIntW(L"Nvof", L"ProbeServices", 0, ini.c_str()) != 0;
            performance_.enabled = GetPrivateProfileIntW(L"Nvof", L"TracePerformance", 0, ini.c_str()) != 0;
            trace_every_gpu_pair_ = GetPrivateProfileIntW(L"Nvof", L"TracePerformance", 0, ini.c_str()) >= 2;
            native_feature_enabled_ = GetPrivateProfileIntW(L"Nvof", L"NativeD3D11", 0, ini.c_str()) != 0;
            async_input_enabled_ = GetPrivateProfileIntW(L"Nvof", L"AsyncNativeInput", 1, ini.c_str()) != 0;
            enabled_ = GetPrivateProfileIntW(L"Nvof", L"Enabled", 1, ini.c_str()) != 0;
            // Historical INI keys cannot re-enable the removed FRUC backend.
            gpu_correction_=false;appearance_protection_=false;native_synthesis_=true;

            input_rate_mask_=GetPrivateProfileIntW(L"Nvof",L"InputRateMask",nvof::kAllInputRates,ini.c_str()) & nvof::kAllInputRates;
            input_height_limit_=nvof::normalize_height_limit(GetPrivateProfileIntW(L"Nvof",L"InputHeightLimit",0,ini.c_str()));
            // Integer multiples only; an absent/invalid limit means 60 fps.
            output_limit_=nvof::normalize_output_limit(int(GetPrivateProfileIntW(L"Nvof",L"OutputFpsLimit",60,ini.c_str())));
            output_mode_=nvof::normalize_output_mode(int(GetPrivateProfileIntW(L"Nvof",L"OutputMode",0,ini.c_str())));
            output_multiplier_=nvof::normalize_output_multiplier(int(GetPrivateProfileIntW(L"Nvof",L"OutputMultiplier",2,ini.c_str())));
            resolution_rules_enabled_=GetPrivateProfileIntW(L"Nvof",L"ResolutionRulesEnabled",0,ini.c_str())!=0;
            wchar_t resolution_text[256]{};
            GetPrivateProfileStringW(L"Nvof",L"ResolutionRules",L"",resolution_text,_countof(resolution_text),ini.c_str());
            std::string resolution_ascii;
            for(wchar_t c:resolution_text){if(!c)break;resolution_ascii.push_back(c<128?char(c):'?');}
            resolution_rules_=nvof::parse_resolution_rules(resolution_ascii);
            target_ = {0, 1};
            write_log("filter created instance="+std::to_string(instance_id_)+" NativeD3D11="+
                std::to_string(native_feature_enabled_)+" ProbeServices="+std::to_string(probe_services_));
        } catch (const std::exception& e) {
            write_log(std::string("initialization failed: ") + e.what());
            *result = E_FAIL;
        }
    }

    ~NvofFilter() override {
        cancelled_.store(true, std::memory_order_release);
        cancel_async_input();
        join_async_input();
        // Graph ownership must already have stopped streaming before Release.
        // Keep a final serialized cleanup for partially connected graphs.
        CAutoLock receive(&m_csReceive);
        release_native_activation();
    }

    static CUnknown* WINAPI CreateInstance(LPUNKNOWN outer, HRESULT* result) {
        if (!result) return nullptr;
        try {
            auto* filter = new (std::nothrow) NvofFilter(outer, result);
            if (!filter) *result = E_OUTOFMEMORY;
            return filter;
        } catch (...) {
            *result = E_OUTOFMEMORY;
            return nullptr;
        }
    }

    CBasePin* GetPin(int index) override {
        if (index<0 || index>1) return nullptr;
        CAutoLock lock(&m_csFilter);
        if (!m_pInput) {
            try {
                HRESULT result=S_OK;
                std::unique_ptr<ProbeInputPin> input(new(std::nothrow) ProbeInputPin(this,&result));
                if (!input || FAILED(result)) return nullptr;
                std::unique_ptr<nvof::transport::OutputPin> output(new(std::nothrow) nvof::transport::OutputPin(this,&result));
                if (!output || FAILED(result)) return nullptr;
                m_pInput=input.release();
                m_pOutput=output.release();
            } catch (...) { return nullptr; }
        }
        return index==0 ? static_cast<CBasePin*>(m_pInput) : static_cast<CBasePin*>(m_pOutput);
    }

    void note_native_input_query(const char* interface_name,HRESULT result,bool d3d11=true) noexcept {
        if(d3d11)native_input_requested_.store(true,std::memory_order_release);
        try {
            write_log("native probe instance="+std::to_string(instance_id_)+" upstream queried "+interface_name+
                "; effective QueryInterface HRESULT="+std::to_string(static_cast<unsigned long>(result)));
        } catch (...) { }
    }

    bool native_renderer_ready() const noexcept {
        return native_feature_enabled_;
    }

    Microsoft::WRL::ComPtr<nvof::transport::DecoderConfiguration> native_renderer() {
        Microsoft::WRL::ComPtr<nvof::transport::DecoderConfiguration> renderer;
        IPin* raw=nullptr;
        {
            CAutoLock lock(&m_csFilter);
            if(!native_renderer_ready() || !m_pOutput || FAILED(m_pOutput->ConnectedTo(&raw)))return renderer;
        }
        raw->QueryInterface(IID_PPV_ARGS(&renderer));
        raw->Release();
        return renderer;
    }

    UINT native_adapter() {
        auto renderer=native_renderer();
        const UINT hint=renderer?renderer->GetD3D11AdapterIndex():UINT_MAX;
        std::optional<uint64_t> retained;
        {CAutoLock receive(&m_csReceive);if(renderer_gpu_committed_)retained=renderer_adapter_luid_;}
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            throw std::runtime_error("Cannot enumerate NVIDIA adapters");
        std::vector<nvof::AdapterCandidate> candidates;
        for(UINT index=0;;++index) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            const HRESULT next=factory->EnumAdapters1(index,&adapter);
            if(next==DXGI_ERROR_NOT_FOUND)break;
            if(FAILED(next) || !adapter)throw std::runtime_error("DXGI adapter enumeration failed");
            DXGI_ADAPTER_DESC1 description{};
            if(FAILED(adapter->GetDesc1(&description)))throw std::runtime_error("Cannot identify DXGI adapter");
            candidates.push_back({index,adapter_luid(description.AdapterLuid),description.VendorId,
                bool(description.Flags&DXGI_ADAPTER_FLAG_SOFTWARE),wcsstr(description.Description,L"5090")!=nullptr});
        }
        const auto selected=nvof::select_nvidia_adapter(candidates,hint,retained);
        if(!selected)throw std::runtime_error(retained?"Retained renderer NVIDIA adapter is unavailable":"No NVIDIA D3D11 hardware adapter");
        // Connection-time only; no additional work/logging on the frame path.
        if(selected->index!=hint && renderer) {
            write_log("adapter selection: rendererHint="+std::to_string(hint)+
                " selectedNvidiaIndex="+std::to_string(selected->index)+
                " reason="+(retained?"retained-renderer-luid":"validated-nvidia-fallback"));
        }
        return selected->index;
    }

    static uint64_t adapter_luid(LUID luid) noexcept {
        return (uint64_t(uint32_t(luid.HighPart))<<32)|luid.LowPart;
    }
    static std::optional<uint64_t> device_adapter_luid(ID3D11Device* device) noexcept {
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC desc{};
        if(!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) ||
           FAILED(dxgi->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&desc)) || desc.VendorId!=0x10de)return {};
        return adapter_luid(desc.AdapterLuid);
    }

    HRESULT activate_native(ID3D11Device* device,ID3D11DeviceContext* context,HANDLE mutex,UINT flags) {
        if(!device || !context || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return E_INVALIDARG;
        if(!native_feature_enabled_)return E_NOINTERFACE;
        if(m_State!=State_Stopped)return VFW_E_NOT_STOPPED;
        if((input_.interlaced || (input_.ten_bit && input_.cpu_format!=nvof::CpuVideoFormat::p010)) && !starting_cpu_upload_)
            return VFW_E_TYPE_NOT_ACCEPTED;
        if(!input_.native_color_supported) {
            write_log("native activation rejected: unsupported native color metadata (expected limited BT.709 SDR or BT.2020 PQ/HLG)");
            return VFW_E_TYPE_NOT_ACCEPTED;
        }
        auto renderer=native_renderer();
        // Upstream PostConnect can run before the renderer has been connected.
        // A validated device may wait in a pending state, but no GPU samples
        // may be processed until downstream activation succeeds.
        if(m_pOutput && m_pOutput->IsConnected() && !renderer)return E_NOINTERFACE;
        Microsoft::WRL::ComPtr<ID3D11Device> context_device;
        context->GetDevice(&context_device);
        if(context_device.Get()!=device)return E_INVALIDARG;
        const auto device_luid=device_adapter_luid(device);
        if(!device_luid)return VFW_E_TYPE_NOT_ACCEPTED;
        HANDLE duplicate=nullptr;
        if(mutex && !DuplicateHandle(GetCurrentProcess(),mutex,GetCurrentProcess(),&duplicate,0,FALSE,DUPLICATE_SAME_ACCESS))
            return HRESULT_FROM_WIN32(GetLastError());
        std::shared_ptr<void> duplicate_owner;
        try {duplicate_owner=std::shared_ptr<void>(duplicate,[](void* value){if(value)CloseHandle(value);});}
        catch(...) {return E_OUTOFMEMORY;}
        CAutoLock receive(&m_csReceive);
        try {
            auto engine=std::make_unique<nvof::GpuFrucEngine>(directory_/L"runtime",device,context,duplicate,nvof::GpuCompletionMode::context_ordered,true,gpu_correction_,appearance_protection_,nvof::GpuInterpolationBackend::native_experimental);
            auto* output=static_cast<nvof::transport::OutputPin*>(m_pOutput);
            const HRESULT transport=output ? output->set_gpu_mode(true) : E_UNEXPECTED;
            if(FAILED(transport)) {
                engine.reset();return transport;
            }
            const HRESULT result=renderer ? renderer->ActivateD3D11Decoding(device,context,duplicate,flags) : S_OK;
            if(FAILED(result)) {
                output->set_gpu_mode(native_gpu_active_);
                engine.reset();
                write_log("native activation rejected by renderer HRESULT="+std::to_string(static_cast<unsigned long>(result)));
                return result;
            }
            if(renderer) {renderer_mutex_lifetime_=duplicate_owner;renderer_gpu_committed_=true;renderer_adapter_luid_=device_luid;}
            release_native_activation(false);
            native_device_=device;native_context_=context;native_mutex_=duplicate;
            native_mutex_lifetime_=std::move(duplicate_owner);
            native_flags_=flags;
            gpu_engine_=std::move(engine);
            native_pending_=!renderer;
            native_gpu_active_=!!renderer;
            try {write_log(native_pending_ ? "native device validated; activation pending renderer connection" :
                "native D3D11 activation accepted; input and output remain on GPU");}catch(...){}
            return S_OK;
        } catch(const std::exception& error) {
            write_log(std::string("native activation unavailable: ")+error.what());
            return E_FAIL;
        } catch(...) {
            return E_FAIL;
        }
    }

    HRESULT activate_cpu_upload(bool include_nv12=false) {
        if(native_feature_enabled_ && (input_.ten_bit || (include_nv12 && input_.native_color_supported)) && !native_input_committed() && !cpu_upload_active_) {
            try {
                auto bridge=std::make_shared<nvof::MfD3d11Bridge>(native_adapter());
                starting_cpu_upload_=true;
                const HRESULT hr=activate_native(bridge->device(),bridge->context(),bridge->mutex(),0);
                starting_cpu_upload_=false;
                if(FAILED(hr)) {
                    // Preserve the legacy CPU path for a renderer that never
                    // accepted GPU transport. A retained native renderer must
                    // not receive host buffers after a failed reactivation.
                    if(!input_.ten_bit && !renderer_gpu_committed_)return S_OK;
                    return hr;
                }
                cpu_bridge_=std::move(bridge);cpu_upload_active_=true;
                write_log(std::string("CPU ")+(input_.ten_bit?"10-bit":"NV12")+
                    " upload activated; progressive "+(input_.ten_bit?"P010":"NV12")+
                    " GPU output, fields="+std::to_string(input_.interlaced?2:1));
            }catch(const std::exception& e) {starting_cpu_upload_=false;write_log(std::string("CPU upload activation failed: ")+e.what());return E_FAIL;}
        }
        return S_OK;
    }

    HRESULT CompleteConnect(PIN_DIRECTION direction,IPin* connected) override {
        if(topology_logs_.fetch_add(1)<16)log_endpoint(instance_id_,"CompleteConnect",direction,connected);
        if (direction==PINDIR_OUTPUT && connected) {
            IUnknown* configuration=nullptr;
            const HRESULT result=connected->QueryInterface(kD3D11DecoderConfiguration,
                reinterpret_cast<void**>(&configuration));
            native_renderer_supported_.store(SUCCEEDED(result) && configuration,std::memory_order_release);
            if (configuration) configuration->Release();
            if(native_feature_enabled_ && FAILED(result)) {
                write_log("native renderer connection rejected: downstream lacks D3D11 configuration; refusing renderer fallback");
                write_status("error","Native mode requires a D3D11 renderer; renderer fallback was refused");
                return VFW_E_TYPE_NOT_ACCEPTED;
            }
            if(native_pending_) {
                const auto device_luid=device_adapter_luid(native_device_.Get());
                if(!device_luid)return VFW_E_TYPE_NOT_ACCEPTED;
                Microsoft::WRL::ComPtr<nvof::transport::DecoderConfiguration> renderer;
                HRESULT activated=connected->QueryInterface(IID_PPV_ARGS(&renderer));
                if(SUCCEEDED(activated))activated=renderer->ActivateD3D11Decoding(native_device_.Get(),
                    native_context_.Get(),native_mutex_,native_flags_);
                if(FAILED(activated)) {
                    write_log("pending native activation rejected by output HRESULT="+
                        std::to_string(static_cast<unsigned long>(activated)));
                    return activated;
                }
                {
                    CAutoLock receive(&m_csReceive);
                    renderer_mutex_lifetime_=native_mutex_lifetime_;
                    renderer_gpu_committed_=true;
                    renderer_adapter_luid_=device_luid;
                    native_pending_=false;
                    native_gpu_active_=true;
                }
                write_log("pending native activation completed by compatible renderer");
            }
            try {
                write_log("native probe: renderer input ID3D11DecoderConfiguration HRESULT="+
                    std::to_string(static_cast<unsigned long>(result))+
                    "; native activation is reported separately");
            } catch (...) { }
        }
        // Establish GPU allocator BEFORE the renderer negotiates buffers. The
        // built-in renderer can reject ordinary host buffers for both NV12
        // and P010. Waiting until Run loses the SW-only graph before streaming
        // starts; a prior HW connection must not be required to prime it.
        if(direction==PINDIR_OUTPUT) {
            const HRESULT uploaded=activate_cpu_upload(true);if(FAILED(uploaded))return uploaded;
        }
        const HRESULT completed=CTransformFilter::CompleteConnect(direction,connected);
        if(FAILED(completed))write_log("CompleteConnect allocator failed HRESULT="+std::to_string(uint32_t(completed)));
        // A retained output does not identify the new decoder transport.
        // PotPlayer can select native surfaces AFTER the input pin connects.
        // Reconnecting that input here forced P010 onto its pending HW path
        // and repeated switches stopped delivering samples (hdrsw.3).
        if(SUCCEEDED(completed) && direction==PINDIR_OUTPUT && cpu_upload_active_ && output_.hdr_transfer &&
           output_.ten_bit && !input_.ten_bit && !native_input_requested_.load(std::memory_order_acquire)) {
            CMediaType preferred;
            if(cpu_hdr_input_type(preferred)) {
                Microsoft::WRL::ComPtr<IFilterGraph2> graph;
                if(!m_pGraph || FAILED(m_pGraph->QueryInterface(IID_PPV_ARGS(&graph))))return E_NOINTERFACE;
                // DirectShow queues this reconnection until the current output
                // Connect returns. Request it on the connection thread, only
                // after the decoder has explicitly accepted the proposed type.
                write_log("CPU HDR requesting P010 decoder output before streaming");
                return graph->ReconnectEx(m_pInput,&preferred);
            }
            return VFW_E_TYPE_NOT_ACCEPTED; // Decoder withdrew its P010 agreement.
        }
        return completed;
    }

    HRESULT BreakConnect(PIN_DIRECTION direction) override {
        {
            CAutoLock receive(&m_csReceive);
            if(direction==PINDIR_INPUT)release_native_activation();
            else {
                reset_history();
                native_gpu_active_=false;
                native_pending_=!!native_device_;
                renderer_mutex_lifetime_.reset();
                renderer_gpu_committed_=false;
                renderer_adapter_luid_.reset();
            }
        }
        if (direction==PINDIR_OUTPUT) native_renderer_supported_.store(false,std::memory_order_release);
        return CTransformFilter::BreakConnect(direction);
    }

    nvof::Rate source_rate(REFERENCE_TIME duration) const noexcept {
        return duration>0?nvof::Rate{kUnits,duration}:nvof::Rate{0,1};
    }
    bool rate_selected(REFERENCE_TIME duration) const noexcept {
        return nvof::input_rate_selected(source_rate(duration),input_rate_mask_);
    }
    nvof::ResolutionOutput resolution_output(int height) const {
        return nvof::resolve_resolution_output(height,resolution_rules_enabled_,resolution_rules_,output_mode_,output_limit_,output_multiplier_);
    }
    nvof::Rate requested_target(REFERENCE_TIME duration,int height) const {
        const auto output=resolution_output(height);
        return output.disabled?nvof::canonical_source_rate(duration):
            nvof::selected_output_rate(duration,output.mode,output.limit,output.multiplier);
    }
    nvof::Rate requested_target(REFERENCE_TIME duration) const {
        return requested_target(duration,input_.height);
    }
    int selected_multiple(REFERENCE_TIME duration) const {
        const auto output=resolution_output(input_.height);
        return output.disabled?1:nvof::selected_output_multiple(duration,output.mode,output.limit,output.multiplier);
    }
    bool same_target(REFERENCE_TIME duration) const {
        const auto incoming=requested_target(duration);
        // Output-rate bounds keep both cross-products below 1e16. A rounded
        // AvgTimePerFrame match alone is insufficient for the x2 clock.
        return incoming.num*target_.den==target_.num*incoming.den;
    }
    bool target_available(REFERENCE_TIME duration,int height) const {
        return nvof::supported_output_rate(requested_target(duration,height));
    }
    bool source_reaches_target(REFERENCE_TIME duration,int height) const {
        const auto target=requested_target(duration,height);
        return duration>0 && nvof::supported_output_rate(target) && duration<=rate_duration(target)+1;
    }
    bool interpolate_source(REFERENCE_TIME duration,int height) const {
        return enabled_ && nvof::input_height_selected(height,input_height_limit_) && rate_selected(duration) &&
            !resolution_output(height).disabled && target_available(duration,height) && !source_reaches_target(duration,height);
    }
    bool interpolation_requested() const {return interpolate_source(input_.duration,input_.height);}
    REFERENCE_TIME negotiated_duration(REFERENCE_TIME duration,int height) const {
        return interpolate_source(duration,height)?rate_duration(requested_target(duration,height)):duration;
    }
    const char* bypass_reason() const {
        if(!enabled_)return "disabled";
        if(!input_rate_selected_)return "source-rate";
        if(!nvof::input_height_selected(input_.height,input_height_limit_))return "source-height";
        if(resolution_output(input_.height).disabled)return "resolution-rule";
        if(!target_available(input_.duration,input_.height))return input_.duration<=0?"unknown-source-rate":"output-rate-unsupported";
        if(source_reaches_target(input_.duration,input_.height))return "at-or-above-target";
        return "";
    }
    const char* bypass_message() const {
        if(!enabled_)return "Interpolation disabled";
        if(!input_rate_selected_)return "Source frame rate excluded; original frames preserved";
        if(!nvof::input_height_selected(input_.height,input_height_limit_))return "Source height excluded; original frames preserved";
        if(resolution_output(input_.height).disabled)return "Resolution rule disabled interpolation; original frames preserved";
        if(!target_available(input_.duration,input_.height))return input_.duration<=0?
            "Source frame rate is unknown; original frames preserved":
            "Output frame rate exceeds supported timing bounds; original frames preserved";
        return "No integer multiple fits the selected limit; original frames preserved";
    }
    const char* input_rate_group() const noexcept {
        switch(nvof::source_rate_bucket(source_rate(input_.duration))) {
        case nvof::SourceRateBucket::Fps24:return "24";
        case nvof::SourceRateBucket::Fps25:return "25";
        case nvof::SourceRateBucket::Fps30:return "30";
        case nvof::SourceRateBucket::Fps50:return "50";
        case nvof::SourceRateBucket::Fps60:return "60";
        default:return "other";
        }
    }
    DWORD resolved_output_color_flags(const CMediaType& input) const noexcept {
        const DWORD fallback=native_output_color_flags(input);
        // The raw decoder type is authoritative whenever it contains color.
        // Never replace an explicitly tone-mapped SDR type with source HDR.
        if(input.formattype==FORMAT_VideoInfo2 && input.pbFormat && input.cbFormat>=sizeof(VIDEOINFOHEADER2)) {
            const DWORD flags=reinterpret_cast<const VIDEOINFOHEADER2*>(input.pbFormat)->dwControlFlags;
            if(flags & AMCONTROL_COLORINFO_PRESENT) {
                DXVA2_ExtendedFormat color{};color.value=flags;
                if(color.VideoTransferFunction || color.VideoPrimaries || color.VideoTransferMatrix)return fallback;
            }
        }
        // Some native decoders expose untagged legacy NV12 media types,
        // while the samples contain P010 + HDR side data. Read only their
        // connected compressed-video sequence header; no filename/GPU guesses.
        try {
            Microsoft::WRL::ComPtr<IPin> upstream;
            if(!m_pInput)return fallback;
            // During an input-only SW/HW reconnect, CheckTransform runs
            // before ConnectedTo is available. Resolve against the proposing
            // decoder, not a cached color from the previous connection.
            if(FAILED(m_pInput->ConnectedTo(&upstream)))upstream=static_cast<ProbeInputPin*>(m_pInput)->candidate;
            if(!upstream)return fallback;
            PIN_INFO info{};if(FAILED(upstream->QueryPinInfo(&info)) || !info.pFilter)return fallback;
            Microsoft::WRL::ComPtr<IBaseFilter> decoder;decoder.Attach(info.pFilter);
            Microsoft::WRL::ComPtr<IEnumPins> pins;if(FAILED(decoder->EnumPins(&pins)))return fallback;
            DWORD recovered=0;Microsoft::WRL::ComPtr<IPin> pin;
            for(unsigned n=0;n<64 && pins->Next(1,&pin,nullptr)==S_OK;++n,pin.Reset()) {
                PIN_DIRECTION dir{};CMediaType media;
                if(FAILED(pin->QueryDirection(&dir)) || dir!=PINDIR_INPUT || FAILED(pin->ConnectionMediaType(&media)) ||
                   media.majortype!=MEDIATYPE_Video || !media.pbFormat)continue;
                const GUID hvc1={MAKEFOURCC('H','V','C','1'),0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
                GUID hevc=hvc1;hevc.Data1=MAKEFOURCC('H','E','V','C');
                GUID hev1=hvc1;hev1.Data1=MAKEFOURCC('H','E','V','1');
                GUID lower=hvc1;lower.Data1=MAKEFOURCC('h','v','c','1');
                GUID av01=hvc1;av01.Data1=MAKEFOURCC('A','V','0','1');
                GUID av01lower=hvc1;av01lower.Data1=MAKEFOURCC('a','v','0','1');
                GUID vp90=hvc1;vp90.Data1=MAKEFOURCC('V','P','9','0');
                GUID vp90lower=hvc1;vp90lower.Data1=MAKEFOURCC('v','p','9','0');
                GUID vp09=hvc1;vp09.Data1=MAKEFOURCC('v','p','0','9');
                const bool av1=media.subtype==av01 || media.subtype==av01lower;
                const bool vp9=media.subtype==vp90 || media.subtype==vp90lower || media.subtype==vp09;
                const bool h265=media.subtype==hvc1 || media.subtype==hevc || media.subtype==hev1 || media.subtype==lower;
                if(!av1 && !h265 && !vp9)continue;
                const BYTE* sequence=nullptr;size_t length=0;
                if(media.formattype==FORMAT_MPEG2Video && media.cbFormat>=offsetof(MPEG2VIDEOINFO,dwSequenceHeader)) {
                    const auto* header=reinterpret_cast<const MPEG2VIDEOINFO*>(media.pbFormat);
                    length=header->cbSequenceHeader;
                    if(length>media.cbFormat-offsetof(MPEG2VIDEOINFO,dwSequenceHeader))continue;
                    sequence=reinterpret_cast<const BYTE*>(header->dwSequenceHeader);
                }
                // AV1/VP9 splitters may append av1C/configOBUs or vpcC to VideoInfo(2).
                if((av1 || vp9) && !sequence) {
                    size_t offset=0;
                    if(media.formattype==FORMAT_VideoInfo)offset=sizeof(VIDEOINFOHEADER);
                    else if(media.formattype==FORMAT_VideoInfo2)offset=sizeof(VIDEOINFOHEADER2);
                    if(offset && media.cbFormat>offset){sequence=media.pbFormat+offset;length=media.cbFormat-offset;}
                }
                if(probe_services_ && !color_probe_logged_) try {
                    write_log("compressed color probe: subtype="+guid_text(media.subtype)+" format="+guid_text(media.formattype)+
                        " formatBytes="+std::to_string(media.cbFormat)+" sequenceBytes="+std::to_string(length));
                    if(vp9 && media.cbFormat<=65536) {
                        wchar_t local[32768]{};
                        const DWORD count=GetEnvironmentVariableW(L"LOCALAPPDATA",local,_countof(local));
                        if(count && count<_countof(local)) {
                            std::ofstream dump(std::filesystem::path(local)/L"NvofPotPlayer"/L"vp9-color-format.bin",std::ios::binary);
                            dump.write(reinterpret_cast<const char*>(media.pbFormat),media.cbFormat);
                        }
                    }
                    if(sequence && length<=65536) {
                        // Opt-in local diagnostic only; contains codec header, never frames/URLs.
                        wchar_t local[32768]{};
                        const DWORD count=GetEnvironmentVariableW(L"LOCALAPPDATA",local,_countof(local));
                        if(!count || count>=_countof(local))continue;
                        std::ofstream dump(std::filesystem::path(local)/L"NvofPotPlayer"/L"compressed-color-header.bin",std::ios::binary);
                        dump.write(reinterpret_cast<const char*>(sequence),static_cast<std::streamsize>(length));
                    }
                    color_probe_logged_=true;
                }catch(...){color_probe_logged_=true;}
                std::optional<nvof::HevcColor> color;
                if(h265)color=nvof::hevc_color(sequence,length);
                if(av1) {
                    auto a=nvof::av1_color(sequence,length);
                    if(a)color=nvof::HevcColor{a->primaries,a->transfer,a->matrix,a->full_range};
                }
                if(vp9) {
                    auto v=nvof::vp9_color(sequence,length);
                    if(v)color=nvof::HevcColor{v->primaries,v->transfer,v->matrix,v->full_range};
                    // Matroska splitters can convey container colour directly on
                    // the compressed VideoInfo2 type, without any vpcC record.
                    if((media.formattype==FORMAT_VideoInfo2 && media.cbFormat>=sizeof(VIDEOINFOHEADER2)) ||
                       (media.formattype==FORMAT_MPEG2Video && media.cbFormat>=sizeof(MPEG2VIDEOINFO))) {
                        const auto* video=reinterpret_cast<const VIDEOINFOHEADER2*>(media.pbFormat);
                        if(video->dwControlFlags & AMCONTROL_COLORINFO_PRESENT) {
                            DXVA2_ExtendedFormat c{};c.value=video->dwControlFlags;
                            if(c.VideoTransferFunction || c.VideoPrimaries || c.VideoTransferMatrix) {
                                if(c.VideoPrimaries!=MFVideoPrimaries_BT2020 || c.VideoTransferMatrix!=MFVideoTransferMatrix_BT2020_10 ||
                                   (c.VideoTransferFunction!=MFVideoTransFunc_2084 && c.VideoTransferFunction!=MFVideoTransFunc_HLG) ||
                                   (c.NominalRange!=DXVA2_NominalRange_Unknown && c.NominalRange!=DXVA2_NominalRange_16_235))return fallback;
                                const unsigned transfer=c.VideoTransferFunction==MFVideoTransFunc_2084?16:18;
                                if(color && (color->primaries!=9 || color->matrix!=9 || color->transfer!=transfer || color->full_range))return fallback;
                                color=nvof::HevcColor{9,transfer,9,false};
                            }
                        }
                    }
                }
                if(!color || color->full_range || color->primaries!=9 || color->matrix!=9 || (color->transfer!=16 && color->transfer!=18))continue;
                DXVA2_ExtendedFormat hdr{};hdr.NominalRange=DXVA2_NominalRange_16_235;
                hdr.VideoPrimaries=MFVideoPrimaries_BT2020;hdr.VideoTransferMatrix=MFVideoTransferMatrix_BT2020_10;
                hdr.VideoTransferFunction=color->transfer==16?MFVideoTransFunc_2084:MFVideoTransFunc_HLG;
                const DWORD value=(hdr.value & 0xffffff00u) | (fallback & 0xffu);
                if(recovered && recovered!=value)return fallback; // Conflicting active video inputs.
                recovered=value;
            }
            return recovered?recovered:fallback;
        }catch(...){return fallback;}
    }
    bool connected_output_color_matches(const CMediaType& input) const noexcept {
        if(!native_feature_enabled_)return true;
        if(!m_pOutput)return false;
        const auto& output=m_pOutput->CurrentMediaType();
        return output.formattype==FORMAT_VideoInfo2 && output.pbFormat && output.cbFormat>=sizeof(VIDEOINFOHEADER2) &&
            reinterpret_cast<const VIDEOINFOHEADER2*>(output.pbFormat)->dwControlFlags==resolved_output_color_flags(input);
    }
    bool cpu_hdr_input_type(CMediaType& preferred,bool confirmed_host_allocator=false) const noexcept {
        // Native decoders may legitimately advertise NV12 over P010 textures.
        // Do not renegotiate a committed native decoder. During initial graph
        // building, also let a native-service probe finish before preferring a
        // host format. An uncommitted input alone does not establish SW mode.
        // Never promote explicitly tagged SDR to HDR.
        if(!native_feature_enabled_ || input_.ten_bit || native_input_committed() ||
           (!confirmed_host_allocator && native_input_requested_.load(std::memory_order_acquire)) ||
           m_State!=State_Stopped || !m_pInput)return false;
        try {
            Microsoft::WRL::ComPtr<IPin> decoder;
            if(FAILED(m_pInput->ConnectedTo(&decoder)))return false;
            preferred=m_pInput->CurrentMediaType();
            if(FAILED(tag_native_output_color(preferred,input_,resolved_output_color_flags(preferred))))return false;
            Layout tagged;
            if(!parse_layout(preferred,tagged) || !tagged.hdr_transfer || !tagged.native_color_supported)return false;
            auto* h=reinterpret_cast<VIDEOINFOHEADER2*>(preferred.pbFormat);
            preferred.SetSubtype(&nvof::p010_subtype);
            h->bmiHeader.biCompression=MAKEFOURCC('P','0','1','0');h->bmiHeader.biBitCount=24;
            const int64_t bytes=int64_t(input_.pitch)*input_.height*3;
            if(bytes<=0 || bytes>LONG_MAX)return false;
            h->bmiHeader.biSizeImage=DWORD(bytes);preferred.SetSampleSize(ULONG(bytes));
            return decoder->QueryAccept(&preferred)==S_OK;
        }catch(...){return false;}
    }
    nvof::AvcFormat upstream_avc_format() const noexcept {
        try {
            Microsoft::WRL::ComPtr<IPin> upstream;
            if(!m_pInput)return {};
            if(FAILED(m_pInput->ConnectedTo(&upstream)))upstream=static_cast<ProbeInputPin*>(m_pInput)->candidate;
            if(!upstream)return {};
            PIN_INFO info{};if(FAILED(upstream->QueryPinInfo(&info)) || !info.pFilter)return {};
            Microsoft::WRL::ComPtr<IBaseFilter> decoder;decoder.Attach(info.pFilter);
            Microsoft::WRL::ComPtr<IEnumPins> pins;if(FAILED(decoder->EnumPins(&pins)))return {};
            Microsoft::WRL::ComPtr<IPin> pin;
            for(unsigned i=0;i<64 && pins->Next(1,&pin,nullptr)==S_OK;++i,pin.Reset()) {
                PIN_DIRECTION dir{};CMediaType mt;
                if(FAILED(pin->QueryDirection(&dir)) || dir!=PINDIR_INPUT || FAILED(pin->ConnectionMediaType(&mt)) || !mt.pbFormat)continue;
                const auto c=mt.subtype.Data1;
                if(c!=MAKEFOURCC('A','V','C','1') && c!=MAKEFOURCC('a','v','c','1') &&
                   c!=MAKEFOURCC('H','2','6','4') && c!=MAKEFOURCC('h','2','6','4'))continue;
                if(mt.formattype==FORMAT_MPEG2Video && mt.cbFormat>=offsetof(MPEG2VIDEOINFO,dwSequenceHeader)) {
                    const auto* h=reinterpret_cast<const MPEG2VIDEOINFO*>(mt.pbFormat);
                    if(h->cbSequenceHeader<=mt.cbFormat-offsetof(MPEG2VIDEOINFO,dwSequenceHeader))
                        return nvof::avc_format(reinterpret_cast<const BYTE*>(h->dwSequenceHeader),h->cbSequenceHeader);
                }
            }
        }catch(...){}
        return {};
    }
    bool parse_input_layout(const CMediaType& media,Layout& layout) const noexcept {
        if(!parse_layout(media,layout))return false;
        if(layout.interlaced && upstream_avc_format().frame_mbs_only==true) {
            layout.interlaced=false;layout.top_first=false;layout.duration=layout.sample_duration;
        }
        return true;
    }
    HRESULT CheckInputType(const CMediaType* type) override {
        if (!type) return E_POINTER;
        Layout layout;
        // Preserve actual 10-bit decoder output instead of accepting an implicit
        // NV12 downgrade. Applies only to verified AVC SPS, not legacy native
        // HEVC/AV1 media types that can advertise NV12 over a P010 surface.
        if(type->subtype==MEDIASUBTYPE_NV12 && native_feature_enabled_ && !native_input_committed() && upstream_avc_format().depth>8)
            return VFW_E_TYPE_NOT_ACCEPTED;
        return parse_input_layout(*type, layout) && (!(layout.ten_bit || layout.hdr_transfer) || native_feature_enabled_) ? S_OK : VFW_E_TYPE_NOT_ACCEPTED;
    }

    HRESULT CheckTransform(const CMediaType* input, const CMediaType* output) override {
        if (!input || !output) return E_POINTER;
        Layout in, out;
        if (!parse_input_layout(*input, in) || !parse_layout(*output, out) ||
            out.interlaced || (output->subtype!=MEDIASUBTYPE_NV12 && output->subtype!=nvof::p010_subtype) ||
            in.width != out.width || in.height != out.height || (in.ten_bit && !out.ten_bit) || (!native_feature_enabled_ && in.ten_bit!=out.ten_bit))
            return VFW_E_TYPE_NOT_ACCEPTED;
        if(native_feature_enabled_) {
            if(output->formattype!=FORMAT_VideoInfo2)return VFW_E_TYPE_NOT_ACCEPTED;
            const auto* header=reinterpret_cast<const VIDEOINFOHEADER2*>(output->pbFormat);
            if(header->dwControlFlags!=resolved_output_color_flags(*input))return VFW_E_TYPE_NOT_ACCEPTED;
        }
        const auto duration = negotiated_duration(in.duration,in.height);
        if ((duration>0 && out.duration<=0) || std::llabs(out.duration - duration) > 1)
            return VFW_E_TYPE_NOT_ACCEPTED;
        return S_OK;
    }

    HRESULT SetMediaType(PIN_DIRECTION direction, const CMediaType* type) override {
        if (!type) return E_POINTER;
        Layout layout;
        if (!(direction==PINDIR_INPUT?parse_input_layout(*type,layout):parse_layout(*type,layout))) return VFW_E_TYPE_NOT_ACCEPTED;
        if (direction == PINDIR_INPUT) {
            if((native_gpu_active_ || native_pending_) && !layout.native_color_supported)return VFW_E_TYPE_NOT_ACCEPTED;
            input_ = layout;
            target_ = requested_target(input_.duration);
            input_rate_selected_=rate_selected(input_.duration);
        } else output_ = layout;
        return CTransformFilter::SetMediaType(direction, type);
    }

    HRESULT GetMediaType(int index, CMediaType* media) override {
        if (!media) return E_POINTER;
        if (index < 0) return E_INVALIDARG;
        if (index > 0) return VFW_S_NO_MORE_ITEMS;
        if (!m_pInput || !m_pInput->IsConnected()) return VFW_E_NOT_CONNECTED;
        HRESULT hr = media->Set(m_pInput->CurrentMediaType());
        if (FAILED(hr)) return hr;
        if(native_feature_enabled_) {
            hr=tag_native_output_color(*media,input_,resolved_output_color_flags(*media));
            if(FAILED(hr))return hr;
        }
        CMediaType preferred;
        const bool output_ten_bit=input_.ten_bit || cpu_hdr_input_type(preferred);
        BITMAPINFOHEADER* bitmap = nullptr;
        RECT* source = nullptr;
        RECT* destination = nullptr;
        DWORD* bit_rate = nullptr;
        const REFERENCE_TIME duration = negotiated_duration(input_.duration,input_.height);
        if (media->formattype == FORMAT_VideoInfo) {
            auto* video = reinterpret_cast<VIDEOINFOHEADER*>(media->pbFormat);
            video->AvgTimePerFrame = duration;
            bitmap = &video->bmiHeader;
            source = &video->rcSource;
            destination = &video->rcTarget;
            bit_rate = &video->dwBitRate;
        } else {
            auto* video = reinterpret_cast<VIDEOINFOHEADER2*>(media->pbFormat);
            video->AvgTimePerFrame = duration;
            video->dwInterlaceFlags = 0;
            bitmap = &video->bmiHeader;
            source = &video->rcSource;
            destination = &video->rcTarget;
            bit_rate = &video->dwBitRate;
        }
        media->SetSubtype(output_ten_bit?&nvof::p010_subtype:&MEDIASUBTYPE_NV12);
        *source = {0, 0, input_.width, input_.height};
        *destination = {0, 0, 0, 0};
        bitmap->biWidth = input_.width;
        bitmap->biHeight = input_.height;
        bitmap->biSizeImage = static_cast<DWORD>(int64_t(input_.width) * input_.height * 3 / (output_ten_bit?1:2));
        bitmap->biBitCount = output_ten_bit?24:12;
        bitmap->biPlanes = 1;
        bitmap->biCompression = output_ten_bit?MAKEFOURCC('P','0','1','0'):MAKEFOURCC('N','V','1','2');
        *bit_rate = 0;
        media->SetSampleSize(bitmap->biSizeImage);
        media->SetTemporalCompression(FALSE);
        return S_OK;
    }

    HRESULT DecideBufferSize(IMemAllocator* allocator, ALLOCATOR_PROPERTIES* properties) override {
        if (!allocator || !properties) return E_POINTER;
        if (!output_.size) return VFW_E_NOT_CONNECTED;
        // A decoder may advertise NV12 while exposing P010 native surfaces.
        const long capacity=native_feature_enabled_?(std::max)(output_.size,long(output_.width*output_.height*3)):output_.size;
        properties->cBuffers = (std::max)(properties->cBuffers, 4L);
        if(m_pOutput && static_cast<nvof::transport::OutputPin*>(m_pOutput)->gpu_mode() && interpolation_requested()) {
            properties->cBuffers=nvof::gpu_output_buffer_count(properties->cBuffers,
                selected_multiple(input_.duration),target_,uint64_t(capacity));
            write_log("output headroom samples="+std::to_string(properties->cBuffers)+
                " capacityMs="+std::to_string(properties->cBuffers*1000.0*target_.den/target_.num)+
                " surfaceBytes="+std::to_string(capacity)+" prefill=0");
        }
        properties->cbBuffer = (std::max)(properties->cbBuffer, capacity);
        properties->cbAlign = (std::max)(properties->cbAlign, 1L);
        properties->cbPrefix = (std::max)(properties->cbPrefix, 0L);
        ALLOCATOR_PROPERTIES actual{};
        HRESULT hr = allocator->SetProperties(properties, &actual);
        if (FAILED(hr)) return hr;
        return actual.cbBuffer >= properties->cbBuffer && actual.cBuffers >= properties->cBuffers
            ? S_OK : E_FAIL;
    }

    HRESULT StartStreaming() override {
        reopen_async_input();
        async_first_input_=true;
        async_queued_=async_peak_=0;async_backpressure_ms_=0;
        pending_gpu_outputs_.clear();output_ahead_peak_=output_ahead_pairs_=0;
        wait_output_frames_=0;wait_delivery_result_=S_OK;
        performance_.reset();
        // Wait until upstream has had its opportunity to activate native decode.
        // A software NV12 decoder must still feed GPU surfaces to a renderer
        // retained from an earlier hardware session (which may retain P010).
        const HRESULT uploaded=activate_cpu_upload(true);if(FAILED(uploaded))return uploaded;
        for(int index=0;index<2;++index) {
            auto* pin=index==0?static_cast<CBasePin*>(m_pInput):static_cast<CBasePin*>(m_pOutput);
            Microsoft::WRL::ComPtr<IPin> connected;
            if(pin && SUCCEEDED(pin->ConnectedTo(&connected)) && topology_logs_.fetch_add(1)<16)
                log_endpoint(instance_id_,"StartStreaming",index==0?PINDIR_INPUT:PINDIR_OUTPUT,connected.Get());
        }
        if(m_pOutput) {
            const HRESULT mode=static_cast<nvof::transport::OutputPin*>(m_pOutput)->set_gpu_mode(native_gpu_active_);
            if(FAILED(mode))return mode;
        }
        cancelled_.store(false, std::memory_order_release);
        cpu_prepare_ms_=cpu_gpu_ms_=0;cpu_upload_fields_=0;cpu_field_override_logged_=false;
        last_pts_.reset();
        seek_preview_.clear();seen_segment_=false;seek_preview_frame_=false;seek_trace_count_=0;
        last_duration_ = input_.duration > 0 ? input_.duration :
            nvof::supported_output_rate(target_)?rate_duration(target_):kUnits/24;
        failed_ = false;
        appearance_pass_frames_=0;
        output_frames_ = source_frames_ = near_endpoint_frames_ = 0;motion_frames_=protected_frames_=scene_cuts_=repeated_pairs_=subpixel_pass_frames_=identical_frames_=midpoint_pass_frames_=midpoint_limited_frames_=0;subpixel_unavailable_=midpoint_unavailable_=false;quality_state_="waiting";
        gpu_delivery_verified_=false;
        sample_probed_ = false;
        hdr_trace_=std::make_shared<nvof::HdrDeliveryTrace>();
        sample_transport_ = "unknown";
        input_surface_format_="unknown";input_conversion_="none";
        engine_active_ = false;
        reset_history();
        write_status(interpolation_requested()?"waiting":"bypass",interpolation_requested()?"Waiting for the next adjacent frame":bypass_message());
        write_log(std::string("stream start: ")+(input_.ten_bit?"P010 ":"NV12 ") + std::to_string(input_.width) + "x" +
            std::to_string(input_.height) + " target=" + std::to_string(target_.num) +
            "/" + std::to_string(target_.den) + " inputRateGroup="+input_rate_group()+
            " InputRateMask="+std::to_string(input_rate_mask_)+" DoubleRate="+std::to_string(selected_multiple(input_.duration)==2)+" bypassReason="+bypass_reason());
        if(performance_.enabled && m_pOutput) {
            ALLOCATOR_PROPERTIES allocation{};
            const HRESULT allocated=static_cast<nvof::transport::OutputPin*>(m_pOutput)->allocator_properties(&allocation);
            write_log("output allocation result="+std::to_string(uint32_t(allocated))+
                " buffers="+std::to_string(allocation.cBuffers)+" capacity="+std::to_string(allocation.cbBuffer)+
                " gpu="+std::to_string(static_cast<nvof::transport::OutputPin*>(m_pOutput)->surface_allocator()));
        }
        return S_OK;
    }

    HRESULT StopStreaming() override {
        write_log("input worker queued="+std::to_string(async_queued_)+" peakPending="+std::to_string(async_peak_)+
            " capacity="+std::to_string(nvof::playback::input_queue_frames)+" producerWaitMs="+std::to_string(async_backpressure_ms_));
        write_log("output ahead peak="+std::to_string(output_ahead_peak_)+" pairs="+std::to_string(output_ahead_pairs_)+
            " limit="+std::to_string(output_ahead_limit())+" prefill=0");
        write_log("output during GPU wait delivered="+std::to_string(wait_output_frames_));
        performance_.flush(true,write_log);
        // CTransformFilter::Stop holds m_csReceive after input decommit.
        cancelled_.store(true, std::memory_order_release);
        release_engine();
        if(cpu_upload_fields_)write_log("CPU upload timing fields="+std::to_string(cpu_upload_fields_)+
            " prepareMs="+std::to_string(cpu_prepare_ms_)+" gpuAndDeliveryMs="+std::to_string(cpu_gpu_ms_));
        last_pts_.reset();
        if(hdr_trace_)write_log("HDR side data: inputMask="+std::to_string(hdr_trace_->input_mask.load())+
            " rendererRequestedMask="+std::to_string(hdr_trace_->requested_mask.load())+
            " rendererReturnedMask="+std::to_string(hdr_trace_->returned_mask.load())+
            " (bits: mastering=1 light=2 DV-RPU=4 DV-v1=8 DV-v2=16; transport only)");
        write_log("stream stop: GPU resources released after receive quiesced");
        write_status("stopped", "Playback stopped");
        return S_OK;
    }

    HRESULT AlterQuality(Quality quality) override {
        performance_.note_quality(quality.Type==Famine,quality.Late,quality.Proportion);
        return CTransformFilter::AlterQuality(quality);
    }

    // Observe graph control without touching receive/history/allocator state.
    // Pausing may leave a Receive blocked in the renderer: never take
    // m_csReceive here or flush/reset its live performance window.
    HRESULT trace_playback_control(bool running,REFERENCE_TIME start=0) {
        if(!performance_.enabled)return running?CTransformFilter::Run(start):CTransformFilter::Pause();
        FILTER_STATE before;
        {CAutoLock filter(&m_csFilter);before=m_State;}
        const std::string action=running?"run":"pause";
        const std::string identity=" instance="+std::to_string(instance_id_)+" action="+action;
        write_log("playback control begin"+identity+" from="+std::to_string(int(before))+
            " start100ns="+std::to_string(start)+" tickMs="+std::to_string(GetTickCount64()));
        const auto begin=nvof::PlaybackTimer::Clock::now();
        const HRESULT hr=running?CTransformFilter::Run(start):CTransformFilter::Pause();
        const double elapsed=std::chrono::duration<double,std::milli>(nvof::PlaybackTimer::Clock::now()-begin).count();
        FILTER_STATE after;
        {CAutoLock filter(&m_csFilter);after=m_State;}
        write_log("playback control end"+identity+" to="+std::to_string(int(after))+
            " result="+std::to_string(uint32_t(hr))+" elapsedMs="+std::to_string(elapsed)+" tickMs="+std::to_string(GetTickCount64()));
        return hr;
    }
    STDMETHODIMP Pause() override {
        set_async_paused(true);
        const HRESULT hr=trace_playback_control(false);
        if(FAILED(hr))set_async_paused(false);
        return hr;
    }
    STDMETHODIMP Run(REFERENCE_TIME start) override {
        const HRESULT hr=trace_playback_control(true,start);
        if(SUCCEEDED(hr))set_async_paused(false);
        return hr;
    }

    STDMETHODIMP Stop() override {
        cancelled_.store(true, std::memory_order_release);
        cancel_async_input();
        CTransformOutputPin* output=nullptr;
        {
            CAutoLock filter(&m_csFilter);
            if (m_State!=State_Stopped && m_pOutput && m_pOutput->IsConnected()) {
                output=m_pOutput;
                // Wake an allocator GetBuffer wait before taking m_csReceive.
                output->Inactive();
            }
        }
        // A direct Stop call can arrive before the renderer itself is stopped.
        // Decommit does not unblock its Receive, so a paired downstream flush
        // must also precede the base class waiting for our streaming lock.
        // Never hold m_csFilter while entering downstream streaming methods.
        const HRESULT flushed=output ? output->DeliverBeginFlush() : S_FALSE;
        join_async_input();
        const HRESULT result=CTransformFilter::Stop();
        if (output && SUCCEEDED(flushed)) output->DeliverEndFlush();
        return result;
    }

    HRESULT BeginFlush() override {
        cancelled_.store(true, std::memory_order_release);
        cancel_async_input();
        // The input pin has already rejected new Receive calls. Crucially,
        // downstream flush precedes m_csReceive to unblock Deliver/GetBuffer.
        if(m_pOutput)static_cast<nvof::transport::OutputPin*>(m_pOutput)->cancel_allocation_waits();
        const HRESULT hr = CTransformFilter::BeginFlush();
        join_async_input();
        {
            CAutoLock receive(&m_csReceive);
            reset_history(true);
            last_pts_.reset();
        }
        return hr;
    }

    HRESULT EndFlush() override {
        {
            CAutoLock receive(&m_csReceive);
            reset_history(true);
            last_pts_.reset();
            failed_ = false;
            async_first_input_=true;
            // A slider drag can issue another seek before the previous pair's
            // phases have reached the renderer. Keep the short settling window
            // at source cadence, without retaining an interpolation endpoint.
            seek_preview_.begin(GetTickCount64(),nvof::playback::seek_preview_ms);
        }
        if(m_pOutput) {
            const HRESULT recommitted=static_cast<nvof::transport::OutputPin*>(m_pOutput)->resume_allocation();
            if(FAILED(recommitted))return recommitted;
        }
        const HRESULT hr = CTransformFilter::EndFlush();
        if (SUCCEEDED(hr)) {
            reopen_async_input();
            cancelled_.store(false, std::memory_order_release);
        }
        // The base input pin unblocks Receive only after this returns.
        return hr;
    }

    HRESULT NewSegment(REFERENCE_TIME start, REFERENCE_TIME stop, double rate) override {
        if (!std::isfinite(rate) || rate <= 0) return E_INVALIDARG;
        CAutoLock receive(&m_csReceive);
        const HRESULT drained=drain_async_input();if(drained!=S_OK)return drained;
        async_first_input_=true;
        reset_history(true);
        last_pts_.reset();
        failed_ = false;
        if(seen_segment_ || seek_preview_.armed())seek_preview_.begin(GetTickCount64(),nvof::playback::seek_preview_ms);
        seen_segment_=true;
        if(performance_.enabled && seek_trace_count_++<32)
            write_log("seek segment start="+std::to_string(start)+" previewMs="+
                std::to_string(seek_preview_.armed()?nvof::playback::seek_preview_ms:0));
        return CTransformFilter::NewSegment(start, stop, rate);
    }

    HRESULT Receive(IMediaSample* sample) override {
        if (!sample) return E_POINTER;
        if (cancelled_.load(std::memory_order_acquire)) return S_FALSE;
        // SampleProps is overwritten by the next decoder Receive. Capture it
        // while the input pin owns m_csReceive. Type changes stay synchronous.
        const AM_SAMPLE2_PROPERTIES properties=*m_pInput->SampleProps();
        const bool async=async_input_enabled_ && native_gpu_active_ && !cpu_upload_active_ && interpolation_requested();
        const bool synchronous=!async || async_first_input_ ||
            (properties.dwSampleFlags & AM_SAMPLE_TYPECHANGED);
        // The first seek preview stays immediate. Later previews also use the
        // worker: a renderer wait during the settling window must not stop
        // decoder input. Preview selection/history remain worker-owned.
        if(synchronous) {
            const HRESULT drained=drain_async_input();if(drained!=S_OK)return drained;
            const HRESULT hr=process_sample(sample,&properties);
            REFERENCE_TIME start=0,stop=0;
            if(hr==S_OK && properties.dwStreamId==AM_STREAM_MEDIA && SUCCEEDED(sample->GetTime(&start,&stop)) && start>=0 && sample->IsPreroll()!=S_OK)
                async_first_input_=false;
            return hr;
        }
        return queue_async_input(sample,properties);
    }

    HRESULT process_sample(IMediaSample* sample,const AM_SAMPLE2_PROPERTIES* properties) {
        if (cancelled_.load(std::memory_order_acquire)) return S_FALSE;
        if (failed_) return E_FAIL;
        if(native_pending_)return VFW_E_NOT_CONNECTED;
        if (properties->dwStreamId != AM_STREAM_MEDIA) return m_pOutput->Deliver(sample);
        struct ReceiveTiming {
            nvof::PlaybackTiming& trace;
            nvof::PlaybackTimer timer;
            ReceiveTiming(nvof::PlaybackTiming& t):trace(t),timer(t.enabled,t.receive) {
                if(t.enabled && t.last_exit!=nvof::PlaybackTiming::Clock::time_point{})
                    t.upstream_gap.add(std::chrono::duration<double,std::milli>(nvof::PlaybackTiming::Clock::now()-t.last_exit).count());
            }
            ~ReceiveTiming(){timer.finish();trace.flush(false,write_log);if(trace.enabled)trace.last_exit=nvof::PlaybackTiming::Clock::now();}
        } receive_timing(performance_);
        try {
            if (!sample_probed_) probe_sample(sample);
            if (properties->dwSampleFlags & AM_SAMPLE_TYPECHANGED) {
                const auto* media = reinterpret_cast<const CMediaType*>(properties->pMediaType);
                Layout incoming;
                if (!media || !parse_input_layout(*media, incoming) ||
                    incoming.width != output_.width || incoming.height != output_.height || incoming.ten_bit!=input_.ten_bit ||
                    incoming.cpu_format!=input_.cpu_format || incoming.interlaced!=input_.interlaced || incoming.top_first!=input_.top_first ||
                    !connected_output_color_matches(*media) ||
                    rate_selected(incoming.duration)!=input_rate_selected_ ||
                    interpolate_source(incoming.duration,incoming.height)!=interpolation_requested() ||
                    (interpolation_requested() && !same_target(incoming.duration)) ||
                    std::llabs(negotiated_duration(incoming.duration,incoming.height)-output_.duration)>1)
                    return report_error("dynamic resolution/rate/color change requires reopening the video", VFW_E_TYPE_NOT_ACCEPTED);
                HRESULT hr = m_pInput->SetMediaType(media);
                if (FAILED(hr)) return hr;
                reset_history();
                // A permitted rate metadata change starts a new source cadence.
                pipeline_.reset();gpu_pipeline_.reset();
                last_pts_.reset();
            }
            REFERENCE_TIME start = 0, stop = 0;
            const HRESULT timing = sample->GetTime(&start, &stop);
            if (FAILED(timing)) {
                reset_history();
                last_pts_.reset();
                return report_error("input sample has no presentation timestamp", VFW_E_SAMPLE_TIME_NOT_SET);
            }
            if(performance_.enabled)performance_.pts=start;
            // Decoder preroll belongs before the new visible segment. FRUC
            // accepts only nonnegative timestamps; retaining a preroll frame
            // would poison the first post-seek pair. Discard it without GPU
            // work and let the first visible original create fresh history.
            if (start < 0 || sample->IsPreroll() == S_OK) {
                reset_history(true);
                last_pts_.reset();
                return S_OK;
            }
            bool discontinuity = sample->IsDiscontinuity() == S_OK || !last_pts_;
            if (last_pts_) {
                const int64_t delta = start - *last_pts_;
                const int64_t maximum_gap = (std::max)(kUnits, last_duration_ * 10);
                if (delta <= 0 || delta > maximum_gap) discontinuity = true;
                else if (timing != S_OK || stop <= start) last_duration_ = delta;
            }
            if (timing == S_OK && stop > start) last_duration_ = stop - start;
            if (discontinuity) reset_history(true);
            const int64_t end=timing==S_OK && stop>start?stop:start+last_duration_;
            seek_preview_frame_=interpolation_requested() && seek_preview_.active(GetTickCount64(),start);
            if(seek_preview_frame_)quality_state_="seek-preview";
            ++source_frames_;
            if(cpu_upload_active_) {
                last_pts_=start;
                return input_.ten_bit?receive_cpu10(sample,start,end,discontinuity):
                    receive_cpu8(sample,start,end,discontinuity);
            }
            if(native_gpu_active_) {
                last_pts_=start;
                return receive_gpu(sample,start,end,discontinuity);
            }
            if(input_.ten_bit || input_.hdr_transfer)return report_error("CPU 10-bit upload requires a compatible D3D11 renderer",VFW_E_TYPE_NOT_ACCEPTED);
            if(renderer_gpu_committed_ || output_.ten_bit)
                return report_error("CPU input cannot use the retained native output; reopen the video",VFW_E_TYPE_NOT_ACCEPTED);
            if (interpolation_requested() && !seek_preview_frame_) ensure_pipeline();
            BYTE* data = nullptr;
            HRESULT hr = sample->GetPointer(&data);
            if (FAILED(hr) || !data) return report_error("input NV12 buffer is not CPU accessible; enable decoder Copy-Back", FAILED(hr) ? hr : E_POINTER);
            if (sample_transport_ == "unknown") sample_transport_ = "system-memory";
            if (sample->GetActualDataLength() < input_.size || sample->GetSize() < input_.size)
                return report_error("input NV12 buffer is smaller than its advertised pitch/height", VFW_E_BUFFER_OVERFLOW);
            nvof::Frame frame;
            frame.width = input_.width;
            frame.height = input_.height;
            frame.pts = start;
            frame.pixels.resize(size_t(input_.width) * input_.height * 3 / 2);
            for (int row = 0; row < input_.height * 3 / 2; ++row)
                std::memcpy(frame.pixels.data() + size_t(row) * input_.width,
                    data + size_t(row) * input_.pitch, input_.width);
            last_pts_ = start;
            preroll_ = sample->IsPreroll() == S_OK;
            if (!interpolation_requested() || seek_preview_frame_) {
                return deliver(nvof::OutputFrame{std::move(frame), end, discontinuity});
            }
            HRESULT delivery = S_OK;
            pipeline_->push(std::move(frame), discontinuity, [this, &delivery](const nvof::OutputFrame& output) {
                delivery = deliver(output);
                return delivery == S_OK;
            });
            return delivery;
        } catch (const std::bad_alloc&) {
            return report_error("frame allocation failed", E_OUTOFMEMORY);
        } catch (const std::exception& error) {
            return report_error(std::string("Frame interpolation failed: ") + error.what(), E_FAIL);
        } catch (...) {
            return report_error("unknown C++ processing failure", E_FAIL);
        }
    }

    HRESULT EndOfStream() override {
        const HRESULT drained=drain_async_input();if(drained!=S_OK)return drained;
        if (cancelled_.load(std::memory_order_acquire)) return S_FALSE;
        if (failed_) return E_FAIL;
        try {
            HRESULT delivery = S_OK;
            if(gpu_pipeline_)gpu_pipeline_->finish(cpu_upload_active_?cpu_last_frame_duration_:last_duration_,[this,&delivery](const nvof::GpuOutputFrame& output) {
                delivery=deliver_gpu(output);
                return delivery==S_OK;
            });
            if (pipeline_) pipeline_->finish(last_duration_, [this, &delivery](const nvof::OutputFrame& output) {
                delivery = deliver(output);
                return delivery == S_OK;
            });
            if (delivery != S_OK) return delivery;
            return CTransformFilter::EndOfStream();
        } catch (const std::exception& error) {
            return report_error(std::string("end-of-stream failed: ") + error.what(), E_FAIL);
        } catch (...) {
            return report_error("end-of-stream processing failure", E_FAIL);
        }
    }

private:
    struct PendingInput {
        Microsoft::WRL::ComPtr<IMediaSample> sample;
        AM_SAMPLE2_PROPERTIES properties{};
    };
    // At most two pending decoder samples and one active sample. AddRef keeps
    // their array slices from being recycled before capture is submitted.
    // The worker never acquires m_csReceive: a producer may hold it while the
    // bounded queue is full. Stop/flush cancel producers before joining us.
    HRESULT queue_async_input(IMediaSample* sample,const AM_SAMPLE2_PROPERTIES& properties) {
        try {
            std::unique_lock<std::mutex> lock(async_mutex_);
            const auto begin=std::chrono::steady_clock::now();
            async_cv_.wait(lock,[this]{return async_stop_ || async_result_!=S_OK || async_queue_.size()<nvof::playback::input_queue_frames;});
            async_backpressure_ms_+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
            if(cancelled_.load(std::memory_order_acquire))return S_FALSE;
            if(async_result_!=S_OK)return async_result_;
            if(async_stop_)return S_FALSE;
            if(!async_thread_.joinable())async_thread_=std::thread([this]{async_input_loop();});
            async_queue_.push_back(PendingInput{sample,properties});
            ++async_queued_;async_peak_=(std::max)(async_peak_,async_queue_.size());
            lock.unlock();async_cv_.notify_all();return S_OK;
        }catch(...) {return E_OUTOFMEMORY;}
    }
    void async_input_loop() noexcept {
        const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        write_log("input worker started instance="+std::to_string(instance_id_)+" pendingLimit="+std::to_string(nvof::playback::input_queue_frames)+"; receive timing now measures worker processing");
        for(;;) {
            PendingInput item;
            {
                std::unique_lock<std::mutex> lock(async_mutex_);
                // Initial and subsequent pauses use the same rule. The first
                // original was already delivered synchronously; no speculative
                // GPU preparation competes with the paused renderer startup.
                async_cv_.wait(lock,[this]{return async_stop_ || (!async_paused_ && !async_queue_.empty());});
                if(async_stop_)break;
                item=std::move(async_queue_.front());async_queue_.pop_front();async_busy_=true;
            }
            async_cv_.notify_all();
            worker_processing_=true;
            HRESULT hr=process_sample(item.sample.Get(),&item.properties);
            if(hr==S_OK && !pending_gpu_outputs_.empty()) {
                ++output_ahead_pairs_;
                if(performance_.enabled && output_ahead_pairs_<=2)
                    write_log("output ahead prepared pending="+std::to_string(pending_gpu_outputs_.size()));
                bool more_input;
                {std::lock_guard<std::mutex> lock(async_mutex_);more_input=!async_queue_.empty();}
                // Never strand the last ready outputs while waiting for a new
                // input (including EOS). With input available, compute its pair
                // while the renderer still owns all allocator samples.
                if(!more_input) {
                    try {hr=drain_gpu_outputs(false);}
                    catch(const std::exception& error){hr=report_error(std::string("Queued output failed: ")+error.what(),E_FAIL);}
                    catch(...){hr=report_error("Queued output failed",E_FAIL);}
                }
            }
            if(hr!=S_OK)pending_gpu_outputs_.clear();
            worker_processing_=false;
            item.sample.Reset();
            {
                std::lock_guard<std::mutex> lock(async_mutex_);
                async_busy_=false;
                if(hr!=S_OK) {async_result_=hr;async_queue_.clear();async_stop_=true;}
            }
            async_cv_.notify_all();
        }
        if(SUCCEEDED(com))CoUninitialize();
    }
    HRESULT drain_async_input() {
        std::unique_lock<std::mutex> lock(async_mutex_);
        async_cv_.wait(lock,[this]{return async_stop_ || (!async_busy_ && async_queue_.empty());});
        if(async_result_!=S_OK)return async_result_;
        return async_stop_?S_FALSE:S_OK;
    }
    void cancel_async_input() {
        {std::lock_guard<std::mutex> lock(async_mutex_);async_stop_=true;async_queue_.clear();}
        async_cv_.notify_all();
    }
    void join_async_input() {
        if(async_thread_.joinable())async_thread_.join();
        pending_gpu_outputs_.clear();
    }
    void reopen_async_input() {
        std::lock_guard<std::mutex> lock(async_mutex_);
        async_stop_=false;async_result_=S_OK;
    }
    void set_async_paused(bool paused) {
        {std::lock_guard<std::mutex> lock(async_mutex_);async_paused_=paused;}
        async_cv_.notify_all();
    }
    bool wait_async_running() {
        if(!worker_processing_)return !cancelled_.load(std::memory_order_acquire);
        std::unique_lock<std::mutex> lock(async_mutex_);
        async_cv_.wait(lock,[this]{return async_stop_ || !async_paused_;});
        return !async_stop_;
    }
    std::shared_ptr<const nvof::HdrMetadata> capture_sample_hdr(IMediaSample* sample) {
        auto hdr=nvof::capture_hdr(sample);
        if(hdr && hdr_trace_)for(size_t i=0;i<hdr->data.size();++i)if(!hdr->data[i].empty()) {
            const unsigned bit=1u<<i;
            if(!(hdr_trace_->input_mask.fetch_or(bit)&bit) && i>=2)
                write_log("Dolby Vision side data received: slot="+std::to_string(i)+" bytes="+std::to_string(hdr->data[i].size()));
        }
        return hdr;
    }
    HRESULT ensure_native_output_format(bool tenBit) {
        if(output_.ten_bit==tenBit)return S_OK;
        // Establish the true decoder storage before the first delivered frame.
        // Do not silently downgrade 10-bit pixels or retag an active stream.
        if(output_frames_!=0)return report_error("Native bit depth changed; reopen the video",VFW_E_TYPE_NOT_ACCEPTED);
        CMediaType proposed=m_pOutput->CurrentMediaType();
        if(proposed.formattype!=FORMAT_VideoInfo2)return VFW_E_TYPE_NOT_ACCEPTED;
        auto* h=reinterpret_cast<VIDEOINFOHEADER2*>(proposed.pbFormat);
        proposed.SetSubtype(tenBit?&nvof::p010_subtype:&MEDIASUBTYPE_NV12);
        h->bmiHeader.biBitCount=tenBit?24:12;
        h->bmiHeader.biCompression=tenBit?MAKEFOURCC('P','0','1','0'):MAKEFOURCC('N','V','1','2');
        h->bmiHeader.biWidth=output_.width;h->bmiHeader.biHeight=output_.height;
        h->bmiHeader.biSizeImage=output_.width*output_.height*3/(tenBit?1:2);
        proposed.SetSampleSize(h->bmiHeader.biSizeImage);
        Microsoft::WRL::ComPtr<IPin> receiver;
        HRESULT hr=m_pOutput->ConnectedTo(&receiver);if(FAILED(hr))return hr;
        if(receiver->QueryAccept(&proposed)!=S_OK)return report_error("Renderer rejected native video bit depth",VFW_E_TYPE_NOT_ACCEPTED);
        hr=m_pOutput->SetMediaType(&proposed);if(FAILED(hr))return hr;
        pending_gpu_media_type_=true;return S_OK;
    }
    HRESULT receive_gpu(IMediaSample* sample,int64_t start,int64_t stop,bool discontinuity) {
        auto input=nvof::extract_gpu_frame(sample,input_.width,input_.height,start,input_.native_color_supported);
        D3D11_TEXTURE2D_DESC surface{};input.texture->GetDesc(&surface);
        const bool p010=surface.Format==DXGI_FORMAT_P010;
        if(output_.hdr_transfer && !p010)return report_error("Native HDR requires a 10-bit P010 decoder surface",VFW_E_TYPE_NOT_ACCEPTED);
        // Metadata is a sample contract, not conditional on a possibly untagged
        // decoder media type. Capture every sample without stale-frame caching.
        input.hdr=capture_sample_hdr(sample);
        const HRESULT agreed=ensure_native_output_format(p010);if(FAILED(agreed))return agreed;
        const std::string format=p010?"P010":"NV12";
        if(input_surface_format_!=format) {
            input_surface_format_=format;
            input_conversion_="none";
            write_log("GPU input surface="+format+" output="+format+" conversion="+input_conversion_+
                " arraySize="+std::to_string(surface.ArraySize)+" codedWidth="+std::to_string(surface.Width)+
                " codedHeight="+std::to_string(surface.Height)+" bindFlags="+std::to_string(surface.BindFlags)+
                " transfer="+std::to_string(output_.hdr_transfer)+" hdrMasteringBytes="+std::to_string(input.hdr?input.hdr->data[0].size():0)+
                " hdrLightBytes="+std::to_string(input.hdr?input.hdr->data[1].size():0));
        }
        if(!gpu_engine_)gpu_engine_=std::make_unique<nvof::GpuFrucEngine>(directory_/L"runtime",
            native_device_.Get(),native_context_.Get(),native_mutex_,nvof::GpuCompletionMode::context_ordered,true,gpu_correction_,appearance_protection_,nvof::GpuInterpolationBackend::native_experimental);
        // Submit capture while the upstream sample is held. Decoder reuse and
        // renderer reads are ordered on this device's shared immediate context;
        // keep an owned output lease without a per-frame CPU completion wait.
        nvof::PlaybackTimer capture_timer(performance_.enabled,performance_.capture);
        // Deliver the first original without adding analysis allocation or
        // shader warmup to the time until the first picture is visible.
        auto owned=gpu_engine_->copy(input,discontinuity || seek_preview_frame_);
        capture_timer.finish();
        sample_transport_="d3d11-texture";
        return push_gpu_frame(std::move(owned),stop,discontinuity);
    }

    HRESULT push_gpu_frame(nvof::GpuFrame owned,int64_t stop,bool discontinuity) {
        wait_delivery_result_=S_OK;
        if(!interpolation_requested() || seek_preview_frame_)return deliver_gpu({std::move(owned),stop,discontinuity});
        if(!gpu_pipeline_)gpu_pipeline_=std::make_unique<nvof::GpuPhasePipeline>(target_,
            [this](const nvof::GpuFrame& a,const nvof::GpuFrame& b,const std::vector<int64_t>& times) {
                nvof::PlaybackTimer synthesis_timer(performance_.enabled,performance_.synthesis);
                gpu_engine_->enable_timing(performance_.enabled,trace_every_gpu_pair_,trace_every_gpu_pair_);
                gpu_engine_->set_wait_service(worker_processing_&&output_ahead_limit()?
                    nvof::GpuFrucEngine::WaitService([this]{return service_waiting_outputs();}):nvof::GpuFrucEngine::WaitService{});
                auto batch=gpu_engine_->interpolate_pair(a,b,times);
                if(performance_.enabled) {
                    const auto timing=gpu_engine_->pair_timing();
                    const double work=timing.context_wait_ms+timing.scene_ms+timing.prepare_ms+timing.submit_ms+timing.completion_ms;
                    const double budget=double(b.pts-a.pts)/10000.0;
                    if(trace_every_gpu_pair_ && (source_frames_<=6||work>budget))
                        write_log("gpu pair instance="+std::to_string(instance_id_)+" pts="+std::to_string(b.pts)+
                            " sourceFrames="+std::to_string(source_frames_)+" budgetMs="+std::to_string(budget)+
                            " workMs="+std::to_string(work)+" contextMs="+std::to_string(timing.context_wait_ms)+
                            " sceneMs="+std::to_string(timing.scene_ms)+" prepareMs="+std::to_string(timing.prepare_ms)+
                            " outputSubmitMs="+std::to_string(timing.completion_ms)+
                            " flushEventWaits="+std::to_string(timing.flush_event_wait_calls)+
                            " wait0Ms="+std::to_string(timing.wait_each_ms[0])+" wait1Ms="+std::to_string(timing.wait_each_ms[1])+
                            " wait2Ms="+std::to_string(timing.wait_each_ms[2])+
                            " resume0Ms="+std::to_string(timing.resume_each_ms[0])+" resume1Ms="+std::to_string(timing.resume_each_ms[1])+
                            " resume2Ms="+std::to_string(timing.resume_each_ms[2])+
                            " decoderLockMs="+std::to_string(timing.decoder_lock_ms)+" multithreadLockMs="+std::to_string(timing.multithread_lock_ms)+
                            " stateSwapMs="+std::to_string(timing.state_swap_ms)+" waitServiceMs="+std::to_string(timing.wait_service_ms)+
                            " tickMs="+std::to_string(GetTickCount64()));
                    performance_.context_wait.add(timing.context_wait_ms);performance_.scene.add(timing.scene_ms);
                    performance_.prepare.add(timing.prepare_ms);performance_.submit.add(timing.submit_ms);performance_.completion.add(timing.completion_ms);
                    performance_.shared_wait.add(timing.shared_wait_ms);
                    performance_.wait_submit.add(timing.wait_submit_ms);
                    performance_.wait_completion.add(timing.wait_completion_ms);
                    performance_.context_resume.add(timing.context_resume_ms);
                    performance_.shared_wait_calls+=timing.shared_wait_calls;
                    performance_.queued_output_flushes+=timing.queued_output_flushes;
                    performance_.combined_scene_pairs+=timing.combined_scene_pairs;
                    performance_.event_wait_calls+=timing.event_wait_calls;
                    performance_.query_wait_calls+=timing.query_wait_calls;
                    for(unsigned i=0;i<timing.capture_sample_count;++i) {
                        const auto& capture=timing.capture_samples[i];
                        write_log("gpu capture instance="+std::to_string(instance_id_)+" pts="+std::to_string(capture.pts)+
                            " copyMs="+std::to_string(capture.copy_ms)+" normalizeMs="+std::to_string(capture.normalize_ms)+
                            " totalMs="+std::to_string(capture.total_ms));
                    }
                    for(unsigned i=0;i<timing.gpu_sample_count;++i) {
                        const auto& sample=timing.gpu_samples[i];
                        write_log("gpu timeline instance="+std::to_string(instance_id_)+" pts="+std::to_string(sample.pts)+
                            " beginTickMs="+std::to_string(sample.begin_tick)+" endTickMs="+std::to_string(sample.end_tick)+
                            " waits="+std::to_string(sample.waits)+" inputMs="+std::to_string(sample.input_ms)+
                            " analysisMs="+std::to_string(sample.analysis_ms)+" tailMs="+std::to_string(sample.tail_ms)+
                            " stageDetail="+std::to_string(sample.stages_measured)+
                            " repairMs="+std::to_string(sample.repair_ms)+" glyphMs="+std::to_string(sample.glyph_ms)+
                            " layersMs="+std::to_string(sample.layers_ms)+" phaseMs="+std::to_string(sample.phase_ms)+
                            " inverseDetail="+std::to_string(sample.inverse_measured)+
                            " inverseMs="+std::to_string(sample.inverse_ms)+" outputMs="+std::to_string(sample.output_ms)+
                            " workMs="+std::to_string(sample.work_ms)+" gapMs="+std::to_string(sample.gap_ms)+
                            " cpuWaitMs="+std::to_string(sample.cpu_wait_ms)+" cpuResumeMs="+std::to_string(sample.cpu_resume_ms)+
                            " cpuSubmitMs="+std::to_string(sample.cpu_submit_ms)+
                            " cpuArmMs="+std::to_string(sample.cpu_arm_ms)+" cpuSleepMs="+std::to_string(sample.cpu_sleep_ms)+
                            " wait0CpuMs="+std::to_string(sample.wait_ms[0])+" wait1CpuMs="+std::to_string(sample.wait_ms[1])+
                            " gap0Ms="+std::to_string(sample.gap_each_ms[0])+" gap1Ms="+std::to_string(sample.gap_each_ms[1]));
                    }
                    if(timing.gpu_samples_invalid||timing.gpu_samples_skipped)
                        write_log("gpu timeline unavailable invalid="+std::to_string(timing.gpu_samples_invalid)+
                            " skipped="+std::to_string(timing.gpu_samples_skipped));
                }
                record_quality(batch.quality,times.size());record_endpoints(a.pts,b.pts,times);return batch;
            },nvof::canonical_source_rate(input_.duration));
        HRESULT delivery=S_OK;
        gpu_pipeline_->push(std::move(owned),discontinuity,[this,&delivery](const nvof::GpuOutputFrame& output) {
            delivery=deliver_gpu(output);return delivery==S_OK;
        });
        return delivery;
    }

    HRESULT receive_cpu8(IMediaSample* sample,int64_t start,int64_t stop,bool discontinuity) {
        if(input_.hdr_transfer)return report_error("CPU HDR requires a 10-bit decoder output",VFW_E_TYPE_NOT_ACCEPTED);
        BYTE* data=nullptr;
        HRESULT hr=sample->GetPointer(&data);
        if(FAILED(hr) || !data)return report_error("CPU NV12 input has no host buffer",FAILED(hr)?hr:E_POINTER);
        if(sample->GetActualDataLength()<input_.size || sample->GetSize()<input_.size)
            return report_error("CPU NV12 input buffer is truncated",VFW_E_BUFFER_OVERFLOW);
        // Input-only reconnects do not renegotiate the output pin. Announce the
        // real surface format on the first delivered sample, before any pixels.
        hr=ensure_native_output_format(false);if(FAILED(hr))return hr;
        if(!gpu_engine_)gpu_engine_=std::make_unique<nvof::GpuFrucEngine>(directory_/L"runtime",
            native_device_.Get(),native_context_.Get(),native_mutex_,nvof::GpuCompletionMode::context_ordered,true,
            gpu_correction_,appearance_protection_,nvof::GpuInterpolationBackend::native_experimental);
        nvof::PlaybackTimer capture_timer(performance_.enabled,performance_.capture);
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width=input_.width;desc.Height=input_.height;desc.MipLevels=1;desc.ArraySize=1;
        desc.Format=DXGI_FORMAT_NV12;desc.SampleDesc.Count=1;
        desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial{};initial.pSysMem=data;initial.SysMemPitch=input_.pitch;
        nvof::GpuFrame uploaded;uploaded.width=input_.width;uploaded.height=input_.height;uploaded.pts=start;
        // The driver consumes the host buffer during CreateTexture2D. Decoder
        // buffers may be recycled immediately after Receive returns. Only owned
        // GPU snapshots reach the pipeline/renderer; there is no CPU readback.
        hr=native_device_->CreateTexture2D(&desc,&initial,&uploaded.texture);
        if(FAILED(hr))return report_error("CPU NV12 texture upload failed",hr);
        auto owned=gpu_engine_->copy(uploaded);
        capture_timer.finish();
        cpu_last_frame_duration_=stop-start;
        sample_transport_="cpu-upload-d3d11";
        if(input_surface_format_!="NV12")write_log("CPU input surface=NV12 output=NV12 conversion=8-bit-upload");
        input_surface_format_="NV12";input_conversion_="8-bit-upload";
        return push_gpu_frame(std::move(owned),stop,discontinuity);
    }

    HRESULT receive_cpu10(IMediaSample* sample,int64_t start,int64_t stop,bool discontinuity) {
        auto tick=std::chrono::steady_clock::now();
        auto elapsed=[&](){const auto now=std::chrono::steady_clock::now();const double ms=std::chrono::duration<double,std::milli>(now-tick).count();tick=now;return ms;};
        // StopStreaming releases the engine while keeping the negotiated device
        // and allocator. A subsequent Run must recreate it, just like receive_gpu.
        if(!gpu_engine_)gpu_engine_=std::make_unique<nvof::GpuFrucEngine>(directory_/L"runtime",
            native_device_.Get(),native_context_.Get(),native_mutex_,nvof::GpuCompletionMode::context_ordered,true,
            gpu_correction_,appearance_protection_,nvof::GpuInterpolationBackend::native_experimental);
        BYTE* data=nullptr;HRESULT hr=sample->GetPointer(&data);
        if(FAILED(hr) || !data)return report_error("CPU 10-bit input has no host buffer",FAILED(hr)?hr:E_POINTER);
        if(sample->GetActualDataLength()<input_.size || sample->GetSize()<input_.size)
            return report_error("CPU 10-bit input buffer is truncated",VFW_E_BUFFER_OVERFLOW);
        hr=ensure_native_output_format(true);if(FAILED(hr))return hr;
        if(!cpu_upload_)cpu_upload_=std::make_unique<nvof::CpuUpload>(native_device_.Get(),native_context_.Get(),native_mutex_);
        cpu_upload_->begin(data,size_t(sample->GetActualDataLength()),
            {input_.width,input_.height,input_.pitch,input_.cpu_format},discontinuity);
        const auto hdr=capture_sample_hdr(sample);
        bool top_first=input_.top_first;
        bool weave=false;
        if(input_.interlaced) {
            const auto flags=m_pInput->SampleProps()->dwTypeSpecificFlags;
            if(flags & AM_VIDEO_FLAG_REPEAT_FIELD)return report_error("Repeat-field cadence is not supported by CPU deinterlacing",VFW_E_TYPE_NOT_ACCEPTED);
            // FIELD1FIRST/WEAVE are valid for VIDEOINFOHEADER2 interlaced types.
            top_first=(flags & AM_VIDEO_FLAG_FIELD1FIRST)!=0;
            weave=(flags & AM_VIDEO_FLAG_WEAVE)!=0;
            if(!flags)top_first=input_.top_first;
        }
        // Normally WEAVE is one progressive picture with its full duration.
        // Some software decoders also tag untouched fields WEAVE. Override only
        // on interlace-capable input with repeated moving-comb evidence; already
        // blended/progressive pictures must not become duplicate half-frames.
        const bool detected_fields=input_.interlaced && weave && cpu_upload_->needs_field_reconstruction();
        if(detected_fields) {
            top_first=input_.top_first;
            if(!cpu_field_override_logged_) {
                write_log("CPU moving fields detected in WEAVE sample: topFirst="+std::to_string(top_first)+
                    " sampleFlags="+std::to_string(m_pInput->SampleProps()->dwTypeSpecificFlags)+
                    " sampleDuration="+std::to_string(stop-start)+"; using negotiated field order");
                cpu_field_override_logged_=true;
            }
        }
        const unsigned fields=input_.interlaced && (!weave || detected_fields)?2:1;
        if(stop-start<int64_t(fields))return VFW_E_SAMPLE_TIME_NOT_SET;
        for(unsigned f=0;f<fields;++f) {
            if(cancelled_.load(std::memory_order_acquire))return S_FALSE;
            const int parity=fields==1?-1:(top_first?int(f):1-int(f));
            cpu_prepare_ms_+=elapsed();++cpu_upload_fields_;
            auto frame=cpu_upload_->render(parity,start+(stop-start)*f/fields);frame.hdr=hdr;
            const int64_t end=start+(stop-start)*(f+1)/fields;
            cpu_last_frame_duration_=end-frame.pts;
            auto owned=gpu_engine_->copy(frame);
            sample_transport_="cpu-upload-d3d11";
            input_surface_format_=input_.cpu_format==nvof::CpuVideoFormat::y210?"Y210":
                input_.cpu_format==nvof::CpuVideoFormat::p210?"P210":input_.cpu_format==nvof::CpuVideoFormat::p016?"P016":"P010";
            input_conversion_=fields==2?(detected_fields?"gpu-10-bit-detected-fields-to-P010":"gpu-10-bit-field-reconstruction-to-P010"):
                nvof::cpu_422(input_.cpu_format)?"10-bit-422-to-P010":"10-bit-upload";
            hr=push_gpu_frame(std::move(owned),end,discontinuity && f==0);
            cpu_gpu_ms_+=elapsed();
            if(hr!=S_OK)return hr;
        }
        return S_OK;
    }

    size_t output_ahead_limit() const noexcept {
        // Cover brief completion stalls with already prepared output, bounded
        // by frame count and surface memory. Never wait to fill this capacity.
        if(!async_input_enabled_ || !native_gpu_active_ || cpu_upload_active_ ||
            !interpolation_requested())return 0;
        const uint64_t bytes=uint64_t(output_.width)*output_.height*3;
        return nvof::gpu_ready_output_count(selected_multiple(input_.duration),target_,bytes);
    }
    HRESULT drain_gpu_outputs(bool nonblocking) {
        while(!pending_gpu_outputs_.empty()) {
            bool unavailable=false;
            const HRESULT hr=deliver_gpu_now(pending_gpu_outputs_.front(),nonblocking,&unavailable);
            if(unavailable)return S_OK;
            if(hr!=S_OK)return hr;
            pending_gpu_outputs_.pop_front();
        }
        return S_OK;
    }
    HRESULT deliver_gpu(const nvof::GpuOutputFrame& output) {
        if(wait_delivery_result_!=S_OK)return wait_delivery_result_;
        const size_t limit=worker_processing_?output_ahead_limit():0;
        if(!limit) {
            const HRESULT drained=drain_gpu_outputs(false);
            return drained==S_OK?deliver_gpu_now(output,false):drained;
        }
        // Keep a single owner thread for delivery, metadata and timing. No
        // output is withheld when the renderer has space; only allocator
        // exhaustion defers delivery. Samples are never dropped or retimed.
        HRESULT hr=drain_gpu_outputs(true);if(hr!=S_OK)return hr;
        if(pending_gpu_outputs_.empty()) {
            bool unavailable=false;
            hr=deliver_gpu_now(output,true,&unavailable);
            if(!unavailable)return hr;
        }
        while(pending_gpu_outputs_.size()>=limit) {
            hr=deliver_gpu_now(pending_gpu_outputs_.front(),false);
            if(hr!=S_OK)return hr;
            pending_gpu_outputs_.pop_front();
        }
        pending_gpu_outputs_.push_back(output);
        output_ahead_peak_=(std::max)(output_ahead_peak_,pending_gpu_outputs_.size());
        return drain_gpu_outputs(true);
    }
    HRESULT deliver_gpu_now(const nvof::GpuOutputFrame& output,bool nonblocking,bool* unavailable=nullptr) {
        if(!ready_for_gpu_output(unavailable))return S_FALSE;
        if(cancelled_.load(std::memory_order_acquire))return S_FALSE;
        if(!output.frame.texture || output.frame.width!=output_.width || output.frame.height!=output_.height ||
            output.stop<=output.frame.pts)return report_error("invalid native GPU output",E_UNEXPECTED);
        IMediaSample* raw=nullptr;
        REFERENCE_TIME start=output.frame.pts,stop=output.stop;
        nvof::PlaybackTimer allocator_timer(performance_.enabled,performance_.allocator);
        HRESULT result=m_pOutput->GetDeliveryBuffer(&raw,&start,&stop,nonblocking?AM_GBF_NOWAIT:0);
        allocator_timer.finish();
        SamplePtr sample(raw);
        // A downstream Receive timeout is a real error, never a reason to
        // retry a sample. Only this allocator's NOWAIT timeout means "full".
        if(nonblocking && result==VFW_E_TIMEOUT && unavailable)*unavailable=true;
        if(result!=S_OK)return result;
        if(cancelled_.load(std::memory_order_acquire))return S_FALSE;
        auto* surface=dynamic_cast<nvof::transport::SurfaceSample*>(raw);
        if(!surface)return report_error("renderer did not accept the native GPU allocator",E_UNEXPECTED);
        surface->assign(output.frame,hdr_trace_);
        sample->SetTime(&start,&stop);
        result=sample->SetMediaType(pending_gpu_media_type_?&m_pOutput->CurrentMediaType():nullptr);if(FAILED(result))return result;
        sample->SetActualDataLength(output_.width*output_.height*3/(output_.ten_bit?1:2));
        sample->SetMediaTime(nullptr,nullptr);
        sample->SetSyncPoint(TRUE);
        sample->SetDiscontinuity(output.discontinuity);
        sample->SetPreroll(FALSE);
        if(cancelled_.load(std::memory_order_acquire))return S_FALSE;
        nvof::PlaybackTimer delivery_timer(performance_.enabled,performance_.delivery);
        if(!ready_for_gpu_output(unavailable))return S_FALSE;
        result=m_pOutput->Deliver(sample.get());
        delivery_timer.finish();
        if(result==S_OK) {
            pending_gpu_media_type_=false;
            gpu_delivery_verified_=true;
            ++output_frames_;
            if(performance_.enabled)performance_.note_output(start,stop,output.discontinuity);
            if(!wait_service_active_)write_status(!interpolation_requested()?"bypass":engine_active_?"active":"waiting",
                !interpolation_requested()?bypass_message():engine_active_?"NVIDIA Optical Flow / Motion phases (D3D11 GPU)":
                    "Waiting for the next adjacent GPU frame");
        }
        return result;
    }

    bool ready_for_gpu_output(bool* unavailable) {
        if(wait_service_active_) {
            // Never park an engine completion wait on Run. A paused callback
            // leaves the prepared output queued; ordinary delivery owns Run.
            std::lock_guard<std::mutex> lock(async_mutex_);
            if(async_stop_)return false;
            if(async_paused_){if(unavailable)*unavailable=true;return false;}
            return true;
        }
        return wait_async_running();
    }
    HANDLE service_waiting_outputs() noexcept {
        if(wait_delivery_result_!=S_OK || pending_gpu_outputs_.empty() || cancelled_.load(std::memory_order_acquire))return nullptr;
        {std::lock_guard<std::mutex> lock(async_mutex_);if(async_stop_||async_paused_)return nullptr;}
        // Same worker, same ordered deque. Device locks are released by the
        // engine, while its private state and all source leases remain owned.
        // Only already prepared outputs are serviced; no engine re-entry.
        wait_service_active_=true;
        const auto before=output_frames_;
        try {wait_delivery_result_=drain_gpu_outputs(true);}
        catch(...) {wait_delivery_result_=E_FAIL;}
        wait_service_active_=false;
        if(wait_delivery_result_!=S_OK)write_log("output during GPU wait failure="+std::to_string(uint32_t(wait_delivery_result_)));
        const auto delivered=output_frames_-before;
        if(delivered) {
            if(performance_.enabled&&wait_output_frames_==0)write_log("ready outputs delivered during GPU wait count="+std::to_string(delivered));
            wait_output_frames_+=delivered;
        }
        if(wait_delivery_result_!=S_OK || pending_gpu_outputs_.empty() || cancelled_.load(std::memory_order_acquire))return nullptr;
        {std::lock_guard<std::mutex> lock(async_mutex_);if(async_stop_||async_paused_)return nullptr;}
        return static_cast<nvof::transport::OutputPin*>(m_pOutput)->returned_event();
    }

    void probe_sample(IMediaSample* sample) {
        sample_probed_=true;
        IUnknown* texture_sample=nullptr;
        const HRESULT texture_result=sample->QueryInterface(kMediaSampleD3D11,
            reinterpret_cast<void**>(&texture_sample));
        const bool has_texture=SUCCEEDED(texture_result) && texture_sample;
        if (texture_sample) texture_sample->Release();
        IUnknown* service=nullptr;
        const HRESULT service_result=sample->QueryInterface(__uuidof(IMFGetService),
            reinterpret_cast<void**>(&service));
        const bool has_service=SUCCEEDED(service_result) && service;
        if (service) service->Release();
        sample_transport_=has_texture ? "d3d11-interface-unvalidated" : has_service ? "mf-service-unknown" : "unknown";
        write_log("native probe: first sample IMediaSampleD3D11 HRESULT="+
            std::to_string(static_cast<unsigned long>(texture_result))+
            " IMFGetService HRESULT="+std::to_string(static_cast<unsigned long>(service_result))+
            "; texture/service interfaces not invoked");
    }

    void ensure_pipeline() {
        if (!pipeline_) {
            // Engine initialization stays lazy: the first original frame after
            // a seek is delivered before GPU work for the next frame pair.
            pipeline_ = std::make_unique<nvof::PhasePipeline>(target_, [this](const nvof::Frame& a, const nvof::Frame& b,const std::vector<int64_t>& times) {
                if (!engine_) engine_ = std::make_unique<nvof::FrucEngine>(directory_ / L"runtime");
                auto batch=engine_->interpolate_pair(a,b,times);record_quality(batch.quality,times.size());record_endpoints(a.pts,b.pts,times);return batch;
            },nvof::canonical_source_rate(input_.duration));
        }
    }

    void record_endpoints(int64_t a,int64_t b,const std::vector<int64_t>& times) {
        for(auto pts:times)if((pts-a)*100<(b-a)*3 || (b-pts)*100<(b-a)*3)++near_endpoint_frames_;
    }

    void record_quality(const nvof::PairQuality& quality,size_t count) {
        if(count)engine_active_=true;
        subpixel_unavailable_=subpixel_unavailable_||quality.subpixel_unavailable;
        midpoint_unavailable_=midpoint_unavailable_||quality.midpoint_stabilization_unavailable;
        if(quality.midpoint_budget_limited)midpoint_limited_frames_+=count;
        if(quality.scene_cut){++scene_cuts_;protected_frames_+=count;quality_state_="scene-cut-hold";}
        else if(quality.identical_warp_skipped){identical_frames_+=count;quality_state_="identical-input-hold";}
        else if(quality.repeated_mask){++repeated_pairs_;protected_frames_+=count;quality_state_="fruc-repetition-hold";}
        else{
            motion_frames_+=count;quality_state_=quality.native_synthesized_mask?"native-motion-synthesis":"motion-interpolation";
            for(uint32_t bits=quality.midpoint_stabilized_mask;bits;bits>>=1)midpoint_pass_frames_+=bits&1U;
            for(uint32_t bits=quality.appearance_protected_mask;bits;bits>>=1)appearance_pass_frames_+=bits&1U;
            for(uint32_t bits=quality.subpixel_refined_mask;bits;bits>>=1)subpixel_pass_frames_+=bits&1U;
        }
    }

    void reset_history(bool same_input=false) noexcept {
        cpu_last_frame_duration_=0;
        if(cpu_upload_)cpu_upload_->reset(same_input);
        engine_active_ = false;quality_state_="waiting";
        if (pipeline_) pipeline_->reset();
        if (engine_) engine_->reset();
        if(gpu_pipeline_)gpu_pipeline_->reset();
        if(gpu_engine_)gpu_engine_->reset();
    }

    void release_engine() noexcept {
        cpu_upload_.reset();
        pipeline_.reset();
        engine_.reset();
        gpu_pipeline_.reset();
        gpu_engine_.reset();
    }

    void release_native_activation(bool reset_transport=true) noexcept {
        release_engine();
        cpu_upload_active_=false;cpu_upload_.reset();
        cpu_bridge_.reset();
        native_gpu_active_=false;native_pending_=false;gpu_delivery_verified_=false;
        native_context_.Reset();native_device_.Reset();
        mf_bridge_.reset();mf_surface_selected_=false;
        native_mutex_=nullptr;
        native_mutex_lifetime_.reset(); // Output retains the exact borrowed handle until disconnect.
        if(reset_transport && m_pOutput) {
            const HRESULT result=static_cast<nvof::transport::OutputPin*>(m_pOutput)->set_gpu_mode(false);
            if(FAILED(result))write_log("output transport reset deferred until allocator samples are returned");
        }
    }

    HRESULT report_error(const std::string& text, HRESULT hr) noexcept {
        failed_ = true;
        // Capture the D3D failure before releasing history; a CUDA interop
        // error can be a delayed report of an earlier device removal.
        std::string detail=text;
        if(native_device_) {
            const HRESULT reason=native_device_->GetDeviceRemovedReason();
            if(FAILED(reason))detail+=" D3D11DeviceRemovedReason="+std::to_string(static_cast<unsigned long>(reason));
        }
        write_log(detail + " HRESULT=" + std::to_string(static_cast<unsigned long>(hr)));
        reset_history();
        write_status("error", detail);
        NotifyEvent(EC_ERRORABORT, hr, 0);
        return hr;
    }

    HRESULT deliver(const nvof::OutputFrame& item) {
        if (cancelled_.load(std::memory_order_acquire)) return S_FALSE;
        if (item.frame.width != output_.width || item.frame.height != output_.height ||
            item.frame.pixels.size() != size_t(output_.width) * output_.height * 3 / 2 ||
            item.stop <= item.frame.pts)
            return report_error("invalid output frame layout or timestamp", E_UNEXPECTED);
        IMediaSample* raw = nullptr;
        REFERENCE_TIME start = item.frame.pts;
        REFERENCE_TIME stop = item.stop;
        HRESULT hr = m_pOutput->GetDeliveryBuffer(&raw, &start, &stop, 0);
        SamplePtr sample(raw);
        if (hr != S_OK) return hr;
        if (cancelled_.load(std::memory_order_acquire)) return S_FALSE;
        AM_MEDIA_TYPE* changed = nullptr;
        if (sample->GetMediaType(&changed) == S_OK && changed) {
            CMediaType type(*changed);
            DeleteMediaType(changed);
            Layout new_layout;
            if (FAILED(CheckTransform(&m_pInput->CurrentMediaType(), &type)) || !parse_layout(type, new_layout))
                return report_error("renderer requested an unsupported output format", VFW_E_TYPE_NOT_ACCEPTED);
            hr = m_pOutput->SetMediaType(&type);
            if (FAILED(hr)) return hr;
            output_ = new_layout;
        }
        if (sample->GetSize() < output_.size)
            return report_error("renderer output allocator is too small", VFW_E_BUFFER_OVERFLOW);
        BYTE* destination = nullptr;
        hr = sample->GetPointer(&destination);
        if (FAILED(hr) || !destination) return FAILED(hr) ? hr : E_POINTER;
        // Initialize padding so the renderer never reads stale buffer bytes.
        std::memset(destination, 0, size_t(output_.pitch) * output_.height);
        std::memset(destination + size_t(output_.pitch) * output_.height, 128,
            size_t(output_.pitch) * output_.height / 2);
        for (int row = 0; row < output_.height * 3 / 2; ++row)
            std::memcpy(destination + size_t(row) * output_.pitch,
                item.frame.pixels.data() + size_t(row) * output_.width, output_.width);
        sample->SetActualDataLength(output_.size);
        sample->SetTime(&start, &stop);
        sample->SetMediaTime(nullptr, nullptr);
        sample->SetSyncPoint(TRUE);
        sample->SetDiscontinuity(item.discontinuity ? TRUE : FALSE);
        sample->SetPreroll(preroll_ || start < 0 ? TRUE : FALSE);
        if (cancelled_.load(std::memory_order_acquire)) return S_FALSE;
        // m_csReceive is held by the input pin; m_csFilter is never acquired
        // in this call path. BeginFlush remains able to unblock this call.
        hr = m_pOutput->Deliver(sample.get());
        if (hr == S_OK) {
            ++output_frames_;
            write_status(!interpolation_requested() ? "bypass" : engine_active_ ? "active" : "waiting",
                !interpolation_requested() ? bypass_message() : engine_active_ ?
                "NVIDIA Optical Flow / Motion phases" : "Waiting for the next adjacent frame");
        }
        return hr;
    }

    void write_status(const std::string& state, const std::string& message) noexcept {
        try {
            if(!status_writer_)return;
            const ULONGLONG now=GetTickCount64();
            const bool transition=state!=last_status_state_;
            if (!transition && now-last_status_time_<1000) return;
            nvof::PlaybackTimer status_timer(performance_.enabled,performance_.status);
            last_status_time_=now;
            last_status_state_=state;
            wchar_t local[32768]{};
            const DWORD length=GetEnvironmentVariableW(L"LOCALAPPDATA",local,_countof(local));
            if (!length || length>=_countof(local)) return;
            const auto directory=std::filesystem::path(local)/L"NvofPotPlayer";
            const double input_fps=input_.duration>0 ? double(kUnits)/input_.duration : 0.0;
            const double output_fps=interpolation_requested() ? double(target_.num)/target_.den : input_fps;
            std::ostringstream stream;
                stream << "{\"processId\":" << GetCurrentProcessId()
                    << ",\"state\":" << json_string(state)
                    << ",\"transport\":" << json_string(gpu_delivery_verified_ ? "d3d11-gpu" : native_input_committed() ? "native-pending" : "system-memory")
                    << ",\"nativeInputRequested\":" << (native_input_requested_.load(std::memory_order_acquire) ? "true" : "false")
                    << ",\"nativeRendererSupported\":" << (native_renderer_supported_.load(std::memory_order_acquire) ? "true" : "false")
                    << ",\"sampleTransport\":" << json_string(sample_transport_)
                    << ",\"inputSurfaceFormat\":" << json_string(input_surface_format_)
                    << ",\"inputConversion\":" << json_string(input_conversion_)
                    << ",\"outputSurfaceFormat\":" << json_string(gpu_delivery_verified_?(output_.ten_bit?"P010":"NV12"):"")
                    << ",\"colorMode\":" << json_string(gpu_delivery_verified_?(output_.hdr_transfer==MFVideoTransFunc_2084?"HDR10":output_.hdr_transfer==MFVideoTransFunc_HLG?"HLG":"SDR"):"")
                    << ",\"hdrSideDataInputMask\":" << (hdr_trace_?hdr_trace_->input_mask.load():0)
                    << ",\"hdrSideDataRendererReadMask\":" << (hdr_trace_?hdr_trace_->returned_mask.load():0)
                    << ",\"inputRateSelected\":" << (input_rate_selected_?"true":"false")
                    << ",\"doubleRate\":" << (selected_multiple(input_.duration)==2?"true":"false")
                    << ",\"outputFpsLimit\":" << resolution_output(input_.height).limit
                    << ",\"outputMode\":" << resolution_output(input_.height).mode
                    << ",\"outputMultiplier\":" << resolution_output(input_.height).multiplier
                    << ",\"resolutionRulesEnabled\":" << (resolution_rules_enabled_?"true":"false")
                    << ",\"resolutionBand\":" << nvof::resolution_heights[nvof::resolution_band(input_.height)]
                    << ",\"resolutionRule\":" << resolution_output(input_.height).rule
                    << ",\"outputMultiple\":" << (interpolation_requested()?selected_multiple(input_.duration):1)
                    << ",\"requestedInterpolationBackend\":\"" << (native_synthesis_?"native-newton-cost":"fruc") << "\""
                    << ",\"nativeSynthesisRevision\":" << (native_synthesis_?11:0)
                    << ",\"gpuMidpointCorrection\":" << (gpu_correction_?"true":"false")
                    << ",\"appearanceProtection\":" << (appearance_protection_?"true":"false")
                    << ",\"appearancePassFrames\":" << appearance_pass_frames_
                    << ",\"inputHeightLimit\":" << input_height_limit_
                    << ",\"inputHeight\":" << input_.height
                    << ",\"inputRateMask\":" << input_rate_mask_
                    << ",\"inputRateGroup\":" << json_string(input_rate_group())
                    << ",\"bypassReason\":" << json_string(bypass_reason())
                    << ",\"inputFps\":" << input_fps
                    << ",\"outputFps\":" << output_fps
                    << ",\"outputFrames\":" << output_frames_
                    << ",\"sourceFrames\":" << source_frames_
                    << ",\"nearEndpointFrames\":" << near_endpoint_frames_
                    << ",\"sourceCadenceAligned\":" << ((gpu_pipeline_?gpu_pipeline_->source_cadence_active():pipeline_?pipeline_->source_cadence_active():false)?"true":"false")
                    << ",\"identicalFrames\":" << identical_frames_
                    << ",\"motionFrames\":" << motion_frames_
                    << ",\"midpointLimitedFrames\":" << midpoint_limited_frames_
                    << ",\"midpointPassFrames\":" << midpoint_pass_frames_
                    << ",\"midpointUnavailable\":" << (midpoint_unavailable_?"true":"false")
                    << ",\"subpixelPassFrames\":" << subpixel_pass_frames_
                    << ",\"subpixelUnavailable\":" << (subpixel_unavailable_?"true":"false")
                    << ",\"protectedFrames\":" << protected_frames_
                    << ",\"sceneCuts\":" << scene_cuts_
                    << ",\"repeatedPairs\":" << repeated_pairs_
                    << ",\"qualityState\":" << json_string(quality_state_)
                    << ",\"buildVersion\":" << json_string(kFilterBuild)
                    << ",\"gpuCompletion\":" << json_string(gpu_engine_&&gpu_engine_->queued_completion()?"context-ordered":"blocking")
                    << ",\"algorithm\":" << json_string(native_synthesis_?"integer-native-newton-0.4.2":midpoint_pass_frames_?"x2-slow-motion-stabilized":appearance_pass_frames_?"x2-appearance-protected":subpixel_pass_frames_?"independent-motion-phases-subpixel":"independent-motion-phases")
                    << ",\"message\":" << json_string(message) << "}\n";
            // Snapshot mutable counters on their existing owner thread. Only
            // immutable JSON is passed to the optional disk worker.
            status_writer_->publish(directory,stream.str());
            if(transition)status_writer_->drain();
        } catch (...) { /* Status is optional and cannot break decoding. */ }
    }

    const unsigned instance_id_=next_filter_instance.fetch_add(1);
    bool async_input_enabled_=true,async_first_input_=true,worker_processing_=false;
    std::mutex async_mutex_;
    std::condition_variable async_cv_;
    std::deque<PendingInput> async_queue_;
    std::thread async_thread_;
    bool async_stop_=false,async_busy_=false,async_paused_=true;
    std::deque<nvof::GpuOutputFrame> pending_gpu_outputs_;
    size_t output_ahead_peak_=0,output_ahead_pairs_=0;
    bool wait_service_active_=false;
    size_t wait_output_frames_=0;
    HRESULT wait_delivery_result_=S_OK;
    HRESULT async_result_=S_OK;
    size_t async_queued_=0,async_peak_=0;
    double async_backpressure_ms_=0;
    std::atomic<unsigned> topology_logs_{0};
    DiagnosticServiceProbe service_probe_;
    bool cpu_field_override_logged_=false;
    bool probe_services_=false;
    nvof::PlaybackTiming performance_;
    bool trace_every_gpu_pair_=false;
    std::filesystem::path directory_;
    Layout input_, output_;
    bool native_feature_enabled_=false;
    bool cpu_upload_active_=false,starting_cpu_upload_=false;
    double cpu_prepare_ms_=0,cpu_gpu_ms_=0;
    uint64_t cpu_upload_fields_=0;
    int64_t cpu_last_frame_duration_=0;
    std::shared_ptr<nvof::MfD3d11Bridge> cpu_bridge_;
    std::unique_ptr<nvof::CpuUpload> cpu_upload_;
    bool native_gpu_active_=false;
    bool renderer_gpu_committed_=false; // Survives input-only reconnects.
    std::optional<uint64_t> renderer_adapter_luid_; // Actual accepted GPU, survives input-only reconnects.
    bool gpu_delivery_verified_=false;
    bool native_pending_=false;
    bool pending_gpu_media_type_=false;
    UINT native_flags_=0;
    Microsoft::WRL::ComPtr<ID3D11Device> native_device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> native_context_;
    HANDLE native_mutex_=nullptr;
    std::shared_ptr<void> native_mutex_lifetime_,renderer_mutex_lifetime_;
    std::mutex mf_service_mutex_;
    std::shared_ptr<nvof::MfD3d11Bridge> mf_bridge_;
    bool mf_surface_selected_=false;
    std::unique_ptr<nvof::GpuFrucEngine> gpu_engine_;
    std::unique_ptr<nvof::GpuPhasePipeline> gpu_pipeline_;
    std::atomic<unsigned> filter_probe_queries_{0};
    std::atomic<bool> native_input_requested_{false};
    std::atomic<bool> native_renderer_supported_{false};
    bool sample_probed_ = false;
    std::shared_ptr<nvof::HdrDeliveryTrace> hdr_trace_;
    std::string sample_transport_ = "unknown";
    std::string input_surface_format_="unknown",input_conversion_="none";
    bool enabled_ = true;
    int output_limit_=60, output_mode_=0, output_multiplier_=2;
    unsigned input_rate_mask_=nvof::kAllInputRates;
    unsigned input_height_limit_=0;
    bool resolution_rules_enabled_=false;
    nvof::ResolutionRules resolution_rules_{};
    mutable bool color_probe_logged_=false;
    bool gpu_correction_=true,appearance_protection_=true,native_synthesis_=false;
    uint64_t appearance_pass_frames_=0;
    bool input_rate_selected_=true;
    bool engine_active_ = false;
    uint64_t output_frames_ = 0,source_frames_=0,near_endpoint_frames_=0;
    bool subpixel_unavailable_=false,midpoint_unavailable_=false;
    uint64_t motion_frames_=0,protected_frames_=0,scene_cuts_=0,repeated_pairs_=0,subpixel_pass_frames_=0,identical_frames_=0,midpoint_pass_frames_=0,midpoint_limited_frames_=0;
    std::string quality_state_="waiting";
    ULONGLONG last_status_time_ = 0;
    std::string last_status_state_;
    std::shared_ptr<nvof::StatusWriter> status_writer_;
    nvof::Rate target_;
    std::atomic<bool> cancelled_{false};
    bool failed_ = false;
    bool preroll_ = false;
    nvof::SeekPreviewWindow seek_preview_;
    bool seen_segment_=false,seek_preview_frame_=false;
    unsigned seek_trace_count_=0;
    std::optional<int64_t> last_pts_;
    int64_t last_duration_ = kUnits / 24;
    std::unique_ptr<nvof::FrucEngine> engine_;
    std::unique_ptr<nvof::PhasePipeline> pipeline_;
};

ProbeInputPin::ProbeInputPin(NvofFilter* owner,HRESULT* result)
    : CTransformInputPin(NAME("NVIDIA interpolation input"),owner,result,L"Input"),owner_(owner) { }

STDMETHODIMP ProbeInputPin::NonDelegatingQueryInterface(REFIID id,void** value) {
    if(!value)return E_POINTER;
    // Expose the implemented D3D11 contract only with the experimental flag
    // and a compatible downstream renderer/allocator. Other GPU contracts
    // remain read-only probes with the unchanged base-class answer.
    HRESULT result;
    if(id==__uuidof(nvof::transport::DecoderConfiguration) && owner_->native_renderer_ready())
        result=GetInterface(static_cast<nvof::transport::DecoderConfiguration*>(this),value);
    else if(id==__uuidof(nvof::transport::TextureConfiguration) && owner_->native_renderer_ready())
        result=GetInterface(static_cast<nvof::transport::TextureConfiguration*>(this),value);
    else if(id==__uuidof(IMFGetService) && owner_->service_probe_enabled())
        result=GetInterface(static_cast<IMFGetService*>(this),value);
    else result=CTransformInputPin::NonDelegatingQueryInterface(id,value);
    const unsigned mask=id==kD3D11DecoderConfiguration ? 1u :
        id==kD3D11DecoderTextureConfiguration ? 2u :
        id==__uuidof(IMFGetService) ? 4u : id==__uuidof(IDirect3DDeviceManager9) ? 8u : 0u;
    if (mask && !(observed_queries_.fetch_or(mask,std::memory_order_acq_rel)&mask)) {
        const char* name=mask==1 ? "ID3D11DecoderConfiguration" : mask==2 ?
            "ID3D11DecoderTextureConfiguration" : mask==4 ? "IMFGetService" : "IDirect3DDeviceManager9";
        owner_->note_native_input_query(name,result,mask<4);
    }
    return result;
}

STDMETHODIMP ProbeInputPin::ReceiveConnection(IPin* connector,const AM_MEDIA_TYPE* type) {
    candidate=connector;
    const HRESULT hr=CTransformInputPin::ReceiveConnection(connector,type);
    candidate=nullptr;
    if(SUCCEEDED(hr)){CAutoLock lock(m_pLock);first_allocator_pending_=true;}
    return hr;
}
HRESULT ProbeInputPin::BreakConnect() {
    first_allocator_pending_=false;offered_host_allocator_.Reset();
    return CTransformInputPin::BreakConnect();
}

STDMETHODIMP ProbeInputPin::GetService(REFGUID service,REFIID requested,LPVOID* value) {
    return owner_->get_native_service("input pin",service,requested,value);
}
STDMETHODIMP ProbeInputPin::GetAllocator(IMemAllocator** allocator) {
    if(!allocator)return E_POINTER;
    owner_->trace_input_allocator("GetAllocator");
    if(owner_->native_input_committed()) {
        *allocator=nullptr;
        return E_NOTIMPL; // A native decoder must provide its own GPU allocator.
    }
    CAutoLock lock(m_pLock);
    const bool creating=m_pAllocator==nullptr;
    const HRESULT hr=CTransformInputPin::GetAllocator(allocator);
    if(SUCCEEDED(hr) && creating)offered_host_allocator_=*allocator;
    return hr;
}
STDMETHODIMP ProbeInputPin::NotifyAllocator(IMemAllocator* allocator,BOOL read_only) {
    bool confirmed_host=false;
    {
        CAutoLock lock(m_pLock);
        confirmed_host=first_allocator_pending_ && allocator && allocator==offered_host_allocator_.Get();
        first_allocator_pending_=false;
    }
    owner_->trace_input_allocator("NotifyAllocator",confirmed_host);
    const HRESULT hr=CTransformInputPin::NotifyAllocator(allocator,read_only);
    if(FAILED(hr) || !confirmed_host)return hr;
    return owner_->prefer_confirmed_cpu_hdr_input();
}
STDMETHODIMP ProbeInputPin::ActivateD3D11Decoding(ID3D11Device* device,ID3D11DeviceContext* context,HANDLE mutex,UINT flags) {
    return owner_->activate_native(device,context,mutex,flags);
}
STDMETHODIMP_(UINT) ProbeInputPin::GetD3D11AdapterIndex() {
    try {return owner_->native_adapter();}
    catch(const std::exception& e) {write_log(std::string("native adapter query failed: ")+e.what());return UINT_MAX;}
    catch(...) {return UINT_MAX;}
}

class NvofPropertyPage final : public CBasePropertyPage {
public:
    NvofPropertyPage(LPUNKNOWN outer,HRESULT* result)
        : CBasePropertyPage(NAME("NVIDIA Optical Flow settings"),outer,100,101) { *result=S_OK; }
    static CUnknown* WINAPI CreateInstance(LPUNKNOWN outer,HRESULT* result) {
        if (!result) return nullptr;
        auto* page=new(std::nothrow) NvofPropertyPage(outer,result);
        if (!page) *result=E_OUTOFMEMORY;
        return page;
    }
    INT_PTR OnReceiveMessage(HWND window,UINT message,WPARAM wparam,LPARAM lparam) override {
        if (message==WM_COMMAND && LOWORD(wparam)==1001 && HIWORD(wparam)==BN_CLICKED) {
            try {
                const auto directory=module_directory();
                const auto application=directory/L"NvofControl.exe";
                const DWORD attributes=GetFileAttributesW(application.c_str());
                if (attributes==INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    MessageBoxW(window,L"필터와 같은 폴더에서 NvofControl.exe를 찾을 수 없습니다.",
                        L"NVIDIA Optical Flow",MB_OK|MB_ICONERROR);
                    return TRUE;
                }
                const auto launched=reinterpret_cast<INT_PTR>(ShellExecuteW(window,L"open",application.c_str(),
                    nullptr,directory.c_str(),SW_SHOWNORMAL));
                if (launched<=32) MessageBoxW(window,L"설정 창을 열지 못했습니다.",
                    L"NVIDIA Optical Flow",MB_OK|MB_ICONERROR);
            } catch (...) {
                MessageBoxW(window,L"설정 프로그램의 위치를 확인하지 못했습니다.",
                    L"NVIDIA Optical Flow",MB_OK|MB_ICONERROR);
            }
            return TRUE;
        }
        return CBasePropertyPage::OnReceiveMessage(window,message,wparam,lparam);
    }
};

const AMOVIESETUP_MEDIATYPE pin_types[] = {{&MEDIATYPE_Video, &MEDIASUBTYPE_NV12},{&MEDIATYPE_Video,&nvof::p010_subtype}};
const AMOVIESETUP_MEDIATYPE input_pin_types[] = {{&MEDIATYPE_Video,&MEDIASUBTYPE_NV12},
    {&MEDIATYPE_Video,&nvof::p010_subtype},{&MEDIATYPE_Video,&nvof::p016_subtype},
    {&MEDIATYPE_Video,&nvof::p210_subtype},{&MEDIATYPE_Video,&nvof::y210_subtype}};
const AMOVIESETUP_PIN pins[] = {
    {L"Input", FALSE, FALSE, FALSE, FALSE, &CLSID_NULL, nullptr, 5, input_pin_types},
    {L"Output", FALSE, TRUE, FALSE, FALSE, &CLSID_NULL, nullptr, 2, pin_types},
};
const AMOVIESETUP_FILTER registration = {
    &CLSID_NvofPotPlayer, L"NVIDIA Optical Flow for PotPlayer", MERIT_DO_NOT_USE, 2, pins
};
} // namespace

CFactoryTemplate g_Templates[] = {
    {L"NVIDIA Optical Flow for PotPlayer", &CLSID_NvofPotPlayer,
        NvofFilter::CreateInstance, nullptr, &registration},
    {L"NVIDIA Optical Flow settings", &CLSID_NvofPropertyPage,
        NvofPropertyPage::CreateInstance, nullptr, nullptr},
};
int g_cTemplates = _countof(g_Templates);

STDAPI DllRegisterServer() { return AMovieDllRegisterServer2(TRUE); }
STDAPI DllUnregisterServer() { return AMovieDllRegisterServer2(FALSE); }

extern "C" BOOL WINAPI DllEntryPoint(HINSTANCE, ULONG, LPVOID);
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) module_handle = module;
    return DllEntryPoint(module, reason, reserved);
}
