#include "nvof/gpu_engine.hpp"
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
void hr_check(HRESULT hr,const char* action) { if(FAILED(hr)) throw std::runtime_error(std::string(action)+" failed, HRESULT="+std::to_string(static_cast<unsigned long>(hr))); }
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
    std::string name;
    std::unique_ptr<MotionSynthesizer> native_synthesizer;
    int64_t cached_pts=0;
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

    Impl(const std::filesystem::path& directory,ID3D11Device* d,ID3D11DeviceContext* c,HANDLE shared_mutex,GpuCompletionMode mode,bool skip,bool stabilize,bool protect,GpuInterpolationBackend backend,unsigned flow_dimension,MotionCostMode costMode,MotionFlowOptions flowOptions):completion_mode(mode),skip_identical_warp(skip),stabilize_midpoint(stabilize),protect_appearance(protect),decoder_mutex(shared_mutex),device(d),context(c){
        try{
            if(backend!=GpuInterpolationBackend::native_experimental)throw std::invalid_argument("FRUC backend was removed; use native synthesis");
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
                native_synthesizer=std::make_unique<MotionSynthesizer>(device.Get(),context.Get(),flow_dimension,costMode,flowOptions);
                ComPtr<IDXGIDevice> dxgi;hr_check(device.As(&dxgi),"Native DXGI device");
                ComPtr<IDXGIAdapter> adapter;hr_check(dxgi->GetAdapter(&adapter),"Native adapter");
                DXGI_ADAPTER_DESC desc{};hr_check(adapter->GetDesc(&desc),"Native adapter name");
                const int bytes=WideCharToMultiByte(CP_UTF8,0,desc.Description,-1,nullptr,0,nullptr,nullptr);
                if(bytes<=0)throw std::runtime_error("Native adapter name conversion failed");
                name.resize(bytes);WideCharToMultiByte(CP_UTF8,0,desc.Description,-1,name.data(),bytes,nullptr,nullptr);
                name.pop_back();
                return;
            }
        }catch(...){cleanup();throw;}
    }
    ~Impl(){cleanup();}
    void cleanup() noexcept {
        if(native_synthesizer) {
            try {DeviceScope scope(*this);wait_gpu();native_synthesizer.reset();output_pool.clear();}
            catch(...) {
                native_synthesizer.release(); // Host lock is unavailable: do not race its context.
                OutputDebugStringA("NVOF: shared D3D11 lock unavailable during native teardown\n");
                return;
            }
        }
        if(completion_event){CloseHandle(completion_event);completion_event=nullptr;}
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

};

GpuFrucEngine::GpuFrucEngine(const std::filesystem::path& runtime,ID3D11Device* device,ID3D11DeviceContext* context,HANDLE decoder_mutex,GpuCompletionMode mode,bool skip,bool stabilize,bool protect,GpuInterpolationBackend backend,unsigned flow_dimension,MotionCostMode costMode,MotionFlowOptions flowOptions):impl_(std::make_unique<Impl>(runtime,device,context,decoder_mutex,mode,skip,stabilize,protect,backend,flow_dimension,costMode,flowOptions)){}
MotionAnalysisInfo GpuFrucEngine::analysis_info() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->native_synthesizer?impl_->native_synthesizer->analysis_info():MotionAnalysisInfo{};
}
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
    PhaseBatch<GpuFrame> result{};if(timestamps.empty())return result;
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
    throw std::logic_error("Native synthesizer unavailable");
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
