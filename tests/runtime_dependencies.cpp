// Packaging oracle: no COM registration, decoder activation, or player interaction.
// This executable must use /MT so its startup does not preload the VC DLLs
// whose app-local resolution it is intended to verify.
#include <windows.h>
#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace {
std::wstring module_path(HMODULE module) {
    std::vector<wchar_t> value(32768);
    const DWORD count = GetModuleFileNameW(module, value.data(), static_cast<DWORD>(value.size()));
    if (!count || count >= value.size()) throw std::runtime_error("Cannot read loaded module path");
    return std::wstring(value.data(), count);
}
std::wstring resolved_file(const fs::path& path) {
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open expected or loaded module");
    std::vector<wchar_t> value(32768);
    const DWORD count = GetFinalPathNameByHandleW(file, value.data(), static_cast<DWORD>(value.size()), FILE_NAME_NORMALIZED);
    CloseHandle(file);
    if (!count || count >= value.size()) throw std::runtime_error("Cannot resolve module identity path");
    return std::wstring(value.data(), count);
}
bool same_file_location(const fs::path& left, const fs::path& right) {
    const auto a = resolved_file(left), b = resolved_file(right);
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
        b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}
class Modules final {
    std::vector<HMODULE> values_;
public:
    ~Modules() { for (auto i = values_.rbegin(); i != values_.rend(); ++i) FreeLibrary(*i); }
    HMODULE load(const fs::path& path) {
        const auto module = LoadLibraryExW(path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) {
            std::wcerr << L"LOAD_FAILED " << path.native() << L" WindowsError=" << GetLastError() << L'\n';
            throw std::runtime_error("Packaged module could not be loaded");
        }
        values_.push_back(module);
        if (!same_file_location(module_path(module), path))
            throw std::runtime_error("Explicit module load resolved outside its requested package location");
        return module;
    }
};
}

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Usage: runtime_dependencies.exe ABSOLUTE_PACKAGE_DIRECTORY");
        const fs::path package(argv[1]);
        if (!package.is_absolute()) throw std::runtime_error("The package directory must be absolute");
        const std::array<const wchar_t*,7> removed={
            L"NvOFFRUC.dll",L"NvofFrucBridge.dll",L"NVEncNVOFFRUC.dll",
            L"cudart64_110.dll",L"cudart64_12.dll",L"nvofapi64.dll",L"nvcuda.dll"};
        for(const auto& entry:fs::recursive_directory_iterator(package)){
            for(auto name:removed)if(_wcsicmp(entry.path().filename().c_str(),name)==0)
                throw std::runtime_error("Forbidden FRUC/CUDA/driver file in package");
        }
        Modules modules;
        modules.load(package / L"NvofPotPlayer.ax");
        for(auto name:removed)if(GetModuleHandleW(name))
            throw std::runtime_error("Filter startup loaded a removed runtime or GPU driver");
        std::wcout<<L"PASS native package: no bundled FRUC/CUDA/driver DLL; filter loads without GPU runtime preload.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
