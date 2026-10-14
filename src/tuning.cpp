#include "nvof/tuning.hpp"
#include <windows.h>
#include <objbase.h>
#include <cmath>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace nvof::tuning {
namespace {
constexpr Values lower = {{
#define NVOF_TUNABLE(name, value, lo, hi, group, unit, description) float(lo),
#include "nvof/tuning_parameters.def"
#undef NVOF_TUNABLE
}};
constexpr Values upper = {{
#define NVOF_TUNABLE(name, value, lo, hi, group, unit, description) float(hi),
#include "nvof/tuning_parameters.def"
#undef NVOF_TUNABLE
}};
constexpr const char* names[] = {
#define NVOF_TUNABLE(name, value, lo, hi, group, unit, description) #name,
#include "nvof/tuning_parameters.def"
#undef NVOF_TUNABLE
};
std::wstring module_profile_path() {
    static const wchar_t anchor=0;
    HMODULE module=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        &anchor,&module))return {};
    wchar_t path[32768]{};
    DWORD length=GetModuleFileNameW(module,path,_countof(path));
    if(!length||length>=_countof(path))return {};
    return (std::filesystem::path(path).parent_path()/L"NvofTuning.ini").wstring();
}
std::wstring environment(const wchar_t* name) {
    DWORD size=GetEnvironmentVariableW(name,nullptr,0);
    if(!size)return {};
    std::wstring text(size,L'\0');
    DWORD written=GetEnvironmentVariableW(name,text.data(),size);
    if(!written||written>=size)throw std::runtime_error("Tuning environment changed");
    text.resize(written);return text;
}
struct Lock {
    HANDLE handle;bool held;
    explicit Lock(HANDLE h):handle(h) {
        DWORD result=WaitForSingleObject(h,0);
        held=result==WAIT_OBJECT_0||result==WAIT_ABANDONED;
    }
    ~Lock(){if(held)ReleaseMutex(handle);}
};
}
uint32_t schema_hash() noexcept {
    uint32_t hash=2166136261u;
    auto add=[&](unsigned char c){hash=(hash^c)*16777619u;};
    for(unsigned i=0;i<count;++i) {
        for(const char* p=names[i];*p;++p)add(static_cast<unsigned char>(*p));
        add(0);
        for(float value:{defaults[i],lower[i],upper[i]}) {
            auto bytes=reinterpret_cast<const unsigned char*>(&value);
            for(unsigned n=0;n<sizeof(float);++n)add(bytes[n]);
        }
    }
    return hash;
}
bool validate(const Values& values) noexcept {
    for(unsigned i=0;i<count;++i)
        if(!std::isfinite(values[i])||values[i]<lower[i]||values[i]>upper[i])return false;
    return values[repair_min_motion]<=values[repair_propagation_min_motion] &&
        values[evidence_fb_low]<values[evidence_fb_high] &&
        values[evidence_residual_low]<values[evidence_residual_high] &&
        values[evidence_photo_low]<values[evidence_photo_high] &&
        values[stationary_error_low]<values[stationary_error_high] &&
        values[stationary_detail_low]<values[stationary_detail_high];
}
Values load_startup_profile(const std::wstring& path) {
    if(path.empty()||GetPrivateProfileIntW(L"Tuning",L"Enabled",0,path.c_str())!=1 ||
       GetPrivateProfileIntW(L"Tuning",L"SchemaHash",0,path.c_str())!=schema_hash())return defaults;
    Values values=defaults;
    for(unsigned i=0;i<count;++i) {
        std::wstring key(names[i],names[i]+std::char_traits<char>::length(names[i]));
        wchar_t text[96]{};
        DWORD length=GetPrivateProfileStringW(L"Tuning",key.c_str(),L"",text,_countof(text),path.c_str());
        if(!length||length>=_countof(text)-1)return defaults;
        std::string number;
        for(DWORD n=0;n<length;++n) {
            if(text[n]>127)return defaults;
            number.push_back(static_cast<char>(text[n]));
        }
        auto parsed=std::from_chars(number.data(),number.data()+number.size(),values[i]);
        if(parsed.ec!=std::errc{}||parsed.ptr!=number.data()+number.size())return defaults;
    }
    return validate(values)?values:defaults;
}
struct Channel::Impl {
    HANDLE mapping=nullptr,mutex=nullptr;
    Shared* shared=nullptr;
    std::string identifier;
    std::filesystem::path manifest;
    ~Impl() {
        if(shared) {
            {Lock lock(mutex);if(lock.held)shared->alive=0;}
            UnmapViewOfFile(shared);
        }
        if(mapping)CloseHandle(mapping);
        if(mutex)CloseHandle(mutex);
        if(!manifest.empty()){std::error_code error;std::filesystem::remove(manifest,error);}
    }
};
Channel::Channel():impl_(std::make_unique<Impl>()) {
    // Read once when this engine is created, never on the frame-processing path.
    values_=load_startup_profile(module_profile_path());
    GUID guid{};
    if(FAILED(CoCreateGuid(&guid)))throw std::runtime_error("Cannot create tuning session ID");
    wchar_t text[40]{};StringFromGUID2(guid,text,40);
    std::wstring suffix=std::to_wstring(GetCurrentProcessId())+L"-"+std::wstring(text+1,36);
    impl_->identifier.assign(suffix.begin(),suffix.end());
    auto name=L"Local\\NvofTuning-"+suffix;
    impl_->mutex=CreateMutexW(nullptr,FALSE,(name+L"-lock").c_str());
    if(!impl_->mutex||GetLastError()==ERROR_ALREADY_EXISTS)throw std::runtime_error("Cannot create tuning mutex");
    impl_->mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(Shared),name.c_str());
    if(!impl_->mapping||GetLastError()==ERROR_ALREADY_EXISTS)throw std::runtime_error("Cannot create tuning mapping");
    impl_->shared=static_cast<Shared*>(MapViewOfFile(impl_->mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(Shared)));
    if(!impl_->shared)throw std::runtime_error("Cannot map tuning state");
    {
        Lock lock(impl_->mutex);
        if(!lock.held)throw std::runtime_error("Cannot initialize tuning state");
        *impl_->shared={magic,version,sizeof(Shared),count,GetCurrentProcessId(),1,1,0,
            0,0,0,0,0,0,0,schema_hash(),values_,values_};
    }
    auto directory=environment(L"NVOF_TUNING_DIR");
    if(directory.empty()) {
        auto local=environment(L"LOCALAPPDATA");
        if(local.empty())throw std::runtime_error("LOCALAPPDATA or NVOF_TUNING_DIR is required");
        directory=(std::filesystem::path(local)/L"NvofPotPlayer"/L"Tuning").wstring();
    }
    if(!std::filesystem::path(directory).is_absolute())throw std::runtime_error("NVOF_TUNING_DIR must be absolute");
    std::filesystem::create_directories(directory);
    impl_->manifest=std::filesystem::path(directory)/(suffix+L".session");
    std::ofstream file(impl_->manifest,std::ios::binary);
    file<<"NvofPotPlayer experimental tuning v1\n";
    file.close();if(!file)throw std::runtime_error("Cannot publish tuning session");
}
Channel::~Channel()=default;
const std::string& Channel::id() const noexcept {return impl_->identifier;}
void Channel::begin_pair() noexcept {
    ++pairs_;
    Lock lock(impl_->mutex);if(!lock.held)return;
    auto& s=*impl_->shared;
    if(s.requested_revision==revision_)return;
    if(!validate(s.requested)) {s.rejected_revision=s.requested_revision;s.error_code=1;return;}
    values_=s.requested;revision_=s.requested_revision;s.error_code=0;
}
void Channel::publish(unsigned width,unsigned height,bool prepared) noexcept {
    Lock lock(impl_->mutex);if(!lock.held)return;
    auto& s=*impl_->shared;
    s.width=width;s.height=height;s.prepared=prepared?1:0;
    s.pair_count=pairs_;s.phase_count=phases_;
    // A failed prepare never acknowledges successful application.
    if(prepared){s.applied=values_;s.applied_revision=revision_;}
}
void Channel::rendered() noexcept {
    ++phases_;
    Lock lock(impl_->mutex);if(!lock.held)return;
    auto& s=*impl_->shared;
    s.applied=values_;s.applied_revision=revision_;s.phase_count=phases_;s.pair_count=pairs_;
}
}
