#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <streams.h>
#include <dvdmedia.h>
#include <mfidl.h>
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
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
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

// Independent identity: never replaces the Smootter/VapourSynth filter.
// {EDECA044-78CD-40EB-8F37-63D947C501A0}
const CLSID CLSID_NvofPotPlayer =
{0xedeca044, 0x78cd, 0x40eb, {0x8f, 0x37, 0x63, 0xd9, 0x47, 0xc5, 0x01, 0xa0}};

// {DE1F386E-626B-442A-98C5-59C7713D21BA}
const CLSID CLSID_NvofPropertyPage =
{0xde1f386e, 0x626b, 0x442a, {0x98, 0xc5, 0x59, 0xc7, 0x71, 0x3d, 0x21, 0xba}};

namespace {
constexpr REFERENCE_TIME kUnits = 10000000;
constexpr char kFilterBuild[]="0.3.2-rate.3";
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
            // Bound diagnostics without retaining a playback history.
            std::ofstream truncate(path, std::ios::trunc);
        }
        SYSTEMTIME now{};
        GetLocalTime(&now);
        char prefix[96]{};
        sprintf_s(prefix, "%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu ",
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
            now.wSecond, now.wMilliseconds, GetCurrentProcessId());
        std::ofstream stream(path, std::ios::app);
        stream << prefix << message << '\n';
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
};

bool parse_layout(const CMediaType& media, Layout& layout) noexcept {
    if (media.majortype != MEDIATYPE_Video || media.subtype != MEDIASUBTYPE_NV12)
        return false;
    const BITMAPINFOHEADER* bitmap = nullptr;
    size_t bitmap_offset=0;
    RECT source{};
    bool native_color_supported = true;
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
        if (!video || (video->dwInterlaceFlags & AMINTERLACE_IsInterlaced)) return false;
        if(video->dwControlFlags & AMCONTROL_COLORINFO_PRESENT) {
            DXVA2_ExtendedFormat color{};color.value=video->dwControlFlags;
            native_color_supported=(color.NominalRange==DXVA2_NominalRange_Unknown || color.NominalRange==DXVA2_NominalRange_16_235) &&
                (color.VideoTransferMatrix==DXVA2_VideoTransferMatrix_Unknown || color.VideoTransferMatrix==DXVA2_VideoTransferMatrix_BT709) &&
                (color.VideoPrimaries==DXVA2_VideoPrimaries_Unknown || color.VideoPrimaries==DXVA2_VideoPrimaries_BT709) &&
                (color.VideoTransferFunction==DXVA2_VideoTransFunc_Unknown || color.VideoTransferFunction==DXVA2_VideoTransFunc_709);
        }
        bitmap = &video->bmiHeader;
        bitmap_offset=offsetof(VIDEOINFOHEADER2,bmiHeader);
        source = video->rcSource;
        duration = video->AvgTimePerFrame;
    } else return false;
    if (bitmap->biSize < sizeof(BITMAPINFOHEADER) || bitmap->biSize>media.cbFormat-bitmap_offset || bitmap->biWidth <= 0 ||
        bitmap->biHeight == std::numeric_limits<LONG>::min() ||
        bitmap->biCompression != MAKEFOURCC('N', 'V', '1', '2') ||
        bitmap->biPlanes != 1 || bitmap->biBitCount != 12) return false;
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
    int pitch = bitmap->biWidth;
    const int64_t rows = int64_t(storage_height) * 3 / 2;
    if (bitmap->biSizeImage) {
        if (bitmap->biSizeImage < rows * pitch) return false;
        // A larger integral row size is a padded surface; do not infer pitch
        // from allocator capacity, which may include unrelated spare bytes.
        if (bitmap->biSizeImage % rows == 0) {
            const int64_t advertised_pitch = bitmap->biSizeImage / rows;
            if (advertised_pitch < pitch || advertised_pitch > 32768) return false;
            pitch = static_cast<int>(advertised_pitch);
        }
    }
    if ((pitch & 1) || pitch < width) return false;
    const int64_t size = rows * pitch;
    if (size > std::numeric_limits<long>::max()) return false;
    layout = {width, height, pitch, static_cast<long>(size), duration, native_color_supported};
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
HRESULT tag_native_output_color(CMediaType& media,const Layout& layout) noexcept {
    try {
        const DWORD controls=native_output_color_flags(media);
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
    STDMETHODIMP_(UINT) GetD3D11AdapterIndex() override;
    STDMETHODIMP_(UINT) GetD3D11TextureBindFlags() override {return D3D11_BIND_SHADER_RESOURCE;}
    STDMETHODIMP_(UINT) GetD3D11TextureMiscFlags() override {return D3D11_RESOURCE_MISC_SHARED;}
private:
    NvofFilter* owner_;
    std::atomic<unsigned> observed_queries_{0};
    DiagnosticServiceProbe service_probe_;
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
    STDMETHODIMP GetService(REFGUID service,REFIID requested,LPVOID* value) override {
        return get_native_service("filter",service,requested,value);
    }
    bool native_input_committed() const noexcept {return native_gpu_active_ || native_pending_;}
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
        try {
            directory_ = module_directory();
            const auto ini = directory_ / L"NvofPotPlayer.ini";
            probe_services_ = GetPrivateProfileIntW(L"Nvof", L"ProbeServices", 0, ini.c_str()) != 0;
            native_feature_enabled_ = GetPrivateProfileIntW(L"Nvof", L"NativeD3D11", 0, ini.c_str()) != 0;
            enabled_ = GetPrivateProfileIntW(L"Nvof", L"Enabled", 1, ini.c_str()) != 0;
            // Historical INI keys cannot re-enable the removed FRUC backend.
            gpu_correction_=false;appearance_protection_=false;native_synthesis_=true;

            input_rate_mask_=GetPrivateProfileIntW(L"Nvof",L"InputRateMask",nvof::kAllInputRates,ini.c_str()) & nvof::kAllInputRates;
            // Integer multiples only; an absent/invalid limit means 60 fps.
            output_limit_=nvof::normalize_output_limit(int(GetPrivateProfileIntW(L"Nvof",L"OutputFpsLimit",60,ini.c_str())));
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
        if(renderer)return renderer->GetD3D11AdapterIndex();
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))return UINT_MAX;
        UINT first_nvidia=UINT_MAX;
        for(UINT index=0;;++index) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            if(factory->EnumAdapters1(index,&adapter)==DXGI_ERROR_NOT_FOUND)break;
            DXGI_ADAPTER_DESC1 description{};
            if(adapter && SUCCEEDED(adapter->GetDesc1(&description)) && description.VendorId==0x10de &&
                !(description.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)) {
                if(wcsstr(description.Description,L"5090"))return index;
                if(first_nvidia==UINT_MAX)first_nvidia=index;
            }
        }
        return first_nvidia;
    }

    HRESULT activate_native(ID3D11Device* device,ID3D11DeviceContext* context,HANDLE mutex,UINT flags) {
        if(!device || !context || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return E_INVALIDARG;
        if(!native_feature_enabled_)return E_NOINTERFACE;
        if(m_State!=State_Stopped)return VFW_E_NOT_STOPPED;
        if(!input_.native_color_supported) {
            write_log("native activation rejected: only limited-range BT.709 SDR is supported");
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
            if(renderer)renderer_mutex_lifetime_=duplicate_owner;
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
                Microsoft::WRL::ComPtr<nvof::transport::DecoderConfiguration> renderer;
                HRESULT activated=connected->QueryInterface(IID_PPV_ARGS(&renderer));
                if(SUCCEEDED(activated))activated=renderer->ActivateD3D11Decoding(native_device_.Get(),
                    native_context_.Get(),native_mutex_,native_flags_);
                if(FAILED(activated)) {
                    write_log("pending native activation rejected by output HRESULT="+
                        std::to_string(static_cast<unsigned long>(activated)));
                    return activated;
                }
                renderer_mutex_lifetime_=native_mutex_lifetime_;
                native_pending_=false;
                native_gpu_active_=true;
                write_log("pending native activation completed by compatible renderer");
            }
            try {
                write_log("native probe: renderer input ID3D11DecoderConfiguration HRESULT="+
                    std::to_string(static_cast<unsigned long>(result))+
                    "; native activation is reported separately");
            } catch (...) { }
        }
        return CTransformFilter::CompleteConnect(direction,connected);
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
    nvof::Rate requested_target(REFERENCE_TIME duration) const {
        return nvof::capped_source_rate(duration,output_limit_);
    }
    bool same_target(REFERENCE_TIME duration) const {
        const auto incoming=requested_target(duration);
        // Output-rate bounds keep both cross-products below 1e16. A rounded
        // AvgTimePerFrame match alone is insufficient for the x2 clock.
        return incoming.num*target_.den==target_.num*incoming.den;
    }
    bool target_available(REFERENCE_TIME duration) const {
        return nvof::supported_output_rate(requested_target(duration));
    }
    bool source_reaches_target(REFERENCE_TIME duration) const {
        const auto target=requested_target(duration);
        return duration>0 && nvof::supported_output_rate(target) && duration<=rate_duration(target)+1;
    }
    bool interpolate_source(REFERENCE_TIME duration) const {
        return enabled_ && rate_selected(duration) && target_available(duration) && !source_reaches_target(duration);
    }
    bool interpolation_requested() const {return interpolate_source(input_.duration);}
    REFERENCE_TIME negotiated_duration(REFERENCE_TIME duration) const {
        return interpolate_source(duration)?rate_duration(requested_target(duration)):duration;
    }
    const char* bypass_reason() const {
        if(!enabled_)return "disabled";
        if(!input_rate_selected_)return "source-rate";
        if(!target_available(input_.duration))return input_.duration<=0?"unknown-source-rate":"output-rate-unsupported";
        if(source_reaches_target(input_.duration))return "at-or-above-target";
        return "";
    }
    const char* bypass_message() const {
        if(!enabled_)return "Interpolation disabled";
        if(!input_rate_selected_)return "Source frame rate excluded; original frames preserved";
        if(!target_available(input_.duration))return input_.duration<=0?
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
    bool connected_output_color_matches(const CMediaType& input) const noexcept {
        if(!native_feature_enabled_)return true;
        if(!m_pOutput)return false;
        const auto& output=m_pOutput->CurrentMediaType();
        return output.formattype==FORMAT_VideoInfo2 && output.pbFormat && output.cbFormat>=sizeof(VIDEOINFOHEADER2) &&
            reinterpret_cast<const VIDEOINFOHEADER2*>(output.pbFormat)->dwControlFlags==native_output_color_flags(input);
    }
    HRESULT CheckInputType(const CMediaType* type) override {
        if (!type) return E_POINTER;
        Layout layout;
        return parse_layout(*type, layout) ? S_OK : VFW_E_TYPE_NOT_ACCEPTED;
    }

    HRESULT CheckTransform(const CMediaType* input, const CMediaType* output) override {
        if (!input || !output) return E_POINTER;
        Layout in, out;
        if (!parse_layout(*input, in) || !parse_layout(*output, out) ||
            in.width != out.width || in.height != out.height)
            return VFW_E_TYPE_NOT_ACCEPTED;
        if(native_feature_enabled_) {
            if(output->formattype!=FORMAT_VideoInfo2)return VFW_E_TYPE_NOT_ACCEPTED;
            const auto* header=reinterpret_cast<const VIDEOINFOHEADER2*>(output->pbFormat);
            if(header->dwControlFlags!=native_output_color_flags(*input))return VFW_E_TYPE_NOT_ACCEPTED;
        }
        const auto duration = negotiated_duration(in.duration);
        if ((duration>0 && out.duration<=0) || std::llabs(out.duration - duration) > 1)
            return VFW_E_TYPE_NOT_ACCEPTED;
        return S_OK;
    }

    HRESULT SetMediaType(PIN_DIRECTION direction, const CMediaType* type) override {
        if (!type) return E_POINTER;
        Layout layout;
        if (!parse_layout(*type, layout)) return VFW_E_TYPE_NOT_ACCEPTED;
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
            hr=tag_native_output_color(*media,input_);
            if(FAILED(hr))return hr;
        }
        BITMAPINFOHEADER* bitmap = nullptr;
        RECT* source = nullptr;
        RECT* destination = nullptr;
        DWORD* bit_rate = nullptr;
        const REFERENCE_TIME duration = negotiated_duration(input_.duration);
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
        *source = {0, 0, input_.width, input_.height};
        *destination = {0, 0, 0, 0};
        bitmap->biWidth = input_.width;
        bitmap->biHeight = input_.height;
        bitmap->biSizeImage = static_cast<DWORD>(int64_t(input_.width) * input_.height * 3 / 2);
        bitmap->biBitCount = 12;
        bitmap->biPlanes = 1;
        bitmap->biCompression = MAKEFOURCC('N', 'V', '1', '2');
        *bit_rate = 0;
        media->SetSampleSize(bitmap->biSizeImage);
        media->SetTemporalCompression(FALSE);
        return S_OK;
    }

    HRESULT DecideBufferSize(IMemAllocator* allocator, ALLOCATOR_PROPERTIES* properties) override {
        if (!allocator || !properties) return E_POINTER;
        if (!output_.size) return VFW_E_NOT_CONNECTED;
        properties->cBuffers = (std::max)(properties->cBuffers, 4L);
        properties->cbBuffer = (std::max)(properties->cbBuffer, output_.size);
        properties->cbAlign = (std::max)(properties->cbAlign, 1L);
        properties->cbPrefix = (std::max)(properties->cbPrefix, 0L);
        ALLOCATOR_PROPERTIES actual{};
        HRESULT hr = allocator->SetProperties(properties, &actual);
        if (FAILED(hr)) return hr;
        return actual.cbBuffer >= properties->cbBuffer && actual.cBuffers >= properties->cBuffers
            ? S_OK : E_FAIL;
    }

    HRESULT StartStreaming() override {
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
        last_pts_.reset();
        last_duration_ = input_.duration > 0 ? input_.duration :
            nvof::supported_output_rate(target_)?rate_duration(target_):kUnits/24;
        failed_ = false;
        appearance_pass_frames_=0;
        output_frames_ = source_frames_ = near_endpoint_frames_ = 0;motion_frames_=protected_frames_=scene_cuts_=repeated_pairs_=subpixel_pass_frames_=identical_frames_=midpoint_pass_frames_=midpoint_limited_frames_=0;subpixel_unavailable_=midpoint_unavailable_=false;quality_state_="waiting";
        gpu_delivery_verified_=false;
        sample_probed_ = false;
        sample_transport_ = "unknown";
        input_surface_format_="unknown";input_conversion_="none";
        engine_active_ = false;
        reset_history();
        write_status(interpolation_requested()?"waiting":"bypass",interpolation_requested()?"Waiting for the next adjacent frame":bypass_message());
        write_log("stream start: NV12 " + std::to_string(input_.width) + "x" +
            std::to_string(input_.height) + " target=" + std::to_string(target_.num) +
            "/" + std::to_string(target_.den) + " inputRateGroup="+input_rate_group()+
            " InputRateMask="+std::to_string(input_rate_mask_)+" DoubleRate="+std::to_string(nvof::output_multiple(input_.duration,output_limit_)==2)+" bypassReason="+bypass_reason());
        return S_OK;
    }

    HRESULT StopStreaming() override {
        // CTransformFilter::Stop holds m_csReceive after input decommit.
        cancelled_.store(true, std::memory_order_release);
        release_engine();
        last_pts_.reset();
        write_log("stream stop: GPU resources released after receive quiesced");
        write_status("stopped", "Playback stopped");
        return S_OK;
    }

    STDMETHODIMP Stop() override {
        cancelled_.store(true, std::memory_order_release);
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
        const HRESULT result=CTransformFilter::Stop();
        if (output && SUCCEEDED(flushed)) output->DeliverEndFlush();
        return result;
    }

    HRESULT BeginFlush() override {
        cancelled_.store(true, std::memory_order_release);
        // The input pin has already rejected new Receive calls. Crucially,
        // downstream flush precedes m_csReceive to unblock Deliver/GetBuffer.
        if(m_pOutput)static_cast<nvof::transport::OutputPin*>(m_pOutput)->cancel_allocation_waits();
        const HRESULT hr = CTransformFilter::BeginFlush();
        {
            CAutoLock receive(&m_csReceive);
            reset_history();
            last_pts_.reset();
        }
        return hr;
    }

    HRESULT EndFlush() override {
        {
            CAutoLock receive(&m_csReceive);
            reset_history();
            last_pts_.reset();
            failed_ = false;
        }
        if(m_pOutput) {
            const HRESULT recommitted=static_cast<nvof::transport::OutputPin*>(m_pOutput)->resume_allocation();
            if(FAILED(recommitted))return recommitted;
        }
        const HRESULT hr = CTransformFilter::EndFlush();
        if (SUCCEEDED(hr)) cancelled_.store(false, std::memory_order_release);
        // The base input pin unblocks Receive only after this returns.
        return hr;
    }

    HRESULT NewSegment(REFERENCE_TIME start, REFERENCE_TIME stop, double rate) override {
        if (!std::isfinite(rate) || rate <= 0) return E_INVALIDARG;
        CAutoLock receive(&m_csReceive);
        reset_history();
        last_pts_.reset();
        failed_ = false;
        return CTransformFilter::NewSegment(start, stop, rate);
    }

    HRESULT Receive(IMediaSample* sample) override {
        if (!sample) return E_POINTER;
        if (cancelled_.load(std::memory_order_acquire)) return S_FALSE;
        if (failed_) return E_FAIL;
        if(native_pending_)return VFW_E_NOT_CONNECTED;
        const auto* properties = m_pInput->SampleProps();
        if (properties->dwStreamId != AM_STREAM_MEDIA) return m_pOutput->Deliver(sample);
        try {
            if (!sample_probed_) probe_sample(sample);
            if (properties->dwSampleFlags & AM_SAMPLE_TYPECHANGED) {
                const auto* media = reinterpret_cast<const CMediaType*>(properties->pMediaType);
                Layout incoming;
                if (!media || !parse_layout(*media, incoming) ||
                    incoming.width != output_.width || incoming.height != output_.height ||
                    !connected_output_color_matches(*media) ||
                    rate_selected(incoming.duration)!=input_rate_selected_ ||
                    interpolate_source(incoming.duration)!=interpolation_requested() ||
                    (interpolation_requested() && !same_target(incoming.duration)) ||
                    std::llabs(negotiated_duration(incoming.duration)-output_.duration)>1)
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
            // Decoder preroll belongs before the new visible segment. FRUC
            // accepts only nonnegative timestamps; retaining a preroll frame
            // would poison the first post-seek pair. Discard it without GPU
            // work and let the first visible original create fresh history.
            if (start < 0 || sample->IsPreroll() == S_OK) {
                reset_history();
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
            if (discontinuity) reset_history();
            const int64_t end=timing==S_OK && stop>start?stop:start+last_duration_;
            ++source_frames_;
            if(native_gpu_active_) {
                last_pts_=start;
                return receive_gpu(sample,start,end,discontinuity);
            }
            if (interpolation_requested()) ensure_pipeline();
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
            if (!interpolation_requested()) {
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
        if (cancelled_.load(std::memory_order_acquire)) return S_FALSE;
        if (failed_) return E_FAIL;
        try {
            HRESULT delivery = S_OK;
            if(gpu_pipeline_)gpu_pipeline_->finish(last_duration_,[this,&delivery](const nvof::GpuOutputFrame& output) {
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
    HRESULT receive_gpu(IMediaSample* sample,int64_t start,int64_t stop,bool discontinuity) {
        auto input=nvof::extract_gpu_frame(sample,input_.width,input_.height,start,input_.native_color_supported);
        D3D11_TEXTURE2D_DESC surface{};input.texture->GetDesc(&surface);
        const bool p010=surface.Format==DXGI_FORMAT_P010;
        const std::string format=p010?"P010":"NV12";
        if(input_surface_format_!=format) {
            input_surface_format_=format;
            input_conversion_=p010?"p010-to-nv12-gpu":"none";
            write_log("GPU input surface="+format+" output=NV12 conversion="+input_conversion_);
        }
        if(!gpu_engine_)gpu_engine_=std::make_unique<nvof::GpuFrucEngine>(directory_/L"runtime",
            native_device_.Get(),native_context_.Get(),native_mutex_,nvof::GpuCompletionMode::context_ordered,true,gpu_correction_,appearance_protection_,nvof::GpuInterpolationBackend::native_experimental);
        // Submit capture while the upstream sample is held. Decoder reuse and
        // renderer reads are ordered on this device's shared immediate context;
        // keep an owned output lease without a per-frame CPU completion wait.
        auto owned=gpu_engine_->copy(input);
        sample_transport_="d3d11-texture";
        if(!interpolation_requested())return deliver_gpu({std::move(owned),stop>start?stop:start+last_duration_,discontinuity});
        if(!gpu_pipeline_)gpu_pipeline_=std::make_unique<nvof::GpuPhasePipeline>(target_,
            [this](const nvof::GpuFrame& a,const nvof::GpuFrame& b,const std::vector<int64_t>& times) {
                auto batch=gpu_engine_->interpolate_pair(a,b,times);record_quality(batch.quality,times.size());record_endpoints(a.pts,b.pts,times);return batch;
            },nvof::canonical_source_rate(input_.duration));
        HRESULT delivery=S_OK;
        gpu_pipeline_->push(std::move(owned),discontinuity,[this,&delivery](const nvof::GpuOutputFrame& output) {
            delivery=deliver_gpu(output);return delivery==S_OK;
        });
        return delivery;
    }

    HRESULT deliver_gpu(const nvof::GpuOutputFrame& output) {
        if(cancelled_.load(std::memory_order_acquire))return S_FALSE;
        if(!output.frame.texture || output.frame.width!=output_.width || output.frame.height!=output_.height ||
            output.stop<=output.frame.pts)return report_error("invalid native GPU output",E_UNEXPECTED);
        IMediaSample* raw=nullptr;
        REFERENCE_TIME start=output.frame.pts,stop=output.stop;
        HRESULT result=m_pOutput->GetDeliveryBuffer(&raw,&start,&stop,0);
        SamplePtr sample(raw);
        if(result!=S_OK)return result;
        if(cancelled_.load(std::memory_order_acquire))return S_FALSE;
        auto* surface=dynamic_cast<nvof::transport::SurfaceSample*>(raw);
        if(!surface)return report_error("renderer did not accept the native GPU allocator",E_UNEXPECTED);
        surface->assign(output.frame);
        sample->SetTime(&start,&stop);
        sample->SetActualDataLength(output_.width*output_.height*3/2);
        sample->SetMediaTime(nullptr,nullptr);
        sample->SetSyncPoint(TRUE);
        sample->SetDiscontinuity(output.discontinuity);
        sample->SetPreroll(FALSE);
        if(cancelled_.load(std::memory_order_acquire))return S_FALSE;
        result=m_pOutput->Deliver(sample.get());
        if(result==S_OK) {
            gpu_delivery_verified_=true;
            ++output_frames_;
            write_status(!interpolation_requested()?"bypass":engine_active_?"active":"waiting",
                !interpolation_requested()?bypass_message():engine_active_?"NVIDIA Optical Flow / Motion phases (D3D11 GPU)":
                    "Waiting for the next adjacent GPU frame");
        }
        return result;
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

    void reset_history() noexcept {
        engine_active_ = false;quality_state_="waiting";
        if (pipeline_) pipeline_->reset();
        if (engine_) engine_->reset();
        if(gpu_pipeline_)gpu_pipeline_->reset();
        if(gpu_engine_)gpu_engine_->reset();
    }

    void release_engine() noexcept {
        pipeline_.reset();
        engine_.reset();
        gpu_pipeline_.reset();
        gpu_engine_.reset();
    }

    void release_native_activation(bool reset_transport=true) noexcept {
        release_engine();
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
            const ULONGLONG now=GetTickCount64();
            if (state==last_status_state_ && now-last_status_time_<1000) return;
            last_status_time_=now;
            last_status_state_=state;
            wchar_t local[32768]{};
            const DWORD length=GetEnvironmentVariableW(L"LOCALAPPDATA",local,_countof(local));
            if (!length || length>=_countof(local)) return;
            const auto directory=std::filesystem::path(local)/L"NvofPotPlayer";
            std::error_code error;
            std::filesystem::create_directories(directory,error);
            const auto destination=directory/L"status.json";
            const auto temporary=directory/(L"status."+std::to_wstring(GetCurrentProcessId())+L"."+
                std::to_wstring(GetCurrentThreadId())+L".tmp");
            const double input_fps=input_.duration>0 ? double(kUnits)/input_.duration : 0.0;
            const double output_fps=interpolation_requested() ? double(target_.num)/target_.den : input_fps;
            {
                std::ofstream stream(temporary,std::ios::binary|std::ios::trunc);
                stream << "{\"processId\":" << GetCurrentProcessId()
                    << ",\"state\":" << json_string(state)
                    << ",\"transport\":" << json_string(gpu_delivery_verified_ ? "d3d11-gpu" : native_input_committed() ? "native-pending" : "system-memory")
                    << ",\"nativeInputRequested\":" << (native_input_requested_.load(std::memory_order_acquire) ? "true" : "false")
                    << ",\"nativeRendererSupported\":" << (native_renderer_supported_.load(std::memory_order_acquire) ? "true" : "false")
                    << ",\"sampleTransport\":" << json_string(sample_transport_)
                    << ",\"inputSurfaceFormat\":" << json_string(input_surface_format_)
                    << ",\"inputConversion\":" << json_string(input_conversion_)
                    << ",\"inputRateSelected\":" << (input_rate_selected_?"true":"false")
                    << ",\"doubleRate\":" << (nvof::output_multiple(input_.duration,output_limit_)==2?"true":"false")
                    << ",\"outputFpsLimit\":" << output_limit_
                    << ",\"outputMultiple\":" << (interpolation_requested()?nvof::output_multiple(input_.duration,output_limit_):1)
                    << ",\"requestedInterpolationBackend\":\"" << (native_synthesis_?"native-newton-cost":"fruc") << "\""
                    << ",\"nativeSynthesisRevision\":" << (native_synthesis_?11:0)
                    << ",\"gpuMidpointCorrection\":" << (gpu_correction_?"true":"false")
                    << ",\"appearanceProtection\":" << (appearance_protection_?"true":"false")
                    << ",\"appearancePassFrames\":" << appearance_pass_frames_
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
                    << ",\"algorithm\":" << json_string(native_synthesis_?"integer-native-newton-0.3.2-rate.3":midpoint_pass_frames_?"x2-slow-motion-stabilized":appearance_pass_frames_?"x2-appearance-protected":subpixel_pass_frames_?"independent-motion-phases-subpixel":"independent-motion-phases")
                    << ",\"message\":" << json_string(message) << "}\n";
                stream.flush();
                if (!stream) return;
            }
            MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
        } catch (...) { /* Status is optional and cannot break decoding. */ }
    }

    const unsigned instance_id_=next_filter_instance.fetch_add(1);
    std::atomic<unsigned> topology_logs_{0};
    DiagnosticServiceProbe service_probe_;
    bool probe_services_=false;
    std::filesystem::path directory_;
    Layout input_, output_;
    bool native_feature_enabled_=false;
    bool native_gpu_active_=false;
    bool gpu_delivery_verified_=false;
    bool native_pending_=false;
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
    std::string sample_transport_ = "unknown";
    std::string input_surface_format_="unknown",input_conversion_="none";
    bool enabled_ = true;
    int output_limit_=60;
    unsigned input_rate_mask_=nvof::kAllInputRates;
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
    nvof::Rate target_;
    std::atomic<bool> cancelled_{false};
    bool failed_ = false;
    bool preroll_ = false;
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

STDMETHODIMP ProbeInputPin::GetService(REFGUID service,REFIID requested,LPVOID* value) {
    return owner_->get_native_service("input pin",service,requested,value);
}
STDMETHODIMP ProbeInputPin::GetAllocator(IMemAllocator** allocator) {
    if(!allocator)return E_POINTER;
    if(owner_->native_input_committed()) {
        *allocator=nullptr;
        return E_NOTIMPL; // A native decoder must provide its own GPU allocator.
    }
    return CTransformInputPin::GetAllocator(allocator);
}
STDMETHODIMP ProbeInputPin::ActivateD3D11Decoding(ID3D11Device* device,ID3D11DeviceContext* context,HANDLE mutex,UINT flags) {
    return owner_->activate_native(device,context,mutex,flags);
}
STDMETHODIMP_(UINT) ProbeInputPin::GetD3D11AdapterIndex() {return owner_->native_adapter();}

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

const AMOVIESETUP_MEDIATYPE pin_types[] = {{&MEDIATYPE_Video, &MEDIASUBTYPE_NV12}};
const AMOVIESETUP_PIN pins[] = {
    {L"Input", FALSE, FALSE, FALSE, FALSE, &CLSID_NULL, nullptr, 1, pin_types},
    {L"Output", FALSE, TRUE, FALSE, FALSE, &CLSID_NULL, nullptr, 1, pin_types},
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
