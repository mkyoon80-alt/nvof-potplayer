#include "nvof/gpu_engine.hpp"
#include "nvof/fractional_refiner.hpp"
#include "nvof/motion_synthesizer.hpp"
#include <d3d11_4.h>
#include "nvof/scene_cut.hpp"
#include <vector>
#include <algorithm>
#include <cmath>
#include <d3d10_1.h>
#include "blend_shaders.hpp"
#include "p010_shaders.hpp"
#include "scene_shaders.hpp"
#include <cuda.h>
#include <cudaD3D11.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace nvof {
namespace {
using Microsoft::WRL::ComPtr;
struct WrapperParams { void* frameIn; int64_t timestampIn; void* frameOut; int64_t timestampOut; };
using CreateFn=int(__stdcall*)(void**); using LoadFn=int(__stdcall*)(void*); using DeleteFn=void(__stdcall*)(void*);
using InitFn=int(__stdcall*)(void*,int,int,bool); using RegisterFn=int(__stdcall*)(void*,void*,void*,void*); using ProcessFn=int(__stdcall*)(void*,WrapperParams*);
struct ProcessResult {uint32_t size=16,version=1,flags=0,reserved=0;};
using ProcessExFn=int(__stdcall*)(void*,WrapperParams*,ProcessResult*);
void hr_check(HRESULT hr,const char* action) { if(FAILED(hr)) throw std::runtime_error(std::string(action)+" failed, HRESULT="+std::to_string(static_cast<unsigned long>(hr))); }
void cu_check(CUresult result,const char* action) { if(result==CUDA_SUCCESS)return;const char* name=nullptr;cuGetErrorName(result,&name);throw std::runtime_error(std::string(action)+": "+(name?name:"CUDA error")); }
void wrapper_check(int status,const char* action) {if(status)throw std::runtime_error(std::string(action)+" failed, NVEnc status="+std::to_string(status));}
HMODULE load_module(const std::filesystem::path& path){auto module=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);if(!module)throw std::runtime_error("Cannot load "+path.filename().string()+", Windows error="+std::to_string(GetLastError()));return module;}
template<class T>T symbol(HMODULE module,const char* name){auto result=reinterpret_cast<T>(GetProcAddress(module,name));if(!result)throw std::runtime_error(std::string("Missing export ")+name);return result;}
struct CudaScope { explicit CudaScope(CUcontext c){cu_check(cuCtxPushCurrent(c),"cuCtxPushCurrent");}~CudaScope(){CUcontext c=nullptr;cuCtxPopCurrent(&c);} };
struct MappedPlanes {
    CUgraphicsResource* resources;bool mapped=false;
    explicit MappedPlanes(CUgraphicsResource* r):resources(r){cu_check(cuGraphicsMapResources(2,resources,0),"Map CUDA Y/UV planes");mapped=true;}
    ~MappedPlanes(){if(mapped)cuGraphicsUnmapResources(2,resources,0);}
    void close(const char* action){cu_check(cuGraphicsUnmapResources(2,resources,0),action);mapped=false;}
    CUarray array(unsigned plane){CUarray result=nullptr;cu_check(cuGraphicsSubResourceGetMappedArray(&result,resources[plane],0,0),"Get CUDA Y/UV array");return result;}
};
uint32_t blend_weight(int64_t elapsed,int64_t interval){
    // Exact floor(elapsed * 65536 / interval), without a large timestamp product.
    uint64_t remainder=static_cast<uint64_t>(elapsed),denominator=static_cast<uint64_t>(interval);uint32_t result=0;
    for(int bit=0;bit<16;++bit){result<<=1;if(remainder>=denominator-remainder){remainder-=denominator-remainder;result|=1;}else remainder*=2;}
    return result;
}

// P010 stores its 10-bit code value in the upper bits of each 16-bit word.
// Divide code values by four, not by 1023: this preserves video black/white
// and neutral chroma (64/940/512 -> 16/235/128). No RGB/range conversion.


}

struct GpuFrucEngine::Impl {
    std::mutex mutex;
    GpuCompletionMode completion_mode=GpuCompletionMode::blocking;
    // P010 decoder interoperability uses explicit completion boundaries until
    // queued conversion is validated with the real host decoder and renderer.
    std::atomic<bool> p010_completion{false};
    bool skip_identical_warp=true;
    bool stabilize_midpoint=true;
    bool protect_appearance=true;
    HANDLE decoder_mutex=nullptr; // Borrowed, never closed here.
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext1> context1;
    ComPtr<ID3D10Multithread> multithread;
    ComPtr<ID3DDeviceContextState> private_state;
    ComPtr<ID3D11Query> completion;
    ComPtr<ID3D11DeviceContext4> context4;
    ComPtr<ID3D11Fence> completion_fence;
    HANDLE completion_event=nullptr;uint64_t fence_value=0;
    ComPtr<ID3D11ComputeShader> scene_compute;
    ComPtr<ID3D11Buffer> scene_buffer,scene_readback;
    ComPtr<ID3D11UnorderedAccessView> scene_uav;
    struct OutputSlot {ComPtr<ID3D11Texture2D> texture;std::array<ComPtr<ID3D11ShaderResourceView>,2> views;std::array<ComPtr<ID3D11RenderTargetView>,2> targets;int width=0,height=0;};
    std::vector<std::shared_ptr<OutputSlot>> output_pool;
    ComPtr<ID3D11VertexShader> vertex_shader;
    ComPtr<ID3D11PixelShader> pixel_shader,p010_pixel_shader;
    ComPtr<ID3D11Texture2D> p010_capture;
    std::array<ComPtr<ID3D11ShaderResourceView>,2> p010_views;
    int p010_width=0,p010_height=0;
    ComPtr<ID3D11Buffer> weight_buffer;
    ComPtr<ID3D11RasterizerState> rasterizer;
    // Y and UV are point-copied without an RGB conversion or chroma resampling.
    // Slots 0/1 upload to packed CUDA NV12; slots 2/3 download the FRUC result.
    std::array<ComPtr<ID3D11Texture2D>,4> planes;
    std::array<ComPtr<ID3D11ShaderResourceView>,4> plane_views;
    std::array<ComPtr<ID3D11RenderTargetView>,2> plane_targets;
    std::array<CUgraphicsResource,4> interop{};
    std::array<CUdeviceptr,3> buffers{};
    std::unique_ptr<std::array<CUdeviceptr,3>> resources;
    CUcontext cuda_context=nullptr;
    std::string name;
    int width=0,height=0,cached_buffer=0;
    HMODULE nvidia_module=nullptr,wrapper_module=nullptr;
    struct Session {void* wrapper=nullptr;};
    std::vector<Session> sessions;
    std::unique_ptr<FractionalRefiner> fractional_refiner;
    std::unique_ptr<MotionSynthesizer> native_synthesizer;
    bool fractional_unavailable=false;
    CreateFn create=nullptr;LoadFn load=nullptr;DeleteFn destroy=nullptr;InitFn init=nullptr;RegisterFn register_resources=nullptr;ProcessFn process=nullptr;ProcessExFn process_ex=nullptr,advance=nullptr;
    bool primed=false;
    int64_t clock=0,cached_pts=0;
    ComPtr<ID3D11Texture2D> cached_texture;
    std::shared_ptr<void> cached_lease;
    UINT cached_slice=0;

    struct DeviceScope {
        Impl& owner;bool owns_mutex=false,owns_multithread=false;ComPtr<ID3DDeviceContextState> old_state;
        explicit DeviceScope(Impl& o):owner(o){
            if(owner.decoder_mutex){const DWORD wait=WaitForSingleObject(owner.decoder_mutex,10000);if(wait!=WAIT_OBJECT_0&&wait!=WAIT_ABANDONED)throw std::runtime_error("Timed out acquiring decoder D3D11 mutex");owns_mutex=true;}
            // MF decoders know the device manager, not our renderer mutex.
            // Hold the protected D3D context for the complete save/work/restore
            // sequence; per-call protection alone permits state interleaving.
            if(owner.multithread && owner.multithread->GetMultithreadProtected()) {
                owner.multithread->Enter();owns_multithread=true;
            }
            owner.context1->SwapDeviceContextState(owner.private_state.Get(),&old_state);
        }
        ~DeviceScope(){owner.context1->SwapDeviceContextState(old_state.Get(),nullptr);if(owns_multithread)owner.multithread->Leave();if(owns_mutex)ReleaseMutex(owner.decoder_mutex);}
    };

    Impl(const std::filesystem::path& directory,ID3D11Device* d,ID3D11DeviceContext* c,HANDLE shared_mutex,GpuCompletionMode mode,bool skip,bool stabilize,bool protect,GpuInterpolationBackend backend,unsigned flow_dimension):completion_mode(mode),skip_identical_warp(skip),stabilize_midpoint(stabilize),protect_appearance(protect),decoder_mutex(shared_mutex),device(d),context(c){
        try{
            if(!d||!c)throw std::invalid_argument("A decoder D3D11 device and immediate context are required");
            if(c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)throw std::invalid_argument("D3D11 immediate context required");
            ComPtr<ID3D11Device> context_device;c->GetDevice(&context_device);if(context_device.Get()!=d)throw std::invalid_argument("D3D11 context belongs to another device");
            hr_check(context.As(&context1),"Query ID3D11DeviceContext1");
            context.As(&multithread); // Optional for existing non-MF callers.
            ComPtr<ID3D11Device1> device1;hr_check(device.As(&device1),"Query ID3D11Device1");
            const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};D3D_FEATURE_LEVEL actual{};
            hr_check(device1->CreateDeviceContextState(0,levels,2,D3D11_SDK_VERSION,__uuidof(ID3D11Device),&actual,&private_state),"CreateDeviceContextState");
            D3D11_QUERY_DESC query_desc{D3D11_QUERY_EVENT,0};hr_check(device->CreateQuery(&query_desc,&completion),"Create GPU completion query");
            ComPtr<ID3D11Device5> device5;
            if(SUCCEEDED(device.As(&device5))&&SUCCEEDED(context.As(&context4))&&SUCCEEDED(device5->CreateFence(0,D3D11_FENCE_FLAG_NONE,IID_PPV_ARGS(&completion_fence)))){
                completion_event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!completion_event)completion_fence.Reset();
            }
            hr_check(device->CreateComputeShader(shaders::scene::main,sizeof(shaders::scene::main),nullptr,&scene_compute),"Create scene detector");
            D3D11_BUFFER_DESC stats_desc{};stats_desc.ByteWidth=sizeof(SceneStats)+sizeof(uint32_t);stats_desc.Usage=D3D11_USAGE_DEFAULT;stats_desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;stats_desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;stats_desc.StructureByteStride=4;
            hr_check(device->CreateBuffer(&stats_desc,nullptr,&scene_buffer),"Create scene statistics");
            D3D11_UNORDERED_ACCESS_VIEW_DESC stats_view{};stats_view.Format=DXGI_FORMAT_UNKNOWN;stats_view.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;stats_view.Buffer.NumElements=70;
            hr_check(device->CreateUnorderedAccessView(scene_buffer.Get(),&stats_view,&scene_uav),"Create scene statistics view");
            stats_desc.Usage=D3D11_USAGE_STAGING;stats_desc.BindFlags=0;stats_desc.MiscFlags=0;stats_desc.StructureByteStride=0;stats_desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            hr_check(device->CreateBuffer(&stats_desc,nullptr,&scene_readback),"Create scene statistics readback");
            hr_check(device->CreateVertexShader(shaders::blend::vs,sizeof(shaders::blend::vs),nullptr,&vertex_shader),"CreateVertexShader");
            hr_check(device->CreatePixelShader(shaders::blend::ps,sizeof(shaders::blend::ps),nullptr,&pixel_shader),"CreatePixelShader");
            D3D11_BUFFER_DESC cb{};cb.ByteWidth=16;cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            hr_check(device->CreateBuffer(&cb,nullptr,&weight_buffer),"Create GPU blend constants");
            D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;
            hr_check(device->CreateRasterizerState(&raster,&rasterizer),"CreateRasterizerState");
            if(backend==GpuInterpolationBackend::native_experimental) {
                DeviceScope scope(*this);
                native_synthesizer=std::make_unique<MotionSynthesizer>(device.Get(),context.Get(),flow_dimension);
                ComPtr<IDXGIDevice> dxgi;hr_check(device.As(&dxgi),"Native DXGI device");
                ComPtr<IDXGIAdapter> adapter;hr_check(dxgi->GetAdapter(&adapter),"Native adapter");
                DXGI_ADAPTER_DESC desc{};hr_check(adapter->GetDesc(&desc),"Native adapter name");
                const int bytes=WideCharToMultiByte(CP_UTF8,0,desc.Description,-1,nullptr,0,nullptr,nullptr);
                if(bytes<=0)throw std::runtime_error("Native adapter name conversion failed");
                name.resize(bytes);WideCharToMultiByte(CP_UTF8,0,desc.Description,-1,name.data(),bytes,nullptr,nullptr);
                name.pop_back();
                return;
            }
            const auto runtime=std::filesystem::absolute(directory);nvidia_module=load_module(runtime/L"NvOFFRUC.dll");wrapper_module=load_module(runtime/L"NvofFrucBridge.dll");
            create=symbol<CreateFn>(wrapper_module,"NVEncNVOFFRUCCreate");load=symbol<LoadFn>(wrapper_module,"NVEncNVOFFRUCLoad");destroy=symbol<DeleteFn>(wrapper_module,"NVEncNVOFFRUCDelete");
            init=symbol<InitFn>(wrapper_module,"NVEncNVOFFRUCCreateFURCHandle");register_resources=symbol<RegisterFn>(wrapper_module,"NVEncNVOFFRUCRegisterResource");process=symbol<ProcessFn>(wrapper_module,"NVEncNVOFFRUCProc");process_ex=symbol<ProcessExFn>(wrapper_module,"NVEncNVOFFRUCProcEx");advance=symbol<ProcessExFn>(wrapper_module,"NVEncNVOFFRUCAdvance");
            // Adapter discovery may clear the D3D immediate context internally.
            // Preserve the caller state and exclude concurrent renderer work.
            DeviceScope discovery_scope(*this);
            cu_check(cuInit(0),"cuInit");CUdevice gpu=0;unsigned count=0;cu_check(cuD3D11GetDevices(&count,&gpu,1,device.Get(),CU_D3D11_DEVICE_LIST_ALL),"cuD3D11GetDevices");if(count!=1)throw std::runtime_error("Exactly one CUDA GPU must back the decoder D3D11 device");
            char label[256]{};cu_check(cuDeviceGetName(label,sizeof(label),gpu),"cuDeviceGetName");name=label;
            cu_check(cuCtxCreate(&cuda_context,CU_CTX_SCHED_BLOCKING_SYNC,gpu),"cuCtxCreate");CUcontext popped=nullptr;cu_check(cuCtxPopCurrent(&popped),"cuCtxPopCurrent after create");
        }catch(...){cleanup();throw;}
    }
    ~Impl(){cleanup();}
    void close_session() noexcept {
        if(cuda_context)cuCtxSynchronize();
        for(auto& session:sessions)if(session.wrapper&&destroy)destroy(std::exchange(session.wrapper,nullptr));
        sessions.clear();primed=false;clock=0;cached_texture.Reset();cached_lease.reset();
    }
    void clear_surfaces() noexcept {
        fractional_refiner.reset();
        for(auto&r:interop)if(r){cuGraphicsUnregisterResource(r);r=nullptr;}
        for(auto&p:buffers)if(p){cuMemFree(p);p=0;}
        output_pool.clear();for(auto& target:plane_targets)target.Reset();for(auto& view:plane_views)view.Reset();for(auto& texture:planes)texture.Reset();width=height=0;
    }
    void cleanup() noexcept {
        if(native_synthesizer) {
            try {DeviceScope scope(*this);wait_gpu();native_synthesizer.reset();output_pool.clear();}
            catch(...) {
                native_synthesizer.release(); // Same host-lock safety rule as FRUC teardown.
                OutputDebugStringA("NVOF: shared D3D11 lock unavailable during native teardown\n");
                return;
            }
        }
        if(cuda_context){
            try{
                // Unregister and CUDA context destruction can also reset the
                // shared immediate context. Restore renderer state before unlock.
                DeviceScope scope(*this);
                wait_gpu();
                if(cuCtxPushCurrent(cuda_context)==CUDA_SUCCESS){close_session();clear_surfaces();CUcontext popped=nullptr;cuCtxPopCurrent(&popped);}
                cuCtxDestroy(cuda_context);cuda_context=nullptr;
            }catch(...){
                // On an invalid/timed-out host lock, retain driver registrations
                // and DLL references rather than race the host during teardown.
                fractional_refiner.release(); // Do not destroy driver objects without the shared lock.
                OutputDebugStringA("NVOF: shared D3D11 lock unavailable during CUDA teardown\n");
                return;
            }
        }
        if(completion_event){CloseHandle(completion_event);completion_event=nullptr;}
        if(wrapper_module){FreeLibrary(wrapper_module);wrapper_module=nullptr;}if(nvidia_module){FreeLibrary(nvidia_module);nvidia_module=nullptr;}
    }
    void validate(const GpuFrame& f,bool allow_p010=false) const {
        if(!f.texture||f.width<2||f.height<2||f.width>8192||f.height>8192||(f.width&1)||(f.height&1))throw std::invalid_argument("GPU frame must be even-size NV12");
        D3D11_TEXTURE2D_DESC desc{};f.texture->GetDesc(&desc);
        if((desc.Format!=DXGI_FORMAT_NV12&&!(allow_p010&&desc.Format==DXGI_FORMAT_P010))||desc.Width<UINT(f.width)||desc.Height<UINT(f.height)||desc.ArraySize<=f.array_slice||desc.SampleDesc.Count!=1||desc.MipLevels!=1)throw std::invalid_argument("GPU NV12 texture format, dimensions or array slice invalid");
        ComPtr<ID3D11Device> source_device;f.texture->GetDevice(&source_device);if(source_device.Get()!=device.Get())throw std::invalid_argument("GPU input texture belongs to another D3D11 device");
    }
    void wait_gpu(){
        if(completion_fence&&completion_event){
            const auto value=++fence_value;hr_check(context4->Signal(completion_fence.Get(),value),"Signal D3D11 completion fence");context->Flush();
            if(completion_fence->GetCompletedValue()<value){hr_check(completion_fence->SetEventOnCompletion(value,completion_event),"Set D3D11 completion event");
                if(WaitForSingleObject(completion_event,10000)!=WAIT_OBJECT_0)throw std::runtime_error("D3D11 completion fence timeout");}
            hr_check(device->GetDeviceRemovedReason(),"D3D11 completion device state");return;
        }
        context->End(completion.Get());context->Flush();const auto begin=GetTickCount64();BOOL ready=FALSE;
        for(;;){const HRESULT hr=context->GetData(completion.Get(),&ready,sizeof(ready),D3D11_ASYNC_GETDATA_DONOTFLUSH);hr_check(hr,"D3D11 GPU completion");if(hr==S_OK&&ready)return;if(GetTickCount64()-begin>10000)throw std::runtime_error("D3D11 GPU completion timeout");SwitchToThread();}
    }
    ComPtr<ID3D11Texture2D> make_texture(DXGI_FORMAT format,int w,int h){
        D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.MipLevels=1;desc.ArraySize=1;desc.Format=format;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> result;hr_check(device->CreateTexture2D(&desc,nullptr,&result),"Create owned GPU texture");return result;
    }
    ComPtr<ID3D11ShaderResourceView> source_view(ID3D11Texture2D* texture,DXGI_FORMAT format){
        for(auto& slot:output_pool)if(slot->texture.Get()==texture)return slot->views[format==DXGI_FORMAT_R8_UNORM?0:1];
        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};desc.Format=format;desc.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;desc.Texture2D.MipLevels=1;
        ComPtr<ID3D11ShaderResourceView> view;hr_check(device->CreateShaderResourceView(texture,&desc,&view),"Create native Y/UV shader view");return view;
    }
    ComPtr<ID3D11RenderTargetView> target_view(ID3D11Texture2D* texture,DXGI_FORMAT format){
        for(auto& slot:output_pool)if(slot->texture.Get()==texture)return slot->targets[format==DXGI_FORMAT_R8_UNORM?0:1];
        D3D11_RENDER_TARGET_VIEW_DESC desc{};desc.Format=format;desc.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11RenderTargetView> view;hr_check(device->CreateRenderTargetView(texture,&desc,&view),"Create native Y/UV target view");return view;
    }
    GpuFrame make_output(int w,int h,int64_t pts){
        for(auto& slot:output_pool)if(slot.use_count()==1&&slot->width==w&&slot->height==h)return {slot->texture,0,w,h,pts,slot};
        auto slot=std::make_shared<OutputSlot>();slot->width=w;slot->height=h;slot->texture=make_texture(DXGI_FORMAT_NV12,w,h);
        for(unsigned p=0;p<2;++p){auto format=p?DXGI_FORMAT_R8G8_UNORM:DXGI_FORMAT_R8_UNORM;slot->views[p]=source_view(slot->texture.Get(),format);slot->targets[p]=target_view(slot->texture.Get(),format);}
        if(output_pool.size()<64)output_pool.push_back(slot);
        return {slot->texture,0,w,h,pts,slot};
    }
    struct SceneAnalysis {SceneDecision decision;bool identical=false;};
    SceneAnalysis scene(const GpuFrame& a,const GpuFrame& b){
        DeviceScope scope(*this);auto ta=shader_source_texture(a),tb=shader_source_texture(b);
        auto va=source_view(ta.Get(),DXGI_FORMAT_R8_UNORM),vb=source_view(tb.Get(),DXGI_FORMAT_R8_UNORM);
        auto ua=source_view(ta.Get(),DXGI_FORMAT_R8G8_UNORM),ub=source_view(tb.Get(),DXGI_FORMAT_R8G8_UNORM);
        const UINT zero[4]={};context->ClearUnorderedAccessViewUint(scene_uav.Get(),zero);
        const uint32_t dimensions[4]={uint32_t(a.width),uint32_t(a.height),0,0};context->UpdateSubresource(weight_buffer.Get(),0,nullptr,dimensions,0,0);
        ID3D11ShaderResourceView* views[]={va.Get(),vb.Get(),ua.Get(),ub.Get()};ID3D11UnorderedAccessView* uav=scene_uav.Get();ID3D11Buffer* constants=weight_buffer.Get();
        context->CSSetShader(scene_compute.Get(),nullptr,0);context->CSSetConstantBuffers(0,1,&constants);context->CSSetShaderResources(0,4,views);context->CSSetUnorderedAccessViews(0,1,&uav,nullptr);context->Dispatch(((std::max)(UINT(a.width),64U)+15)/16,((std::max)(UINT(a.height),36U)+15)/16,1);
        ID3D11ShaderResourceView* empty[4]={};uav=nullptr;context->CSSetShaderResources(0,4,empty);context->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
        context->CopyResource(scene_readback.Get(),scene_buffer.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
        hr_check(context->Map(scene_readback.Get(),0,D3D11_MAP_READ,0,&mapped),"Read aggregate scene statistics");
        SceneStats statistics{};memcpy(&statistics,mapped.pData,sizeof(statistics));uint32_t difference=0;memcpy(&difference,static_cast<const uint8_t*>(mapped.pData)+sizeof(statistics),sizeof(difference));context->Unmap(scene_readback.Get(),0);return {classify_scene(statistics),difference==0};
    }
    ComPtr<ID3D11Texture2D> shader_source_texture(const GpuFrame& frame){
        D3D11_TEXTURE2D_DESC desc{};frame.texture->GetDesc(&desc);
        if(desc.ArraySize==1 && desc.Width==UINT(frame.width) && desc.Height==UINT(frame.height) && (desc.BindFlags&D3D11_BIND_SHADER_RESOURCE))return frame.texture;
        // Decoder arrays or decode-only surfaces are first captured on GPU.
        // All draws share the immediate context, so copy ordering is preserved.
        auto result=make_texture(DXGI_FORMAT_NV12,frame.width,frame.height);D3D11_BOX box{0,0,0,UINT(frame.width),UINT(frame.height),1};
        context->CopySubresourceRegion(result.Get(),0,0,0,0,frame.texture.Get(),frame.array_slice,&box);return result;
    }
    void draw_plane(ID3D11RenderTargetView* target,ID3D11ShaderResourceView* a,ID3D11ShaderResourceView* b,int w,int h,uint32_t weight,ID3D11PixelShader* override_shader=nullptr){
        const uint32_t constants[4]={weight,0,0,0};context->UpdateSubresource(weight_buffer.Get(),0,nullptr,constants,0,0);
        ID3D11ShaderResourceView* views[]={a,b};ID3D11Buffer* constant=weight_buffer.Get();
        context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(vertex_shader.Get(),nullptr,0);context->GSSetShader(nullptr,nullptr,0);context->HSSetShader(nullptr,nullptr,0);context->DSSetShader(nullptr,nullptr,0);context->PSSetShader(override_shader?override_shader:pixel_shader.Get(),nullptr,0);
        context->OMSetRenderTargets(1,&target,nullptr);context->OMSetBlendState(nullptr,nullptr,0xffffffff);context->OMSetDepthStencilState(nullptr,0);context->RSSetState(rasterizer.Get());
        D3D11_VIEWPORT viewport{0,0,float(w),float(h),0,1};context->RSSetViewports(1,&viewport);context->PSSetShaderResources(0,2,views);context->PSSetConstantBuffers(0,1,&constant);context->Draw(3,0);
        // D3D11 treats the two NV12 planes as the same resource for hazards.
        ID3D11ShaderResourceView* empty[]={nullptr,nullptr};context->PSSetShaderResources(0,2,empty);context->OMSetRenderTargets(0,nullptr,nullptr);
    }
    void normalize_p010(const GpuFrame& input,const GpuFrame& output) {
        if(!p010_pixel_shader) {
            hr_check(device->CreatePixelShader(shaders::p010::ps,sizeof(shaders::p010::ps),nullptr,&p010_pixel_shader),"Create P010 normalization shader");
        }
        if(!p010_capture || p010_width!=input.width || p010_height!=input.height) {
            D3D11_TEXTURE2D_DESC desc{};desc.Width=input.width;desc.Height=input.height;
            desc.MipLevels=1;desc.ArraySize=1;desc.Format=DXGI_FORMAT_P010;desc.SampleDesc.Count=1;
            desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            ComPtr<ID3D11Texture2D> capture;hr_check(device->CreateTexture2D(&desc,nullptr,&capture),"Create P010 capture texture");
            std::array<ComPtr<ID3D11ShaderResourceView>,2> views;
            views[0]=source_view(capture.Get(),DXGI_FORMAT_R16_UINT);
            views[1]=source_view(capture.Get(),DXGI_FORMAT_R16G16_UINT);
            p010_capture=std::move(capture);p010_views=std::move(views);
            p010_width=input.width;p010_height=input.height;
        }
        // Copy the selected decoder slice while its sample is retained. This
        // removes decoder padding without reading video pixels back to the CPU.
        D3D11_BOX box{0,0,0,UINT(input.width),UINT(input.height),1};
        context->CopySubresourceRegion(p010_capture.Get(),0,0,0,0,input.texture.Get(),input.array_slice,&box);
        for(unsigned plane=0;plane<2;++plane) {
            auto target=target_view(output.texture.Get(),plane?DXGI_FORMAT_R8G8_UNORM:DXGI_FORMAT_R8_UNORM);
            draw_plane(target.Get(),p010_views[plane].Get(),nullptr,plane?input.width/2:input.width,plane?input.height/2:input.height,0,p010_pixel_shader.Get());
        }
    }
    void configure(int w,int h){
        if(width==w&&height==h)return;
        DeviceScope scope(*this);
        close_session();clear_surfaces();
        try{
            for(unsigned i=0;i<planes.size();++i){
                const bool uv=(i&1)!=0;const auto format=uv?DXGI_FORMAT_R8G8_UNORM:DXGI_FORMAT_R8_UNORM;
                planes[i]=make_texture(format,uv?w/2:w,uv?h/2:h);plane_views[i]=source_view(planes[i].Get(),format);
                if(i<2)plane_targets[i]=target_view(planes[i].Get(),format);
                cu_check(cuGraphicsD3D11RegisterResource(&interop[i],planes[i].Get(),CU_GRAPHICS_REGISTER_FLAGS_NONE),"Register native Y/UV plane with CUDA");
            }
            for(auto& buffer:buffers)cu_check(cuMemAlloc(&buffer,size_t(w)*h*3/2),"Allocate packed GPU NV12");
            width=w;height=h;
        }catch(...){close_session();clear_surfaces();throw;}
    }
    void upload_gpu(const GpuFrame& frame,int index){
        // CUDA map/unmap also touch the shared D3D11 immediate context.
        // Keep the renderer/decoder lock until the interop transfer is closed.
        DeviceScope scope(*this);const auto source=shader_source_texture(frame);
        for(unsigned plane=0;plane<2;++plane){
            const auto format=plane?DXGI_FORMAT_R8G8_UNORM:DXGI_FORMAT_R8_UNORM;auto view=source_view(source.Get(),format);
            draw_plane(plane_targets[plane].Get(),view.Get(),view.Get(),plane?width/2:width,plane?height/2:height,0);
        }
        // Map orders preceding D3D work; no CPU completion poll here.
        MappedPlanes mapped(interop.data());
        for(unsigned plane=0;plane<2;++plane){
            CUDA_MEMCPY2D copy{};copy.srcMemoryType=CU_MEMORYTYPE_ARRAY;copy.srcArray=mapped.array(plane);copy.dstMemoryType=CU_MEMORYTYPE_DEVICE;copy.dstDevice=buffers[index]+(plane?size_t(width)*height:0);copy.dstPitch=width;copy.WidthInBytes=width;copy.Height=plane?height/2:height;
            cu_check(cuMemcpy2D(&copy),"GPU Y/UV plane to packed NV12");
        }
        if(p010_completion.load())cu_check(cuCtxSynchronize(),"Complete P010 CUDA input transfer");
        mapped.close("Unmap CUDA input Y/UV planes");
    }
    GpuFrame output_gpu(int64_t pts,float refined_phase=-1.0f,bool midpoint_stable=false){
        DeviceScope scope(*this);
        {MappedPlanes mapped(interop.data()+2);
            for(unsigned plane=0;plane<2;++plane){
                CUDA_MEMCPY2D copy{};copy.srcMemoryType=CU_MEMORYTYPE_DEVICE;copy.srcDevice=buffers[2]+(plane?size_t(width)*height:0);copy.srcPitch=width;copy.dstMemoryType=CU_MEMORYTYPE_ARRAY;copy.dstArray=mapped.array(plane);copy.WidthInBytes=width;copy.Height=plane?height/2:height;
                cu_check(cuMemcpy2D(&copy),"GPU packed NV12 to Y/UV plane");
            }
            if(p010_completion.load())cu_check(cuCtxSynchronize(),"Complete P010 CUDA output transfer");
            mapped.close("Unmap CUDA output Y/UV planes");
        }
        auto result=make_output(width,height,pts);
        if(refined_phase>=0.0f||midpoint_stable){
            auto y=target_view(result.texture.Get(),DXGI_FORMAT_R8_UNORM);
            auto uv=target_view(result.texture.Get(),DXGI_FORMAT_R8G8_UNORM);
            if(midpoint_stable)fractional_refiner->render_midpoint(y.Get(),uv.Get(),plane_views[2].Get(),plane_views[3].Get());
            else fractional_refiner->render(refined_phase,y.Get(),uv.Get(),plane_views[2].Get(),plane_views[3].Get());
        }else for(unsigned plane=0;plane<2;++plane){
            auto target=target_view(result.texture.Get(),plane?DXGI_FORMAT_R8G8_UNORM:DXGI_FORMAT_R8_UNORM);
            draw_plane(target.Get(),plane_views[plane+2].Get(),plane_views[plane+2].Get(),plane?width/2:width,plane?height/2:height,0);
        }
        return result;
    }
    void prime(const GpuFrame& previous){
        close_session();
        if(fractional_refiner)fractional_refiner->invalidate_history();
        // Preserve resource-address identity until unregister; allocate its next
        // generation before releasing the old addresses (SDK re-prime regression).
        auto next_resources=std::make_unique<std::array<CUdeviceptr,3>>(buffers);
        resources.swap(next_resources);
        upload_gpu(previous,0);cached_buffer=0;cached_texture=previous.texture;cached_lease=previous.lease;cached_slice=previous.array_slice;cached_pts=previous.pts;primed=true;
    }
    void ensure_session(size_t index,const GpuFrame& previous){
        while(sessions.size()<=index){
            sessions.push_back({});auto& session=sessions.back();
            wrapper_check(create(&session.wrapper),"Create FRUC phase");if(!session.wrapper)throw std::runtime_error("Null FRUC phase");
            wrapper_check(load(session.wrapper),"Load FRUC phase");wrapper_check(init(session.wrapper,width,height,true),"Initialize FRUC phase");
            wrapper_check(register_resources(session.wrapper,&(*resources)[0],&(*resources)[1],&(*resources)[2]),"Register shared FRUC phase buffers");
            WrapperParams params{&(*resources)[cached_buffer],previous.pts,&(*resources)[2],previous.pts};ProcessResult ignored{};
            wrapper_check(process_ex(session.wrapper,&params,&ignored),"Prime FRUC phase");
            // Explicit startup boundary; steady-state phases do not use a
            // whole-context synchronization.
            cu_check(cuCtxSynchronize(),"Complete new FRUC history priming");
        }
    }

};

GpuFrucEngine::GpuFrucEngine(const std::filesystem::path& runtime,ID3D11Device* device,ID3D11DeviceContext* context,HANDLE decoder_mutex,GpuCompletionMode mode,bool skip,bool stabilize,bool protect,GpuInterpolationBackend backend,unsigned flow_dimension):impl_(std::make_unique<Impl>(runtime,device,context,decoder_mutex,mode,skip,stabilize,protect,backend,flow_dimension)){}
GpuFrucEngine::~GpuFrucEngine()=default;
std::string GpuFrucEngine::device_name()const{return impl_->name;}
bool GpuFrucEngine::queued_completion()const{return impl_->completion_mode==GpuCompletionMode::context_ordered&&!impl_->p010_completion.load();}
void GpuFrucEngine::reset()noexcept{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    try{
        Impl::DeviceScope scope(*impl_);
        if(impl_->native_synthesizer) {
            impl_->native_synthesizer->invalidate_history();
            impl_->cached_texture.Reset();impl_->cached_lease.reset();return;
        }
        if(impl_->fractional_refiner)impl_->fractional_refiner->reset();
        if(cuCtxPushCurrent(impl_->cuda_context)==CUDA_SUCCESS){impl_->close_session();CUcontext previous=nullptr;cuCtxPopCurrent(&previous);}
    }catch(...){OutputDebugStringA("NVOF: shared D3D11 lock unavailable during history reset\n");}
}
GpuFrame GpuFrucEngine::copy(const GpuFrame& input){
    std::lock_guard<std::mutex> lock(impl_->mutex);impl_->validate(input,true);Impl::DeviceScope device(*impl_);
    auto result=impl_->make_output(input.width,input.height,input.pts);
    D3D11_TEXTURE2D_DESC desc{};input.texture->GetDesc(&desc);
    if(desc.Format==DXGI_FORMAT_P010){impl_->p010_completion.store(true);impl_->normalize_p010(input,result);}
    else {
        D3D11_BOX box{0,0,0,UINT(input.width),UINT(input.height),1};
        impl_->context->CopySubresourceRegion(result.texture.Get(),0,0,0,0,input.texture.Get(),input.array_slice,&box);
    }
    if(!queued_completion())impl_->wait_gpu();return result;
}
GpuFrame GpuFrucEngine::midpoint(const GpuFrame& previous,const GpuFrame& current){
    const auto time=previous.pts+(current.pts-previous.pts)/2;auto batch=interpolate_pair(previous,current,{time});
    if(batch.quality.scene_cut||batch.quality.repeated_mask){auto result=previous;result.pts=time;return result;}
    return std::move(batch.frames.at(0));
}
PhaseBatch<GpuFrame> GpuFrucEngine::interpolate_pair(const GpuFrame& previous,const GpuFrame& current,const std::vector<int64_t>& timestamps){
    std::lock_guard<std::mutex> lock(impl_->mutex);impl_->validate(previous);impl_->validate(current);
    if(previous.width!=current.width||previous.height!=current.height||previous.pts<0||current.pts<=previous.pts||current.pts-previous.pts>100000000LL)throw std::invalid_argument("GPU phase interpolation requires matching adjacent frames");
    if(timestamps.size()>32)throw std::invalid_argument("More than 32 motion phases per source pair are unsupported");
    int64_t last=previous.pts;for(auto pts:timestamps){if(pts<=last||pts>=current.pts)throw std::invalid_argument("Motion timestamps must increase inside source pair");last=pts;}
    PhaseBatch<GpuFrame> result{};if(timestamps.empty()&&impl_->sessions.empty())return result;
    if(impl_->native_synthesizer) {
        // A nominal x2 clock can land away from the midpoint when container
        // duration and actual PTS differ, or when input is VFR. Synthesize the
        // requested time; never label a midpoint as a different phase.
        Impl::DeviceScope scope(*impl_);
        try {
            if(impl_->cached_texture.Get()!=previous.texture.Get()||impl_->cached_pts!=previous.pts||
               impl_->cached_slice!=previous.array_slice)impl_->native_synthesizer->invalidate_history();
            const auto analysis=impl_->scene(previous,current);
            if(analysis.decision.cut) {
                impl_->native_synthesizer->invalidate_history();
                result.quality.scene_cut=true;impl_->cached_texture.Reset();impl_->cached_lease.reset();return result;
            }
            if(analysis.identical) {
                for(auto pts:timestamps) {auto exact=previous;exact.pts=pts;result.frames.push_back(std::move(exact));}
                result.quality.identical_warp_skipped=true;impl_->native_synthesizer->invalidate_history();
            }else{
                auto a=impl_->shader_source_texture(previous),b=impl_->shader_source_texture(current);
                impl_->native_synthesizer->prepare(a.Get(),b.Get(),previous.width,previous.height);
                result.quality.repetition_known=true;
                for(size_t i=0;i<timestamps.size();++i) {
                    if(!impl_->native_synthesizer->reliable()) {
                        auto held=previous;held.pts=timestamps[i];result.frames.push_back(std::move(held));
                        result.quality.repeated_mask|=uint32_t(1)<<i;continue;
                    }
                    auto output=impl_->make_output(previous.width,previous.height,timestamps[i]);
                    auto y=impl_->target_view(output.texture.Get(),DXGI_FORMAT_R8_UNORM);
                    auto uv=impl_->target_view(output.texture.Get(),DXGI_FORMAT_R8G8_UNORM);
                    const float fraction=(std::min)(std::nextafter(1.0f,0.0f),float(double(timestamps[i]-previous.pts)/double(current.pts-previous.pts)));
                    impl_->native_synthesizer->render_phase(y.Get(),uv.Get(),fraction);
                    result.frames.push_back(std::move(output));result.quality.native_synthesized_mask|=uint32_t(1)<<i;
                }
            }
            impl_->cached_texture=current.texture;impl_->cached_lease=current.lease;
            impl_->cached_pts=current.pts;impl_->cached_slice=current.array_slice;
            if(!queued_completion())impl_->wait_gpu();
            return result;
        }catch(...){impl_->native_synthesizer->invalidate_history();throw;}
    }
    CudaScope cuda(impl_->cuda_context);
    try{
        impl_->configure(previous.width,previous.height);
        const auto analysis=impl_->scene(previous,current);
        if(analysis.decision.cut){result.quality.scene_cut=true;impl_->close_session();if(impl_->fractional_refiner){Impl::DeviceScope scope(*impl_);impl_->fractional_refiner->reset();}return result;}
        if(!impl_->primed||impl_->cached_texture.Get()!=previous.texture.Get()||impl_->cached_slice!=previous.array_slice||impl_->cached_pts!=previous.pts)impl_->prime(previous);
        const auto phase_count=(std::max)(timestamps.size(),impl_->sessions.size());
        // Equality covers every stored Y/UV byte on the GPU. The cached CUDA
        // resource already contains this exact picture. Advance FRUC once for
        // the new timestamp, preserving temporal state without generating an
        // unused warp or uploading the same pixels again.
        const bool skip=analysis.identical&&impl_->skip_identical_warp;
        const int next=skip?impl_->cached_buffer:1-impl_->cached_buffer;
        if(!skip)impl_->upload_gpu(current,next);
        result.quality.repetition_known=!skip;
        result.quality.identical_warp_skipped=skip;
        const int64_t duration=current.pts-previous.pts;
        const auto fractional=[&](int64_t pts){return std::abs((pts-previous.pts)*2-duration)>1;};
        const bool fractional_requested=NVOF_ENABLE_EXPERIMENTAL_SUBPIXEL&&!analysis.identical&&std::any_of(timestamps.begin(),timestamps.end(),fractional);
        const bool has_midpoint=(impl_->stabilize_midpoint||impl_->protect_appearance)&&!analysis.identical&&
            std::any_of(timestamps.begin(),timestamps.end(),[&](int64_t pts){return !fractional(pts);});
        // RTX 5090 measurements: ~10 ms/pair at 1080p, ~34 ms at 2160p.
        // Keep headroom for the renderer/VSR; high-rate large frames retain
        // regular FRUC rather than introducing a processing backlog.
        const bool within_midpoint_budget=int64_t(previous.width)*previous.height<=1920LL*1080 || duration>=333333;
        result.quality.midpoint_budget_limited=has_midpoint&&!within_midpoint_budget;
        const bool midpoint_requested=has_midpoint&&within_midpoint_budget;
        bool prepared=false;
        // Prepare only after FRUC accepts a generated phase. Cuts, repeated
        // pictures, and identical pairs must never enter the extra warp pass.
        const auto prepare_refinement=[&](){
            if(prepared)return true;
            if(impl_->fractional_unavailable)return false;
            Impl::DeviceScope scope(*impl_);
            try{
                if(!impl_->fractional_refiner)impl_->fractional_refiner=std::make_unique<FractionalRefiner>(impl_->device.Get(),impl_->context.Get(),impl_->protect_appearance,impl_->stabilize_midpoint);
                auto a=impl_->shader_source_texture(previous),b=impl_->shader_source_texture(current);
                impl_->fractional_refiner->prepare(a.Get(),b.Get(),previous.width,previous.height);
                prepared=true;
            }catch(const std::exception& error){
                // Raw flow is optional; keep the working FRUC path if this
                // driver/device cannot provide it. Retry with a new engine.
                impl_->fractional_refiner.reset();impl_->fractional_unavailable=true;
                const std::string warning=std::string("NVOF: motion refinement unavailable: ")+error.what()+"\n";
                OutputDebugStringA(warning.c_str());
            }
            return prepared;
        };
        // Each independent history receives each original once. Unused phases
        // still advance, so variable-rate target clocks cannot leave stale A.
        for(size_t i=0;i<phase_count;++i){
            impl_->ensure_session(i,previous);
            const int64_t pts=i<timestamps.size()?timestamps[i]:previous.pts+(current.pts-previous.pts)/2;
            WrapperParams params{&(*impl_->resources)[next],current.pts,&(*impl_->resources)[2],pts};ProcessResult quality{};
            wrapper_check((skip?impl_->advance:impl_->process_ex)(impl_->sessions[i].wrapper,&params,&quality),skip?"Advance identical FRUC input":"Generate FRUC motion phase");
            if(!(quality.flags&(skip?4u:1u)))throw std::runtime_error("FRUC result status unavailable");
            if(i<timestamps.size()){
                if(quality.flags&2)result.quality.repeated_mask|=uint32_t(1)<<i;
                // NvOFFRUC CUDA Process is blocking. Interop unmap orders CUDA
                // copies before subsequent D3D draws; no full context sync.
                if(analysis.identical){auto exact=previous;exact.pts=pts;result.frames.push_back(std::move(exact));}
                else{
                    const bool accepted=!(quality.flags&2);
                    const bool refine_phase=fractional_requested&&fractional(pts)&&accepted&&prepare_refinement();
                    const bool stable_phase=midpoint_requested&&!fractional(pts)&&accepted&&prepare_refinement();
                    const float t=refine_phase?float(double(pts-previous.pts)/double(duration)):-1.0f;
                    result.frames.push_back(impl_->output_gpu(pts,t,stable_phase));
                    if(refine_phase)result.quality.subpixel_refined_mask|=uint32_t(1)<<i;
                    if(stable_phase&&impl_->stabilize_midpoint)result.quality.midpoint_stabilized_mask|=uint32_t(1)<<i;
                    if(stable_phase&&impl_->protect_appearance)result.quality.appearance_protected_mask|=uint32_t(1)<<i;
                }
            }
        }
        if(!prepared&&impl_->fractional_refiner)impl_->fractional_refiner->invalidate_history();
        result.quality.subpixel_unavailable=fractional_requested&&impl_->fractional_unavailable;
        result.quality.midpoint_stabilization_unavailable=midpoint_requested&&impl_->fractional_unavailable;
        if(!result.frames.empty()&&!queued_completion()){Impl::DeviceScope scope(*impl_);impl_->wait_gpu();}
        impl_->cached_texture=current.texture;impl_->cached_lease=current.lease;impl_->cached_slice=current.array_slice;impl_->cached_pts=current.pts;impl_->cached_buffer=next;
        return result;
    }catch(...){impl_->close_session();throw;}
}
GpuFrame GpuFrucEngine::blend(const GpuFrame& previous,const GpuFrame& current,int64_t pts){
    if(pts<=previous.pts){auto result=copy(previous);result.pts=pts;return result;}if(pts>=current.pts){auto result=copy(current);result.pts=pts;return result;}
    std::lock_guard<std::mutex> lock(impl_->mutex);impl_->validate(previous);impl_->validate(current);
    if(previous.width!=current.width||previous.height!=current.height||previous.pts<0||current.pts<=previous.pts)throw std::invalid_argument("GPU blend requires matching dimensions and increasing nonnegative timestamps");
    Impl::DeviceScope device(*impl_);
    const auto a=impl_->shader_source_texture(previous),b=impl_->shader_source_texture(current);
    const uint32_t weight=blend_weight(pts-previous.pts,current.pts-previous.pts);
    auto result=impl_->make_output(previous.width,previous.height,pts);
    for(unsigned plane=0;plane<2;++plane){
        const auto format=plane?DXGI_FORMAT_R8G8_UNORM:DXGI_FORMAT_R8_UNORM;
        auto first=impl_->source_view(a.Get(),format),second=impl_->source_view(b.Get(),format);auto target=impl_->target_view(result.texture.Get(),format);
        impl_->draw_plane(target.Get(),first.Get(),second.Get(),plane?previous.width/2:previous.width,plane?previous.height/2:previous.height,weight);
    }
    if(!queued_completion())impl_->wait_gpu();return result;
}
}
