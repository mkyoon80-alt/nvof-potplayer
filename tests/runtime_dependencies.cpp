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
        const fs::path runtime = package / L"runtime";
        // Five pinned vendor runtimes plus the project-built /MT bridge.
        constexpr std::array<const wchar_t*, 6> required = {
            L"NvOFFRUC.dll", L"NvofFrucBridge.dll", L"cudart64_110.dll",
            L"msvcp140.dll", L"vcruntime140.dll", L"vcruntime140_1.dll"
        };
        for (const auto name : required) {
            if (!fs::is_regular_file(runtime / name)) {
                std::wcerr << L"MISSING_PACKAGE_FILE " << name << L'\n';
                throw std::runtime_error("The package is not self-contained");
            }
            if (const auto preloaded = GetModuleHandleW(name))
                std::wcout << L"PRELOADED " << name << L" = " << module_path(preloaded) << L'\n';
        }
        Modules modules;
        modules.load(runtime / L"NvOFFRUC.dll");
        const auto bridge = modules.load(runtime / L"NvofFrucBridge.dll");
        if (!GetProcAddress(bridge, "NVEncNVOFFRUCProcEx"))
            throw std::runtime_error("The bridge lacks versioned repetition metadata");
        const auto filter = modules.load(package / L"NvofPotPlayer.ax");
        bool valid = true;
        for (const auto name : required) {
            const auto module = GetModuleHandleW(name);
            if (!module) {
                std::wcerr << L"NOT_LOADED " << name << L'\n';
                valid = false;
                continue;
            }
            const auto actual = module_path(module);
            std::wcout << L"LOADED " << name << L" = " << actual << L'\n';
            if (!same_file_location(actual, runtime / name)) {
                std::wcerr << L"OUTSIDE_PACKAGE " << name << L'\n';
                valid = false;
            }
        }
        std::wcout << L"LOADED NvofPotPlayer.ax = " << module_path(filter) << L'\n';
        for (const auto name : { L"nvcuda.dll", L"nvofapi64.dll" }) {
            if (const auto driver = GetModuleHandleW(name))
                std::wcout << L"DRIVER " << name << L" = " << module_path(driver) << L'\n';
            else
                std::wcout << L"DRIVER " << name << L" = not loaded (no interpolation requested)\n";
        }
        if (!valid) throw std::runtime_error("A required runtime was missing or loaded from outside the package");
        std::wcout << L"PASS all 6 native runtime DLLs loaded from package/runtime; no installed VC runtime was selected.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
