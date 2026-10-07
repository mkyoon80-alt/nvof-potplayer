#include "nvof/gpu_engine.hpp"
#include <d3d11_1.h>
#include <d3d10_1.h>
#include <d3dcompiler.h>
#include <cuda.h>
#include <cudaD3D11.h>
#include <array>
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
    void close(){cu_check(cuGraphicsUnmapResources(2,resources,0),"Unmap CUDA Y/UV planes");mapped=false;}
    CUarray array(unsigned plane){CUarray result=nullptr;cu_check(cuGraphicsSubResourceGetMappedArray(&result,resources[plane],0,0),"Get CUDA Y/UV array");return result;}
};
uint32_t blend_weight(int64_t elapsed,int64_t interval){
    // Exact floor(elapsed * 65536 / interval), without a large timestamp product.
    uint64_t remainder=static_cast<uint64_t>(elapsed),denominator=static_cast<uint64_t>(interval);uint32_t result=0;
    for(int bit=0;bit<16;++bit){result<<=1;if(remainder>=denominator-remainder){remainder-=denominator-remainder;result|=1;}else remainder*=2;}
    return result;
}
const char* shader_source=R"(
Texture2D<float4> previousTexture:register(t0);
Texture2D<float4> currentTexture:register(t1);
cbuffer BlendWeight:register(b0){uint weight;uint3 padding;};
float4 vs(uint vertex:SV_VertexID):SV_Position {
 float2 position=vertex==0?float2(-1,-1):(vertex==1?float2(-1,3):float2(3,-1));
 return float4(position,0,1);
}
float4 ps(float4 position:SV_Position):SV_Target {
 int3 p=int3(int2(position.xy),0);
 uint4 a=uint4(round(previousTexture.Load(p)*255.0f));
 uint4 b=uint4(round(currentTexture.Load(p)*255.0f));
 uint4 value=(a*(65536u-weight)+b*weight+32768u)>>16;
 return float4(value)/255.0f;
}
)";
ComPtr<ID3DBlob> compile_shader(const char* entry,const char* target){ComPtr<ID3DBlob>blob,error;HRESULT hr=D3DCompile(shader_source,strlen(shader_source),"nvof-gpu-blend",nullptr,nullptr,entry,target,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&error);if(FAILED(hr))throw std::runtime_error(std::string("Shader compilation failed: ")+(error?static_cast<const char*>(error->GetBufferPointer()):"unknown"));return blob;}
}

struct GpuFrucEngine::Impl {
    std::mutex mutex;
    HANDLE decoder_mutex=nullptr; // Borrowed, never closed here.
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext1> context1;
    ComPtr<ID3D10Multithread> multithread;
    ComPtr<ID3DDeviceContextState> private_state;
    ComPtr<ID3D11Query> completion;
    ComPtr<ID3D11VertexShader> vertex_shader;
    ComPtr<ID3D11PixelShader> pixel_shader;
    ComPtr<ID3D11Buffer> weight_buffer;
    ComPtr<ID3D11RasterizerState> rasterizer;
    // Y and UV are point-copied without an RGB conversion or chroma resampling.
    // Slots 0/1 upload to packed CUDA NV12; slots 2/3 download the FRUC result.
    std::array<ComPtr<ID3D11Texture2D>,4> planes;
    std::array<ComPtr<ID3D11ShaderResourceView>,4> plane_views;
    std::array<ComPtr<ID3D11RenderTargetView>,2> plane_targets;
    std::array<CUgraphicsResource,4> interop{};
    std::array<CUdeviceptr,3> buffers{};
    CUcontext cuda_context=nullptr;
    std::string name;
    int width=0,height=0,cached_buffer=0;
    HMODULE nvidia_module=nullptr,wrapper_module=nullptr;
    void* wrapper=nullptr;
    CreateFn create=nullptr;LoadFn load=nullptr;DeleteFn destroy=nullptr;InitFn init=nullptr;RegisterFn register_resources=nullptr;ProcessFn process=nullptr;
    bool primed=false;
    int64_t clock=0,cached_pts=0;
    ComPtr<ID3D11Texture2D> cached_texture;
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

    Impl(const std::filesystem::path& directory,ID3D11Device* d,ID3D11DeviceContext* c,HANDLE shared_mutex):decoder_mutex(shared_mutex),device(d),context(c){
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
            const auto vs=compile_shader("vs","vs_4_0"),ps=compile_shader("ps","ps_4_0");
            hr_check(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex_shader),"CreateVertexShader");
            hr_check(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&pixel_shader),"CreatePixelShader");
            D3D11_BUFFER_DESC cb{};cb.ByteWidth=16;cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            hr_check(device->CreateBuffer(&cb,nullptr,&weight_buffer),"Create GPU blend constants");
            D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;
            hr_check(device->CreateRasterizerState(&raster,&rasterizer),"CreateRasterizerState");
            const auto runtime=std::filesystem::absolute(directory);nvidia_module=load_module(runtime/L"NvOFFRUC.dll");wrapper_module=load_module(runtime/L"NVEncNVOFFRUC.dll");
            create=symbol<CreateFn>(wrapper_module,"NVEncNVOFFRUCCreate");load=symbol<LoadFn>(wrapper_module,"NVEncNVOFFRUCLoad");destroy=symbol<DeleteFn>(wrapper_module,"NVEncNVOFFRUCDelete");
            init=symbol<InitFn>(wrapper_module,"NVEncNVOFFRUCCreateFURCHandle");register_resources=symbol<RegisterFn>(wrapper_module,"NVEncNVOFFRUCRegisterResource");process=symbol<ProcessFn>(wrapper_module,"NVEncNVOFFRUCProc");
            cu_check(cuInit(0),"cuInit");CUdevice gpu=0;unsigned count=0;cu_check(cuD3D11GetDevices(&count,&gpu,1,device.Get(),CU_D3D11_DEVICE_LIST_ALL),"cuD3D11GetDevices");if(count!=1)throw std::runtime_error("Exactly one CUDA GPU must back the decoder D3D11 device");
            char label[256]{};cu_check(cuDeviceGetName(label,sizeof(label),gpu),"cuDeviceGetName");name=label;
            cu_check(cuCtxCreate(&cuda_context,CU_CTX_SCHED_BLOCKING_SYNC,gpu),"cuCtxCreate");CUcontext popped=nullptr;cu_check(cuCtxPopCurrent(&popped),"cuCtxPopCurrent after create");
        }catch(...){cleanup();throw;}
    }
    ~Impl(){cleanup();}
    void close_session() noexcept {if(cuda_context)cuCtxSynchronize();if(wrapper&&destroy)destroy(std::exchange(wrapper,nullptr));primed=false;clock=0;cached_texture.Reset();}
    void clear_surfaces() noexcept {
        for(auto&r:interop)if(r){cuGraphicsUnregisterResource(r);r=nullptr;}
        for(auto&p:buffers)if(p){cuMemFree(p);p=0;}
        for(auto& target:plane_targets)target.Reset();for(auto& view:plane_views)view.Reset();for(auto& texture:planes)texture.Reset();width=height=0;
    }
    void cleanup() noexcept {
        if(cuda_context){if(cuCtxPushCurrent(cuda_context)==CUDA_SUCCESS){close_session();clear_surfaces();CUcontext popped=nullptr;cuCtxPopCurrent(&popped);}cuCtxDestroy(cuda_context);cuda_context=nullptr;}
        if(wrapper_module){FreeLibrary(wrapper_module);wrapper_module=nullptr;}if(nvidia_module){FreeLibrary(nvidia_module);nvidia_module=nullptr;}
    }
    void validate(const GpuFrame& f) const {
        if(!f.texture||f.width<2||f.height<2||f.width>8192||f.height>8192||(f.width&1)||(f.height&1))throw std::invalid_argument("GPU frame must be even-size NV12");
        D3D11_TEXTURE2D_DESC desc{};f.texture->GetDesc(&desc);
        if(desc.Format!=DXGI_FORMAT_NV12||desc.Width<UINT(f.width)||desc.Height<UINT(f.height)||desc.ArraySize<=f.array_slice||desc.SampleDesc.Count!=1||desc.MipLevels!=1)throw std::invalid_argument("GPU NV12 texture format, dimensions or array slice invalid");
        ComPtr<ID3D11Device> source_device;f.texture->GetDevice(&source_device);if(source_device.Get()!=device.Get())throw std::invalid_argument("GPU input texture belongs to another D3D11 device");
    }
    void wait_gpu(){
        context->End(completion.Get());context->Flush();const auto begin=GetTickCount64();BOOL ready=FALSE;
        for(;;){const HRESULT hr=context->GetData(completion.Get(),&ready,sizeof(ready),D3D11_ASYNC_GETDATA_DONOTFLUSH);hr_check(hr,"D3D11 GPU completion");if(hr==S_OK&&ready)return;if(GetTickCount64()-begin>10000)throw std::runtime_error("D3D11 GPU completion timeout");SwitchToThread();}
    }
    ComPtr<ID3D11Texture2D> make_texture(DXGI_FORMAT format,int w,int h){
        D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.MipLevels=1;desc.ArraySize=1;desc.Format=format;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> result;hr_check(device->CreateTexture2D(&desc,nullptr,&result),"Create owned GPU texture");return result;
    }
    ComPtr<ID3D11ShaderResourceView> source_view(ID3D11Texture2D* texture,DXGI_FORMAT format){
        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};desc.Format=format;desc.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;desc.Texture2D.MipLevels=1;
        ComPtr<ID3D11ShaderResourceView> view;hr_check(device->CreateShaderResourceView(texture,&desc,&view),"Create native Y/UV shader view");return view;
    }
    ComPtr<ID3D11RenderTargetView> target_view(ID3D11Texture2D* texture,DXGI_FORMAT format){
        D3D11_RENDER_TARGET_VIEW_DESC desc{};desc.Format=format;desc.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11RenderTargetView> view;hr_check(device->CreateRenderTargetView(texture,&desc,&view),"Create native Y/UV target view");return view;
    }
    ComPtr<ID3D11Texture2D> shader_source_texture(const GpuFrame& frame){
        D3D11_TEXTURE2D_DESC desc{};frame.texture->GetDesc(&desc);
        if(desc.ArraySize==1 && (desc.BindFlags&D3D11_BIND_SHADER_RESOURCE))return frame.texture;
        // Decoder arrays or decode-only surfaces are first captured on GPU.
        // All draws share the immediate context, so copy ordering is preserved.
        auto result=make_texture(DXGI_FORMAT_NV12,frame.width,frame.height);D3D11_BOX box{0,0,0,UINT(frame.width),UINT(frame.height),1};
        context->CopySubresourceRegion(result.Get(),0,0,0,0,frame.texture.Get(),frame.array_slice,&box);return result;
    }
    void draw_plane(ID3D11RenderTargetView* target,ID3D11ShaderResourceView* a,ID3D11ShaderResourceView* b,int w,int h,uint32_t weight){
        const uint32_t constants[4]={weight,0,0,0};context->UpdateSubresource(weight_buffer.Get(),0,nullptr,constants,0,0);
        ID3D11ShaderResourceView* views[]={a,b};ID3D11Buffer* constant=weight_buffer.Get();
        context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(vertex_shader.Get(),nullptr,0);context->GSSetShader(nullptr,nullptr,0);context->HSSetShader(nullptr,nullptr,0);context->DSSetShader(nullptr,nullptr,0);context->PSSetShader(pixel_shader.Get(),nullptr,0);
        context->OMSetRenderTargets(1,&target,nullptr);context->OMSetBlendState(nullptr,nullptr,0xffffffff);context->OMSetDepthStencilState(nullptr,0);context->RSSetState(rasterizer.Get());
        D3D11_VIEWPORT viewport{0,0,float(w),float(h),0,1};context->RSSetViewports(1,&viewport);context->PSSetShaderResources(0,2,views);context->PSSetConstantBuffers(0,1,&constant);context->Draw(3,0);
        // D3D11 treats the two NV12 planes as the same resource for hazards.
        ID3D11ShaderResourceView* empty[]={nullptr,nullptr};context->PSSetShaderResources(0,2,empty);context->OMSetRenderTargets(0,nullptr,nullptr);
    }
    void configure(int w,int h){
        if(width==w&&height==h)return;
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
        const auto source=shader_source_texture(frame);
        for(unsigned plane=0;plane<2;++plane){
            const auto format=plane?DXGI_FORMAT_R8G8_UNORM:DXGI_FORMAT_R8_UNORM;auto view=source_view(source.Get(),format);
            draw_plane(plane_targets[plane].Get(),view.Get(),view.Get(),plane?width/2:width,plane?height/2:height,0);
        }
        wait_gpu();MappedPlanes mapped(interop.data());
        for(unsigned plane=0;plane<2;++plane){
            CUDA_MEMCPY2D copy{};copy.srcMemoryType=CU_MEMORYTYPE_ARRAY;copy.srcArray=mapped.array(plane);copy.dstMemoryType=CU_MEMORYTYPE_DEVICE;copy.dstDevice=buffers[index]+(plane?size_t(width)*height:0);copy.dstPitch=width;copy.WidthInBytes=width;copy.Height=plane?height/2:height;
            cu_check(cuMemcpy2D(&copy),"GPU Y/UV plane to packed NV12");
        }
        mapped.close();
    }
    GpuFrame output_gpu(int64_t pts){
        {MappedPlanes mapped(interop.data()+2);
            for(unsigned plane=0;plane<2;++plane){
                CUDA_MEMCPY2D copy{};copy.srcMemoryType=CU_MEMORYTYPE_DEVICE;copy.srcDevice=buffers[2]+(plane?size_t(width)*height:0);copy.srcPitch=width;copy.dstMemoryType=CU_MEMORYTYPE_ARRAY;copy.dstArray=mapped.array(plane);copy.WidthInBytes=width;copy.Height=plane?height/2:height;
                cu_check(cuMemcpy2D(&copy),"GPU packed NV12 to Y/UV plane");
            }
            mapped.close();
        }
        cu_check(cuCtxSynchronize(),"Synchronize CUDA NV12 output planes");GpuFrame result{make_texture(DXGI_FORMAT_NV12,width,height),0,width,height,pts};
        for(unsigned plane=0;plane<2;++plane){
            auto target=target_view(result.texture.Get(),plane?DXGI_FORMAT_R8G8_UNORM:DXGI_FORMAT_R8_UNORM);
            draw_plane(target.Get(),plane_views[plane+2].Get(),plane_views[plane+2].Get(),plane?width/2:width,plane?height/2:height,0);
        }
        wait_gpu();return result;
    }
    void prime(const GpuFrame& previous){
        close_session();
        wrapper_check(create(&wrapper),"NVEncNVOFFRUCCreate");if(!wrapper)throw std::runtime_error("NVEnc wrapper returned null");wrapper_check(load(wrapper),"NVEncNVOFFRUCLoad");
        wrapper_check(init(wrapper,width,height,true),"Create NV12 FRUC session");wrapper_check(register_resources(wrapper,&buffers[0],&buffers[1],&buffers[2]),"Register NV12 FRUC resources");
        upload_gpu(previous,0);WrapperParams params{&buffers[0],0,&buffers[2],0};wrapper_check(process(wrapper,&params),"Prime NV12 FRUC");cu_check(cuCtxSynchronize(),"Synchronize FRUC prime");
        primed=true;cached_buffer=0;clock=0;cached_texture=previous.texture;cached_slice=previous.array_slice;cached_pts=previous.pts;
    }
};

GpuFrucEngine::GpuFrucEngine(const std::filesystem::path& runtime,ID3D11Device* device,ID3D11DeviceContext* context,HANDLE decoder_mutex):impl_(std::make_unique<Impl>(runtime,device,context,decoder_mutex)){}
GpuFrucEngine::~GpuFrucEngine()=default;
std::string GpuFrucEngine::device_name()const{return impl_->name;}
void GpuFrucEngine::reset()noexcept{std::lock_guard<std::mutex> lock(impl_->mutex);if(cuCtxPushCurrent(impl_->cuda_context)==CUDA_SUCCESS){impl_->close_session();CUcontext previous=nullptr;cuCtxPopCurrent(&previous);}}
GpuFrame GpuFrucEngine::copy(const GpuFrame& input){
    std::lock_guard<std::mutex> lock(impl_->mutex);impl_->validate(input);Impl::DeviceScope device(*impl_);
    GpuFrame result{impl_->make_texture(DXGI_FORMAT_NV12,input.width,input.height),0,input.width,input.height,input.pts};
    D3D11_BOX box{0,0,0,UINT(input.width),UINT(input.height),1};impl_->context->CopySubresourceRegion(result.texture.Get(),0,0,0,0,input.texture.Get(),input.array_slice,&box);impl_->wait_gpu();return result;
}
GpuFrame GpuFrucEngine::midpoint(const GpuFrame& previous,const GpuFrame& current){
    std::lock_guard<std::mutex> lock(impl_->mutex);impl_->validate(previous);impl_->validate(current);
    if(previous.width!=current.width||previous.height!=current.height||previous.pts<0||current.pts<=previous.pts)throw std::invalid_argument("GPU midpoint requires matching dimensions and increasing nonnegative timestamps");
    Impl::DeviceScope device(*impl_);CudaScope cuda(impl_->cuda_context);
    try{
        impl_->configure(previous.width,previous.height);
        if(!impl_->primed||impl_->cached_texture.Get()!=previous.texture.Get()||impl_->cached_slice!=previous.array_slice||impl_->cached_pts!=previous.pts)impl_->prime(previous);
        const int next=1-impl_->cached_buffer;impl_->upload_gpu(current,next);const int64_t next_clock=impl_->clock+2;
        WrapperParams params{&impl_->buffers[next],next_clock,&impl_->buffers[2],next_clock-1};wrapper_check(impl_->process(impl_->wrapper,&params),"GPU NV12 FRUC midpoint");cu_check(cuCtxSynchronize(),"Synchronize GPU NV12 FRUC");
        auto result=impl_->output_gpu(previous.pts+(current.pts-previous.pts)/2);
        impl_->cached_texture=current.texture;impl_->cached_slice=current.array_slice;impl_->cached_pts=current.pts;impl_->cached_buffer=next;impl_->clock=next_clock;return result;
    }catch(...){impl_->close_session();throw;}
}
GpuFrame GpuFrucEngine::blend(const GpuFrame& previous,const GpuFrame& current,int64_t pts){
    if(pts<=previous.pts){auto result=copy(previous);result.pts=pts;return result;}if(pts>=current.pts){auto result=copy(current);result.pts=pts;return result;}
    std::lock_guard<std::mutex> lock(impl_->mutex);impl_->validate(previous);impl_->validate(current);
    if(previous.width!=current.width||previous.height!=current.height||previous.pts<0||current.pts<=previous.pts)throw std::invalid_argument("GPU blend requires matching dimensions and increasing nonnegative timestamps");
    Impl::DeviceScope device(*impl_);
    const auto a=impl_->shader_source_texture(previous),b=impl_->shader_source_texture(current);
    const uint32_t weight=blend_weight(pts-previous.pts,current.pts-previous.pts);
    GpuFrame result{impl_->make_texture(DXGI_FORMAT_NV12,previous.width,previous.height),0,previous.width,previous.height,pts};
    for(unsigned plane=0;plane<2;++plane){
        const auto format=plane?DXGI_FORMAT_R8G8_UNORM:DXGI_FORMAT_R8_UNORM;
        auto first=impl_->source_view(a.Get(),format),second=impl_->source_view(b.Get(),format);auto target=impl_->target_view(result.texture.Get(),format);
        impl_->draw_plane(target.Get(),first.Get(),second.Get(),plane?previous.width/2:previous.width,plane?previous.height/2:previous.height,weight);
    }
    impl_->wait_gpu();return result;
}
}
