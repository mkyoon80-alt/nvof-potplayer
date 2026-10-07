#include "nvof/fruc_bridge.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cuda.h>
#include <NvOFFRUC.h>
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool value, const char* what) { if (!value) throw std::runtime_error(what); }
void cucheck(CUresult result, const char* what) {
    if (result == CUDA_SUCCESS) return;
    const char* name = nullptr; cuGetErrorName(result, &name);
    throw std::runtime_error(std::string(what) + ": " + (name ? name : "CUDA error"));
}
void check(int result, const char* what) {
    if (result) throw std::runtime_error(std::string(what) + ": " + std::to_string(result));
}
template<class T> T symbol(HMODULE module, const char* name) {
    auto result = reinterpret_cast<T>(GetProcAddress(module, name));
    if (!result) throw std::runtime_error(std::string("Missing export ") + name);
    return result;
}
struct Module {
    HMODULE module = nullptr;
    explicit Module(const std::filesystem::path& path) {
        module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) throw std::runtime_error("Cannot load " + path.string() + ": " + std::to_string(GetLastError()));
    }
    ~Module() { if (module) FreeLibrary(module); }
};
struct Context {
    CUdevice device = 0;
    CUcontext context = nullptr;
    Context() {
        cucheck(cuInit(0), "cuInit");
        cucheck(cuDeviceGet(&device, 0), "cuDeviceGet");
        char name[256]{}; cucheck(cuDeviceGetName(name, sizeof(name), device), "cuDeviceGetName");
        std::cout << "GPU " << name << '\n';
        cucheck(cuDevicePrimaryCtxRetain(&context, device), "cuDevicePrimaryCtxRetain");
        cucheck(cuCtxPushCurrent(context), "cuCtxPushCurrent");
    }
    ~Context() {
        if (context) { CUcontext popped = nullptr; cuCtxPopCurrent(&popped); cuDevicePrimaryCtxRelease(device); }
    }
};
struct Resources {
    std::array<CUdeviceptr, 3> values{};
    size_t bytes;
    explicit Resources(size_t count) : bytes(count) {
        try { for (auto& value : values) cucheck(cuMemAlloc(&value, bytes), "cuMemAlloc"); }
        catch (...) { for (auto value : values) if (value) cuMemFree(value); throw; }
    }
    ~Resources() { for (auto value : values) if (value) cuMemFree(value); }
    void upload(size_t index, const std::vector<uint8_t>& pixels) {
        cucheck(cuMemcpyHtoD(values[index], pixels.data(), bytes), "cuMemcpyHtoD");
    }
    std::vector<uint8_t> read() {
        std::vector<uint8_t> result(bytes); cucheck(cuMemcpyDtoH(result.data(), values[2], bytes), "cuMemcpyDtoH"); return result;
    }
};
struct Adapter {
    decltype(&NVEncNVOFFRUCCreate) create;
    decltype(&NVEncNVOFFRUCLoad) load;
    decltype(&NVEncNVOFFRUCDelete) destroy;
    decltype(&NVEncNVOFFRUCCreateFURCHandle) init;
    decltype(&NVEncNVOFFRUCRegisterResource) registration;
    decltype(&NVEncNVOFFRUCCloseFURCHandle) close;
    decltype(&NVEncNVOFFRUCProc) process;
    decltype(&NVEncNVOFFRUCProcEx) process_ex;
    decltype(&NVEncNVOFFRUCAdvance) advance;
    void* handle = nullptr;
    explicit Adapter(HMODULE module)
        : create(symbol<decltype(create)>(module, "NVEncNVOFFRUCCreate")),
          load(symbol<decltype(load)>(module, "NVEncNVOFFRUCLoad")),
          destroy(symbol<decltype(destroy)>(module, "NVEncNVOFFRUCDelete")),
          init(symbol<decltype(init)>(module, "NVEncNVOFFRUCCreateFURCHandle")),
          registration(symbol<decltype(registration)>(module, "NVEncNVOFFRUCRegisterResource")),
          close(symbol<decltype(close)>(module, "NVEncNVOFFRUCCloseFURCHandle")),
          process(symbol<decltype(process)>(module, "NVEncNVOFFRUCProc")),
          process_ex(symbol<decltype(process_ex)>(module, "NVEncNVOFFRUCProcEx")),
          advance(symbol<decltype(advance)>(module, "NVEncNVOFFRUCAdvance")) {
        check(create(&handle), "bridge create");
        try { check(load(handle), "bridge load"); } catch (...) { destroy(handle); handle = nullptr; throw; }
    }
    ~Adapter() { destroy(handle); }
    void start(int width, int height, Resources& resources) {
        check(init(handle, width, height, true), "bridge init");
        check(registration(handle, &resources.values[0], &resources.values[1], &resources.values[2]), "bridge register");
    }
};
struct Reference {
    PtrToFuncNvOFFRUCCreate create;
    PtrToFuncNvOFFRUCRegisterResource registration;
    PtrToFuncNvOFFRUCUnregisterResource unregistration;
    PtrToFuncNvOFFRUCProcess process;
    PtrToFuncNvOFFRUCDestroy destroy;
    NvOFFRUCHandle handle = nullptr;
    Resources* resources = nullptr;
    explicit Reference(HMODULE module)
        : create(symbol<decltype(create)>(module, CreateProcName)),
          registration(symbol<decltype(registration)>(module, RegisterResourceProcName)),
          unregistration(symbol<decltype(unregistration)>(module, UnregisterResourceProcName)),
          process(symbol<decltype(process)>(module, ProcessProcName)),
          destroy(symbol<decltype(destroy)>(module, DestroyProcName)) {}
    ~Reference() { close(); }
    void close() {
        if (!handle) return;
        if (resources) {
            NvOFFRUC_UNREGISTER_RESOURCE_PARAM parameters{};
            parameters.uiCount = 3;
            for (size_t i = 0; i < 3; ++i) parameters.pArrResource[i] = &resources->values[i];
            unregistration(handle, &parameters);
        }
        destroy(handle); handle = nullptr; resources = nullptr;
    }
    void start(int width, int height, Resources& input, bool nv12=true) {
        close();
        NvOFFRUC_CREATE_PARAM parameters{};
        parameters.uiWidth = width; parameters.uiHeight = height;
        parameters.eResourceType = CudaResource; parameters.eSurfaceFormat = nv12?NV12Surface:ARGBSurface;
        parameters.eCUDAResourceType = CudaResourceCuDevicePtr;
        check(create(&parameters, &handle), "reference create");
        NvOFFRUC_REGISTER_RESOURCE_PARAM registration_parameters{};
        registration_parameters.uiCount = 3;
        for (size_t i = 0; i < 3; ++i) registration_parameters.pArrResource[i] = &input.values[i];
        check(registration(handle, &registration_parameters), "reference register");
        resources = &input;
    }
    bool run(int index, double input_time, double output_time, bool skip=false) {
        bool repeated = false;
        NvOFFRUC_PROCESS_IN_PARAMS input{};
        NvOFFRUC_PROCESS_OUT_PARAMS output{};
        input.bSkipWarp=skip;
        input.stFrameDataInput.pFrame = &resources->values[index];
        input.stFrameDataInput.nTimeStamp = input_time;
        output.stFrameDataOutput.pFrame = &resources->values[2];
        output.stFrameDataOutput.nTimeStamp = output_time;
        output.stFrameDataOutput.bHasFrameRepetitionOccurred = &repeated;
        check(process(handle, &input, &output), "reference process");
        return repeated;
    }
};
uint32_t hash(uint32_t value) { value ^= value >> 16; value *= 0x7feb352dU; value ^= value >> 15; return value * 0x846ca68bU; }
std::vector<uint8_t> frame(int width, int height, int shift, int mode) {
    std::vector<uint8_t> pixels(size_t(width) * height * 3 / 2, 128);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const int sx = x - shift + 1024, gx = sx / 8, gy = y / 8;
        const double a = double(sx % 8) / 8, b = double(y % 8) / 8;
        const auto cell = [](int xx, int yy) { return 32.0 + hash(uint32_t(xx) * 733U + uint32_t(yy) * 19349663U) % 190U; };
        pixels[size_t(y) * width + x] = mode == 1 ? 16 : mode == 2 ? 235 :
            uint8_t((1-a)*(1-b)*cell(gx,gy) + a*(1-b)*cell(gx+1,gy) + (1-a)*b*cell(gx,gy+1) + a*b*cell(gx+1,gy+1));
    }
    return pixels;
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        require(argc == 2, "Usage: fruc_bridge_smoke <runtime-directory>");
        const auto directory = std::filesystem::absolute(argv[1]);
        Module nvidia(directory / L"NvOFFRUC.dll"), module(directory / L"NvofFrucBridge.dll");
        Context context;
        constexpr int width = 640, height = 360;
        Resources adapter_resources(size_t(width)*height*3/2), reference_resources(size_t(width)*height*3/2);
        Adapter adapter(module.module);
        Reference reference(nvidia.module);

        require(adapter.create(nullptr) != 0, "Null creation pointer accepted");
        NVEncNVOFFRUCResult result;
        result.flags = 0xffffffffU;
        require(adapter.process_ex(nullptr, nullptr, &result) != 0 && result.flags == 0, "Failed process reported valid metadata");
        result.version = 2;
        require(adapter.process_ex(adapter.handle, nullptr, &result) != 0, "Unknown result version accepted");
        result.version = 1; result.size = 8;
        require(adapter.process_ex(adapter.handle, nullptr, &result) != 0, "Undersized result accepted");
        result.size = 16;

        unsigned compared = 0, repeated_count = 0, synthesized_count = 0;
        for (int cycle = 0; cycle < 3; ++cycle) {
            adapter.start(width, height, adapter_resources);
            reference.start(width, height, reference_resources);
            // Exercise signed timestamps and large absolute values. SDK receives
            // the same small relative timestamps in both independent sessions.
            const int64_t origin = cycle == 0 ? 0 : cycle == 1 ? -90000 : (std::numeric_limits<int64_t>::max)() / 2;
            for (int step = 0; step < 9; ++step) {
                const int index = step % 2;
                const int mode = step == 4 || step == 5 ? 1 : step == 6 || step == 7 ? 2 : 0;
                const auto pixels = frame(width, height, step*8, mode);
                adapter_resources.upload(index, pixels); reference_resources.upload(index, pixels);
                const int64_t input_time = int64_t(step)*10000;
                const int64_t output_time = step ? input_time-5000 : 0;
                NVEncNVOFFRUCParams parameters{&adapter_resources.values[index], origin+input_time,
                                             &adapter_resources.values[2], origin+output_time};
                struct ExtendedResult { NVEncNVOFFRUCResult result; uint32_t canary = 0xa5a51234U; } extended;
                extended.result.size = sizeof(extended);
                extended.result.flags = 0xffffffffU; extended.result.reserved = 0xffffffffU;
                check(adapter.process_ex(adapter.handle, &parameters, &extended.result), "bridge process_ex");
                const bool expected = reference.run(index, double(input_time), double(output_time));
                require(extended.canary == 0xa5a51234U, "Bridge overwrote future ABI fields");
                require(extended.result.reserved == 0, "Bridge did not clear reserved result field");
                require(extended.result.flags == (NVOF_FRUC_REPETITION_KNOWN | (expected ? NVOF_FRUC_FRAME_REPEATED : 0)),
                        "ProcEx repetition metadata differs from NVIDIA's direct bool");
                if (step) {
                    require(adapter_resources.read() == reference_resources.read(), "Bridge pixels differ from direct NVIDIA output");
                    expected ? ++repeated_count : ++synthesized_count;
                }
                ++compared;
                std::cout << "cycle=" << cycle << " step=" << step << " flags=" << extended.result.flags << " direct_repeated=" << expected << '\n';
            }
            check(adapter.close(adapter.handle), "bridge close");
            check(adapter.close(adapter.handle), "bridge repeated close");
            reference.close();
        }
        // Same cached input allocation, new identical-picture timestamps,
        // then resumed movement; compare against official bSkipWarp directly.
        {
        // Fresh address identity, as production prime() uses for a new history.
        Resources advanced_a(size_t(width)*height*3/2),advanced_b(size_t(width)*height*3/2);
        adapter.start(width,height,advanced_a);reference.start(width,height,advanced_b);
        int cached=0;
        for(int step=0;step<16;++step){
            const bool skip=step>0&&(step%4)!=0;
            if(!skip){cached=1-cached;auto image=frame(width,height,(step/4)*8,0);advanced_a.upload(cached,image);advanced_b.upload(cached,image);}
            const int64_t time=int64_t(step)*10000,out=step?time-5000:0;
            NVEncNVOFFRUCParams parameters{&advanced_a.values[cached],time,&advanced_a.values[2],out};
            NVEncNVOFFRUCResult actual;
            check((skip?adapter.advance:adapter.process_ex)(adapter.handle,&parameters,&actual),"advance/motion bridge");
            const bool repeated=reference.run(cached,double(time),double(out),skip);
            require(actual.flags==(skip?NVOF_FRUC_WARP_SKIPPED:(NVOF_FRUC_REPETITION_KNOWN|(repeated?NVOF_FRUC_FRAME_REPEATED:0))),"Advance result fabricated repetition metadata");
            if(!skip&&step){auto a=advanced_a.read(),b=advanced_b.read();size_t changed=0;uint64_t squared=0;int maximum=0;
                for(size_t i=0;i<a.size();++i){int d=std::abs(int(a[i])-b[i]);changed+=d!=0;squared+=d*d;maximum=(std::max)(maximum,d);}
                std::cout<<"ADVANCE_ORACLE step="<<step<<" changed="<<changed<<" max="<<maximum<<" mse="<<double(squared)/a.size()<<std::endl;
                require(changed==0,"Post-advance pixels differ from official SDK");
            }
        }
        check(adapter.close(adapter.handle),"advance close");reference.close();
        result.flags=0xffffffffU;require(adapter.advance(nullptr,nullptr,&result)!=0&&result.flags==0,"Invalid advance reported success");
        std::cout<<"PASS 16 official skip-warp/history calls and resumed output pixels\n";
        }
        // The legacy entry point remains callable after repeated session resets.
        adapter.start(width, height, adapter_resources);
        adapter_resources.upload(0, frame(width,height,0,0));
        NVEncNVOFFRUCParams legacy{&adapter_resources.values[0], 0, &adapter_resources.values[2], 0};
        check(adapter.process(adapter.handle, &legacy), "legacy process");
        check(adapter.close(adapter.handle), "legacy close");
        // Destroy an initialized but unregistered SDK handle, a legacy wrapper leak.
        check(adapter.init(adapter.handle, width, height, true), "unregistered bridge init");
        check(adapter.close(adapter.handle), "unregistered bridge close");
        require(synthesized_count > 0, "Oracle never exercised non-repeated motion output");
        require(repeated_count > 0, "Oracle never exercised actual NVIDIA repetition output");
        std::cout << "PASS metadata matched " << compared << " direct SDK calls; non-repeated=" << synthesized_count
                  << " repeated=" << repeated_count << "; output pixels, versioning, legacy ABI, and teardown verified.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n'; return 1;
    }
}
