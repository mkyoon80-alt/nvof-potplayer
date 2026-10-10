#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dshow.h>
#include <wrl/client.h>
#include <filesystem>
#include <iomanip>
#include <fstream>
#include <iostream>
#include <string>

using Microsoft::WRL::ComPtr;
using RegisterFn=HRESULT(__stdcall*)();
using FactoryFn=HRESULT(__stdcall*)(REFCLSID,REFIID,void**);
const CLSID kFilter={0xedeca044,0x78cd,0x40eb,{0x8f,0x37,0x63,0xd9,0x47,0xc5,0x01,0xa0}};
const CLSID kPage={0xde1f386e,0x626b,0x442a,{0x98,0xc5,0x59,0xc7,0x71,0x3d,0x21,0xba}};

namespace {
struct MachineLog {
    std::ofstream file;
    std::streambuf* out = nullptr;
    std::streambuf* err = nullptr;
    explicit MachineLog(bool enabled) {
        if (!enabled) return;
        try {
            wchar_t local[32768]{};
            const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", local, _countof(local));
            if (!count || count >= _countof(local)) return;
            const auto directory = std::filesystem::path(local) / L"NvofPotPlayer";
            std::error_code error;
            std::filesystem::create_directories(directory, error);
            file.open(directory / L"registration.log", std::ios::trunc);
            if (file) { out = std::cout.rdbuf(file.rdbuf()); err = std::cerr.rdbuf(file.rdbuf()); }
        } catch (...) {}
    }
    ~MachineLog() { if(out)std::cout.rdbuf(out); if(err)std::cerr.rdbuf(err); }
};
struct ComScope {
    HRESULT result=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    ~ComScope(){if(SUCCEEDED(result))CoUninitialize();}
};
struct ModuleScope {
    HMODULE value=nullptr;
    ~ModuleScope(){if(value)FreeLibrary(value);}
};
class ClassesRedirect {
public:
    LONG begin(bool machine) {
        LONG result = machine
            ? RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Classes", 0, KEY_READ|KEY_WOW64_64KEY, &key_)
            : RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes", 0, nullptr, 0, KEY_READ|KEY_WRITE|KEY_WOW64_64KEY, nullptr, &key_, nullptr);
        if(result!=ERROR_SUCCESS)return result;
        result=RegOverridePredefKey(HKEY_CLASSES_ROOT,key_);
        active_=result==ERROR_SUCCESS;
        return result;
    }
    LONG restore() {
        if(!active_)return ERROR_SUCCESS;
        const LONG result=RegOverridePredefKey(HKEY_CLASSES_ROOT,nullptr);
        if(result==ERROR_SUCCESS)active_=false;
        return result;
    }
    ~ClassesRedirect(){restore();if(key_)RegCloseKey(key_);}
private:
    HKEY key_=nullptr;
    bool active_=false;
};

bool elevated_administrator() noexcept {
    HANDLE token=nullptr;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return false;
    TOKEN_ELEVATION elevation{};
    DWORD returned=0;
    const bool elevated=GetTokenInformation(token,TokenElevation,&elevation,sizeof(elevation),&returned)
        && elevation.TokenIsElevated!=0;
    CloseHandle(token);
    if(!elevated)return false;
    SID_IDENTIFIER_AUTHORITY authority=SECURITY_NT_AUTHORITY;
    PSID administrators=nullptr;
    if(!AllocateAndInitializeSid(&authority,2,SECURITY_BUILTIN_DOMAIN_RID,
        DOMAIN_ALIAS_RID_ADMINS,0,0,0,0,0,0,&administrators))return false;
    BOOL member=FALSE;
    const BOOL checked=CheckTokenMembership(nullptr,administrators,&member);
    FreeSid(administrators);
    return checked && member;
}

void print_result(const char* operation,HRESULT result) {
    std::cout<<operation<<"=0x"<<std::hex<<std::setw(8)<<std::setfill('0')
             <<static_cast<unsigned long>(result)<<std::dec<<'\n';
}

int verify_registration() {
    // No DLL is loaded by path and no keys are changed. This exercises exactly
    // the effective COM view used by a player with this process's token.
    std::cout<<"ProcessBits="<<sizeof(void*)*8<<" ElevatedAdministrator="
             <<(elevated_administrator()?1:0)<<'\n';
    ComPtr<IBaseFilter> filter;
    HRESULT result=CoCreateInstance(kFilter,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&filter));
    print_result("CoCreateFilter",result);
    if(FAILED(result))return 20;
    ComPtr<ISpecifyPropertyPages> pages;
    result=filter.As(&pages);
    print_result("ISpecifyPropertyPages",result);
    if(FAILED(result))return 21;
    CAUUID ids{};
    result=pages->GetPages(&ids);
    print_result("GetPages",result);
    const bool expected=SUCCEEDED(result) && ids.cElems==1 && ids.pElems && ids.pElems[0]==kPage;
    CoTaskMemFree(ids.pElems);
    if(!expected)return 22;
    ComPtr<IPropertyPage> page;
    result=CoCreateInstance(kPage,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&page));
    print_result("CoCreatePropertyPage",result);
    if(FAILED(result))return 23;
    IUnknown* object=filter.Get();
    result=page->SetObjects(1,&object);
    print_result("PropertyPageSetObjects",result);
    if(FAILED(result))return 24;
    PROPPAGEINFO info{};
    info.cb=sizeof(info);
    result=page->GetPageInfo(&info);
    print_result("PropertyPageGetInfo",result);
    CoTaskMemFree(info.pszTitle);
    CoTaskMemFree(info.pszDocString);
    CoTaskMemFree(info.pszHelpFile);
    page->SetObjects(0,nullptr);
    if(FAILED(result))return 25;
    std::cout<<"Filter and settings page registration verified; no windows opened.\n";
    return 0;
}
}

int wmain(int argc,wchar_t** argv) {
    if(argc!=2) {
        std::wcerr<<L"Usage: NvofRegister --register | --unregister | --register-machine | --unregister-machine | --verify\n";
        return 2;
    }
    const std::wstring mode=argv[1];
    const bool verify=mode==L"--verify";
    const bool machine=mode==L"--register-machine" || mode==L"--unregister-machine";
    const bool install=mode==L"--register" || mode==L"--register-machine";
    MachineLog registration_log(machine);
    if(!verify && !machine && mode!=L"--register" && mode!=L"--unregister")return 2;
    if(sizeof(void*)!=8) {
        std::cerr<<"This helper must run as x64 for the x64 PotPlayer filter.\n";
        return 11;
    }
    // Fail before loading a registration DLL or opening a writable key.
    // Elevation is requested only by the caller's explicit registration action.
    if(machine && !elevated_administrator()) {
        std::cerr<<"Machine registration requires an elevated administrator token. No keys changed.\n";
        return 10;
    }
    ComScope com;
    if(FAILED(com.result)){print_result("CoInitialize",com.result);return 9;}
    if(verify)return verify_registration();
    wchar_t path[32768]{};
    const DWORD length=GetModuleFileNameW(nullptr,path,_countof(path));
    if(!length || length>=_countof(path))return 3;
    const auto filter=std::filesystem::path(path).parent_path()/L"NvofPotPlayer.ax";
    if(!install) {
        // Refuse to unregister a different installation sharing the same CLSIDs.
        // Read the explicit hive, never the merged HKCR view.
        for(const CLSID& id : {kFilter,kPage}) {
            wchar_t clsid[40]{}; StringFromGUID2(id,clsid,40);
            const std::wstring key=std::wstring(L"Software\\Classes\\CLSID\\")+clsid+L"\\InprocServer32";
            wchar_t registered[32768]{}; DWORD bytes=sizeof(registered);
            const LSTATUS status=RegGetValueW(machine?HKEY_LOCAL_MACHINE:HKEY_CURRENT_USER,key.c_str(),nullptr,
                RRF_RT_REG_SZ|RRF_SUBKEY_WOW6464KEY,nullptr,registered,&bytes);
            if(status==ERROR_FILE_NOT_FOUND || status==ERROR_PATH_NOT_FOUND)continue;
            if(status!=ERROR_SUCCESS) { std::cerr<<"Cannot verify registration ownership: "<<status<<'\n'; return 14; }
            if(_wcsicmp(registered,filter.c_str())!=0) {
                std::cerr<<"Registration belongs to another folder; nothing changed.\n"; return 14;
            }
        }
    }
    ModuleScope module;
    module.value=LoadLibraryExW(filter.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!module.value){std::cerr<<"Cannot load adjacent filter. Windows error "<<GetLastError()<<'\n';return 4;}
    const auto run=reinterpret_cast<RegisterFn>(GetProcAddress(module.value,install?"DllRegisterServer":"DllUnregisterServer"));
    const auto factory=reinterpret_cast<FactoryFn>(GetProcAddress(module.value,"DllGetClassObject"));
    if(!run || !factory)return 5;
    // Pin the expected identities. The adjacent filter's fixed g_Templates has
    // only this filter and this property page; its standard registration also
    // adds/removes only this filter's LegacyAmFilterCategory instance.
    for(const CLSID& id : {kFilter,kPage}) {
        ComPtr<IClassFactory> instance;
        const HRESULT checked=factory(id,IID_PPV_ARGS(&instance));
        if(FAILED(checked)){print_result("Expected COM identity missing",checked);return 12;}
    }
    // Bootstrap the system mapper before redirecting this process's HKCR view,
    // as the per-user Classes hive need not contain the mapper's COM class.
    ComPtr<IFilterMapper2> mapper;
    HRESULT result=CoCreateInstance(CLSID_FilterMapper2,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&mapper));
    if(FAILED(result)){print_result("CreateFilterMapper",result);return 9;}
    ClassesRedirect redirect;
    const LONG redirected=redirect.begin(machine);
    if(redirected!=ERROR_SUCCESS){std::cerr<<"Cannot open selected Classes hive: "<<redirected<<'\n';return 6;}
    result=run();
    const LONG restored=redirect.restore();
    if(restored!=ERROR_SUCCESS){std::cerr<<"Cannot restore this process's Classes view: "<<restored<<'\n';return 13;}
    if(FAILED(result)){print_result("Filter registration failed",result);return 8;}
    std::cout<<(install?"Registered":"Unregistered")
             <<(machine?" in the 64-bit machine Classes hive.\n":" for the current user.\n");
    if(install)std::cout<<"Restart PotPlayer before opening the filter settings.\n";
    return 0;
}
