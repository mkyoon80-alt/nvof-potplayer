#include "nvof/cpu_upload.hpp"
#include "cpu_upload_shaders.hpp"
#include <array>
#include <d3d11_1.h>
#include <d3d10_1.h>
using Microsoft::WRL::ComPtr;
namespace nvof {
namespace {
void checked(HRESULT hr,const char* message){if(FAILED(hr))throw std::runtime_error(std::string(message)+" HRESULT="+std::to_string(uint32_t(hr)));}
struct DeviceLock {
    HANDLE mutex;
    explicit DeviceLock(HANDLE h):mutex(h){if(h){DWORD w=WaitForSingleObject(h,INFINITE);if(w!=WAIT_OBJECT_0 && w!=WAIT_ABANDONED)throw std::runtime_error("CPU upload device lock");}}
    ~DeviceLock(){if(mutex)ReleaseMutex(mutex);}
};
}
struct CpuUpload::Impl {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;HANDLE mutex;CpuVideoLayout layout{};
    ComPtr<ID3D11DeviceContext1> context1;ComPtr<ID3DDeviceContextState> private_state;ComPtr<ID3D10Multithread> multithread;
    struct StateScope {
        Impl& owner;DeviceLock lock;bool protected_context=false;ComPtr<ID3DDeviceContextState> saved;
        explicit StateScope(Impl& a):owner(a),lock(a.mutex) {
            if(a.multithread && a.multithread->GetMultithreadProtected()){a.multithread->Enter();protected_context=true;}
            a.context1->SwapDeviceContextState(a.private_state.Get(),&saved);
        }
        ~StateScope(){owner.context1->SwapDeviceContextState(saved.Get(),nullptr);if(protected_context)owner.multithread->Leave();}
    };
    struct Slot{ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11ShaderResourceView> view;};
    std::array<Slot,6> inputs;size_t current=0,previous=0,next=0;bool valid=false,history=false;
    ComPtr<ID3D11Texture2D> output;std::array<ComPtr<ID3D11RenderTargetView>,2> targets;
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;ComPtr<ID3D11Buffer> constants;ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11ComputeShader> comb_shader;ComPtr<ID3D11Buffer> comb_buffer,comb_readback;
    ComPtr<ID3D11UnorderedAccessView> comb_view;
    unsigned comb_streak=0,clear_streak=0;bool field_mode=false,comb_checked=false,field_hint=false;
    Impl(ID3D11Device* d,ID3D11DeviceContext* c,HANDLE h):device(d),context(c),mutex(h) {
        if(!d || !c || c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)throw std::invalid_argument("CPU upload requires an immediate D3D11 context");
        ComPtr<ID3D11Device> owner;c->GetDevice(&owner);if(owner.Get()!=d)throw std::invalid_argument("CPU upload context device mismatch");
        checked(context.As(&context1),"CPU upload context1");context.As(&multithread);
        ComPtr<ID3D11Device1> device1;checked(device.As(&device1),"CPU upload device1");
        const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};D3D_FEATURE_LEVEL level{};
        checked(device1->CreateDeviceContextState(0,levels,2,D3D11_SDK_VERSION,__uuidof(ID3D11Device),&level,&private_state),"CPU upload private context state");
        checked(d->CreateVertexShader(shaders::cpu_upload::vs,sizeof(shaders::cpu_upload::vs),nullptr,&vs),"CPU upload vertex shader");
        checked(d->CreatePixelShader(shaders::cpu_upload::ps,sizeof(shaders::cpu_upload::ps),nullptr,&ps),"CPU upload field shader");
        checked(d->CreateComputeShader(shaders::cpu_upload::comb,sizeof(shaders::cpu_upload::comb),nullptr,&comb_shader),"CPU upload comb detector");
        D3D11_BUFFER_DESC stats{};stats.ByteWidth=4;stats.Usage=D3D11_USAGE_DEFAULT;stats.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
        stats.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;stats.StructureByteStride=4;
        checked(d->CreateBuffer(&stats,nullptr,&comb_buffer),"CPU upload comb count");
        D3D11_UNORDERED_ACCESS_VIEW_DESC u{};u.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;u.Buffer.NumElements=1;
        checked(d->CreateUnorderedAccessView(comb_buffer.Get(),&u,&comb_view),"CPU upload comb view");
        stats.Usage=D3D11_USAGE_STAGING;stats.BindFlags=stats.MiscFlags=stats.StructureByteStride=0;stats.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        checked(d->CreateBuffer(&stats,nullptr,&comb_readback),"CPU upload comb readback");
        D3D11_BUFFER_DESC b{};b.ByteWidth=32;b.Usage=D3D11_USAGE_DEFAULT;b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        checked(d->CreateBuffer(&b,nullptr,&constants),"CPU upload constants");
        D3D11_RASTERIZER_DESC r{};r.FillMode=D3D11_FILL_SOLID;r.CullMode=D3D11_CULL_NONE;r.DepthClipEnable=TRUE;
        checked(d->CreateRasterizerState(&r,&raster),"CPU upload rasterizer");
    }
};
CpuUpload::CpuUpload(ID3D11Device* d,ID3D11DeviceContext* c,HANDLE h):impl_(std::make_unique<Impl>(d,c,h)){}
CpuUpload::~CpuUpload()=default;
void CpuUpload::reset(bool preserve_field_hint) noexcept {
    auto& a=*impl_;a.valid=false;a.history=false;a.comb_streak=a.clear_streak=0;
    a.field_mode=false;a.comb_checked=false;
    if(!preserve_field_hint)a.field_hint=false;
}
void CpuUpload::begin(const uint8_t* data,size_t bytes,const CpuVideoLayout& l,bool disc) {
    if(!data || bytes<cpu_video_bytes(l))throw std::invalid_argument("Truncated CPU upload");
    auto& a=*impl_;DeviceLock lock(a.mutex);
    if(l.width!=a.layout.width || l.height!=a.layout.height || l.format!=a.layout.format) {
        for(auto& slot:a.inputs){slot.view.Reset();slot.texture.Reset();}
        for(auto& target:a.targets)target.Reset();a.output.Reset();a.valid=false;a.next=0;a.field_hint=false;
    }
    a.layout=l;a.history=a.valid&&!disc;a.previous=a.current;a.current=a.next++%a.inputs.size();auto& slot=a.inputs[a.current];
    a.comb_checked=false;
    if(!a.history){a.comb_streak=a.clear_streak=0;a.field_mode=false;}
    if(!slot.texture) {
        D3D11_TEXTURE2D_DESC d{};d.Width=l.width*(l.format==CpuVideoFormat::y210?2:1);
        d.Height=l.format==CpuVideoFormat::y210?l.height:cpu_422(l.format)?l.height*2:l.height*3/2;
        d.MipLevels=d.ArraySize=1;d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R16_UINT;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        checked(a.device->CreateTexture2D(&d,nullptr,&slot.texture),"CPU raw texture");
        checked(a.device->CreateShaderResourceView(slot.texture.Get(),nullptr,&slot.view),"CPU raw view");
    }
    a.context->UpdateSubresource(slot.texture.Get(),0,nullptr,data,l.pitch,0);a.valid=true;
    if(!a.output) {
        D3D11_TEXTURE2D_DESC d{};d.Width=l.width;d.Height=l.height;d.MipLevels=d.ArraySize=1;d.SampleDesc.Count=1;
        d.Format=DXGI_FORMAT_P010;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        checked(a.device->CreateTexture2D(&d,nullptr,&a.output),"CPU P010 target");
        for(int plane=0;plane<2;++plane) {
            D3D11_RENDER_TARGET_VIEW_DESC v{};v.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;v.Format=plane?DXGI_FORMAT_R16G16_UNORM:DXGI_FORMAT_R16_UNORM;
            checked(a.device->CreateRenderTargetView(a.output.Get(),&v,&a.targets[plane]),"CPU P010 plane target");
        }
    }
}
bool CpuUpload::needs_field_reconstruction() {
    auto& a=*impl_;if(a.comb_checked)return a.field_mode;
    a.comb_checked=true;if(!a.valid || (!a.history&&!a.field_hint) || a.layout.height<8)return false;
    Impl::StateScope scope(a);const auto& l=a.layout;
    const int params[]={l.width,l.height,int(l.format),cpu_422(l.format)?l.height:l.height/2,-1,a.history?1:0,0,a.field_hint?1:0};
    a.context->UpdateSubresource(a.constants.Get(),0,nullptr,params,0,0);
    const UINT zero[4]{};a.context->ClearUnorderedAccessViewUint(a.comb_view.Get(),zero);
    a.context->CSSetShader(a.comb_shader.Get(),nullptr,0);
    ID3D11ShaderResourceView* views[]={a.inputs[a.current].view.Get(),a.inputs[a.previous].view.Get()};
    a.context->CSSetShaderResources(0,2,views);ID3D11Buffer* cb=a.constants.Get();a.context->CSSetConstantBuffers(0,1,&cb);
    ID3D11UnorderedAccessView* u=a.comb_view.Get();a.context->CSSetUnorderedAccessViews(0,1,&u,nullptr);a.context->Dispatch(16,9,1);
    u=nullptr;a.context->CSSetUnorderedAccessViews(0,1,&u,nullptr);ID3D11ShaderResourceView* empty[2]{};a.context->CSSetShaderResources(0,2,empty);
    a.context->CopyResource(a.comb_readback.Get(),a.comb_buffer.Get());D3D11_MAPPED_SUBRESOURCE map{};
    checked(a.context->Map(a.comb_readback.Get(),0,D3D11_MAP_READ,0,&map),"CPU upload comb statistics");
    const unsigned count=*static_cast<const unsigned*>(map.pData);a.context->Unmap(a.comb_readback.Get(),0);
    // A seek invalidates every previous pixel, but not the fact that this
    // input has already delivered raw fields tagged WEAVE. Recheck spatial
    // combs in the new picture instead of passing the first two pictures raw.
    // Blended/progressive input with no combs must still bypass reconstruction.
    if(!a.history){a.field_mode=a.field_hint&&count>=138;a.comb_streak=a.field_mode?2:0;return a.field_mode;}
    // Require repeated spatial evidence. Retain field cadence through brief
    // stillness/flashes; sustained progressive pictures release it again.
    if(count>=138){a.clear_streak=0;if(++a.comb_streak>=2){a.field_mode=true;a.field_hint=true;}}
    else{a.comb_streak=0;if(++a.clear_streak>=16)a.field_mode=false;}
    return a.field_mode;
}
GpuFrame CpuUpload::render(int parity,int64_t pts) {
    // The renderer shares this context, including while presenting a paused
    // frame. Preserve its complete state and prevent calls interleaving with it.
    auto& a=*impl_;Impl::StateScope scope(a);const auto& l=a.layout;
    if(!a.valid || parity < -1 || parity>1 || (parity>=0 && !cpu_422(l.format) && l.height%4))throw std::invalid_argument("Invalid CPU field request");
    ID3D11ShaderResourceView* views[]={a.inputs[a.current].view.Get(),a.inputs[a.history?a.previous:a.current].view.Get()};
    a.context->IASetInputLayout(nullptr);a.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    a.context->VSSetShader(a.vs.Get(),nullptr,0);a.context->PSSetShader(a.ps.Get(),nullptr,0);
    a.context->GSSetShader(nullptr,nullptr,0);a.context->HSSetShader(nullptr,nullptr,0);a.context->DSSetShader(nullptr,nullptr,0);
    a.context->RSSetState(a.raster.Get());a.context->OMSetBlendState(nullptr,nullptr,0xffffffff);a.context->OMSetDepthStencilState(nullptr,0);
    a.context->PSSetShaderResources(0,2,views);ID3D11Buffer* constants=a.constants.Get();a.context->PSSetConstantBuffers(0,1,&constants);
    for(int plane=0;plane<2;++plane) {
        const int params[]={l.width,l.height,int(l.format),cpu_422(l.format)?l.height:l.height/2,parity,a.history?1:0,plane,0};
        a.context->UpdateSubresource(a.constants.Get(),0,nullptr,params,0,0);
        D3D11_VIEWPORT viewport{0,0,float(plane?l.width/2:l.width),float(plane?l.height/2:l.height),0,1};a.context->RSSetViewports(1,&viewport);
        ID3D11RenderTargetView* target=a.targets[plane].Get();a.context->OMSetRenderTargets(1,&target,nullptr);a.context->Draw(3,0);
    }
    ID3D11ShaderResourceView* empty[2]{};a.context->PSSetShaderResources(0,2,empty);a.context->OMSetRenderTargets(0,nullptr,nullptr);
    // Scratch lifetime: copy before next render, ordered on this same context.
    GpuFrame out;out.texture=a.output;out.width=l.width;out.height=l.height;out.pts=pts;return out;
}
}
