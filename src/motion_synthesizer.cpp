#include "nvof/motion_synthesizer.hpp"
#include <windows.h>
#include "motion_shaders.hpp"
#include <wrl/client.h>
#include <nvOpticalFlowD3D11.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace nvof {
namespace {
using Microsoft::WRL::ComPtr;
void check(HRESULT hr, const char* name) {
    if(FAILED(hr)) throw std::runtime_error(std::string(name)+" HRESULT="+std::to_string(static_cast<unsigned long>(hr)));
}
}

struct MotionSynthesizer::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    HMODULE module=nullptr;
    NV_OF_D3D11_API_FUNCTION_LIST api{};
    NvOFHandle handle=nullptr;
    std::array<ComPtr<ID3D11Texture2D>,2> sources,inputs,flows,costs;
    std::array<ComPtr<ID3D11ShaderResourceView>,2> ys,uvs,fvs,costViews;
    std::array<std::array<ComPtr<ID3D11RenderTargetView>,2>,2> inputTargets;
    std::array<NvOFGPUBufferHandle,2> ih{},fh{},ch{};
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> synth,reduce,invert,stationary,expandStationary,repair;
    std::array<ComPtr<ID3D11Texture2D>,2> repairScratch,repairedTextures;
    std::array<ComPtr<ID3D11RenderTargetView>,2> repairScratchTargets;
    std::array<ComPtr<ID3D11ShaderResourceView>,2> repairScratchViews;
    std::array<ComPtr<ID3D11RenderTargetView>,2> repairedTargets;
    std::array<ComPtr<ID3D11ShaderResourceView>,2> repairedViews;
    std::array<ComPtr<ID3D11Texture2D>,2> layerTextures;
    std::array<ComPtr<ID3D11RenderTargetView>,2> layerTargets;
    std::array<ComPtr<ID3D11ShaderResourceView>,2> layerViews;
    ComPtr<ID3D11ComputeShader> assess;
    ComPtr<ID3D11Buffer> riskBuffer,riskReadback;
    ComPtr<ID3D11UnorderedAccessView> riskUav;
    bool trustworthy=false;
    std::array<ComPtr<ID3D11Texture2D>,2> warpMaps;
    std::array<ComPtr<ID3D11RenderTargetView>,2> warpTargets;
    std::array<ComPtr<ID3D11ShaderResourceView>,2> warpViews;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11Buffer> constants;
    unsigned maxDimension,grid=0;
    MotionCostMode costMode;
    MotionFlowOptions flowOptions;
    DXGI_FORMAT costFormat=DXGI_FORMAT_UNKNOWN;
    int w=0,h=0,aw=0,ah=0;
    bool prepared=false,history=false;

    Impl(ID3D11Device* d,ID3D11DeviceContext* c,unsigned limit,MotionCostMode costMode,MotionFlowOptions options):device(d),context(c),maxDimension(limit),costMode(costMode),flowOptions(options) {
        if((options.output_grid!=0&&options.output_grid!=1&&options.output_grid!=2&&options.output_grid!=4)||
           (options.quality!=MotionFlowQuality::medium&&options.quality!=MotionFlowQuality::slow))throw std::invalid_argument("Invalid flow grid or quality");
        if(!d||!c||limit<160||limit>8192)throw std::invalid_argument("Invalid motion synthesis configuration");
        ComPtr<ID3D11Device> owner;c->GetDevice(&owner);
        if(owner.Get()!=d||c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)
            throw std::invalid_argument("Motion synthesis requires matching immediate context");
        try {
            module=LoadLibraryExW(L"nvofapi64.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
            if(!module)throw std::runtime_error("NVOFA driver unavailable");
            using Create=NV_OF_STATUS(NVOFAPI*)(uint32_t,NV_OF_D3D11_API_FUNCTION_LIST*);
            auto create=reinterpret_cast<Create>(GetProcAddress(module,"NvOFAPICreateInstanceD3D11"));
            if(!create)throw std::runtime_error("NVOFA D3D11 entry point unavailable");
            of(create(NV_OF_API_VERSION,&api),"Create NVOFA API");
            if(!api.nvCreateOpticalFlowD3D11||!api.nvOFInit||!api.nvOFExecute||!api.nvOFDestroy||
               !api.nvOFGetCaps||!api.nvOFGetSurfaceFormatCountD3D11||!api.nvOFGetSurfaceFormatD3D11||!api.nvOFRegisterResourceD3D11||!api.nvOFUnregisterResourceD3D11)
                throw std::runtime_error("Incomplete NVOFA API");
            check(d->CreateVertexShader(shaders::motion::vs,sizeof(shaders::motion::vs),nullptr,&vertex),"Motion vs");
            check(d->CreatePixelShader(shaders::motion::midpoint,sizeof(shaders::motion::midpoint),nullptr,&synth),"Motion midpoint");
            check(d->CreatePixelShader(shaders::motion::downsample,sizeof(shaders::motion::downsample),nullptr,&reduce),"Motion downsample");
            const unsigned char* inverseCode=shaders::motion::inverseMap;
            size_t inverseSize=sizeof(shaders::motion::inverseMap);
            if(costMode==MotionCostMode::disabled){inverseCode=shaders::motion::inverseMapOff;inverseSize=sizeof(shaders::motion::inverseMapOff);}
            else if(costMode==MotionCostMode::blend_only){inverseCode=shaders::motion::inverseMapBlend;inverseSize=sizeof(shaders::motion::inverseMapBlend);}
            else if(costMode!=MotionCostMode::confidence_fusion)throw std::invalid_argument("Invalid cost mode");
            check(d->CreatePixelShader(inverseCode,inverseSize,nullptr,&invert),"Motion inverseMap");
            check(d->CreatePixelShader(shaders::motion::stationaryMask,sizeof(shaders::motion::stationaryMask),nullptr,&stationary),"Motion stationaryMask");
            check(d->CreatePixelShader(shaders::motion::expandStationaryMask,sizeof(shaders::motion::expandStationaryMask),nullptr,&expandStationary),"Motion expandStationaryMask");
            check(d->CreatePixelShader(shaders::motion::repairMotion,sizeof(shaders::motion::repairMotion),nullptr,&repair),"Motion repairMotion");
            check(d->CreateComputeShader(shaders::motion::assessMotion,sizeof(shaders::motion::assessMotion),nullptr,&assess),"Motion assessMotion");
            D3D11_BUFFER_DESC rb{};rb.ByteWidth=4;rb.Usage=D3D11_USAGE_DEFAULT;rb.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
            rb.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;rb.StructureByteStride=4;
            check(d->CreateBuffer(&rb,nullptr,&riskBuffer),"Motion assessment buffer");
            D3D11_UNORDERED_ACCESS_VIEW_DESC ru{};ru.Format=DXGI_FORMAT_UNKNOWN;ru.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;ru.Buffer.NumElements=1;
            check(d->CreateUnorderedAccessView(riskBuffer.Get(),&ru,&riskUav),"Motion assessment UAV");
            rb.Usage=D3D11_USAGE_STAGING;rb.BindFlags=rb.MiscFlags=rb.StructureByteStride=0;rb.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            check(d->CreateBuffer(&rb,nullptr,&riskReadback),"Motion assessment readback");
            D3D11_BUFFER_DESC cb{};cb.ByteWidth=32;cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            check(d->CreateBuffer(&cb,nullptr,&constants),"Motion constants");
            D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
            check(d->CreateSamplerState(&sd,&sampler),"Motion sampler");
            D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
            check(d->CreateRasterizerState(&rd,&raster),"Motion rasterizer");
        }catch(...){reset();if(module)FreeLibrary(module);module=nullptr;throw;}
    }
    ~Impl(){reset();if(module)FreeLibrary(module);}
    void of(NV_OF_STATUS status,const char* action) {
        if(status!=NV_OF_SUCCESS)throw std::runtime_error(std::string(action)+" NVOF="+std::to_string(status));
    }
    void reset() noexcept {
        prepared=history=trustworthy=false;
        if(handle){
            for(auto& p:ch)if(p){api.nvOFUnregisterResourceD3D11(p);p=nullptr;}
            for(auto& p:fh)if(p){api.nvOFUnregisterResourceD3D11(p);p=nullptr;}
            for(auto& p:ih)if(p){api.nvOFUnregisterResourceD3D11(p);p=nullptr;}
            api.nvOFDestroy(handle);handle=nullptr;
        }
        for(auto& p:ys)p.Reset();for(auto& p:uvs)p.Reset();for(auto& p:fvs)p.Reset();
        for(auto& row:inputTargets)for(auto& p:row)p.Reset();
        for(auto& p:sources)p.Reset();for(auto& p:inputs)p.Reset();for(auto& p:flows)p.Reset();
        for(auto& p:warpViews)p.Reset();for(auto& p:warpTargets)p.Reset();for(auto& p:warpMaps)p.Reset();
        for(auto& p:layerViews)p.Reset();for(auto& p:layerTargets)p.Reset();for(auto& p:layerTextures)p.Reset();
        for(auto& p:repairedViews)p.Reset();for(auto& p:repairedTargets)p.Reset();for(auto& p:repairedTextures)p.Reset();
        for(auto& p:costViews)p.Reset();for(auto& p:costs)p.Reset();
        for(auto& p:repairScratchViews)p.Reset();for(auto& p:repairScratchTargets)p.Reset();for(auto& p:repairScratch)p.Reset();
        w=h=aw=ah=0;grid=0;costFormat=DXGI_FORMAT_UNKNOWN;
    }
    ComPtr<ID3D11ShaderResourceView> srv(ID3D11Texture2D* t,DXGI_FORMAT format) {
        D3D11_SHADER_RESOURCE_VIEW_DESC d{};d.Format=format;d.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;d.Texture2D.MipLevels=1;
        ComPtr<ID3D11ShaderResourceView> v;check(device->CreateShaderResourceView(t,&d,&v),"Motion SRV");return v;
    }
    void configure(int width,int height) {
        if(handle&&w==width&&h==height)return;
        reset();
        try {
            w=width;h=height;
            double scale=(std::min)(1.0,double(maxDimension)/(std::max)(w,h));
            aw=(std::max)(2,int(w*scale)&~1);ah=(std::max)(2,int(h*scale)&~1);
            for(unsigned i=0;i<2;++i) {
                D3D11_TEXTURE2D_DESC d{};d.Width=aw;d.Height=ah;d.MipLevels=d.ArraySize=1;
                d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;d.SampleDesc.Count=1;
                d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
                check(device->CreateTexture2D(&d,nullptr,&warpMaps[i]),"Inverse map texture");
                check(device->CreateRenderTargetView(warpMaps[i].Get(),nullptr,&warpTargets[i]),"Inverse map RTV");
                warpViews[i]=srv(warpMaps[i].Get(),d.Format);
            }
            for(unsigned i=0;i<2;++i) {
                D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=1;
                d.Format=DXGI_FORMAT_R8_UNORM;d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;
                d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
                check(device->CreateTexture2D(&d,nullptr,&layerTextures[i]),"Stationary mask texture");
                check(device->CreateRenderTargetView(layerTextures[i].Get(),nullptr,&layerTargets[i]),"Stationary mask RTV");
                layerViews[i]=srv(layerTextures[i].Get(),d.Format);
            }
            of(api.nvCreateOpticalFlowD3D11(device.Get(),context.Get(),&handle),"Create NVOFA");
            uint32_t count=0;
            of(api.nvOFGetCaps(handle,NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES,nullptr,&count),"Flow grid count");
            if(!count||count>32)throw std::runtime_error("Invalid NVOFA grid count");
            std::vector<uint32_t> grids(count);
            of(api.nvOFGetCaps(handle,NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES,grids.data(),&count),"Flow grids");
            if(flowOptions.output_grid) {
                if(std::find(grids.begin(),grids.end(),flowOptions.output_grid)==grids.end())
                    throw std::runtime_error("Requested flow grid is unsupported: "+std::to_string(flowOptions.output_grid));
                grid=flowOptions.output_grid;
            } else for(unsigned g:{4u,2u,1u})if(std::find(grids.begin(),grids.end(),g)!=grids.end()){grid=g;break;}
            if(!grid)throw std::runtime_error("No supported flow grid");
            // Disabled mode neither queries, allocates, registers nor executes cost output.
            if(costMode!=MotionCostMode::disabled) {
                // Query rather than assuming that every driver supports 8-bit cost.
                uint32_t costCount=0;
                of(api.nvOFGetSurfaceFormatCountD3D11(handle,NV_OF_BUFFER_USAGE_COST,NV_OF_MODE_OPTICALFLOW,&costCount),"Cost format count");
                if(costCount>32)throw std::runtime_error("Invalid NVOFA cost format count");
                if(costCount) {
                    std::vector<DXGI_FORMAT> formats(costCount);
                    of(api.nvOFGetSurfaceFormatD3D11(handle,NV_OF_BUFFER_USAGE_COST,NV_OF_MODE_OPTICALFLOW,formats.data()),"Cost formats");
                    // Unknown cost formats are ignored: keep baseline synthesis available.
                    if(std::find(formats.begin(),formats.end(),DXGI_FORMAT_R8_UINT)!=formats.end())costFormat=DXGI_FORMAT_R8_UINT;
                }
            }
            NV_OF_INIT_PARAMS init{};init.width=aw;init.height=ah;
            init.enableOutputCost=costFormat==DXGI_FORMAT_R8_UINT?NV_OF_TRUE:NV_OF_FALSE;
            init.outGridSize=static_cast<NV_OF_OUTPUT_VECTOR_GRID_SIZE>(grid);
            init.mode=NV_OF_MODE_OPTICALFLOW;init.perfLevel=flowOptions.quality==MotionFlowQuality::slow?NV_OF_PERF_LEVEL_SLOW:NV_OF_PERF_LEVEL_MEDIUM;
            init.predDirection=NV_OF_PRED_DIRECTION_BOTH;init.inputBufferFormat=NV_OF_BUFFER_FORMAT_NV12;
            of(api.nvOFInit(handle,&init),"Initialize NVOFA");
            for(unsigned i=0;i<2;++i){
                D3D11_TEXTURE2D_DESC d{};d.Width=aw;d.Height=ah;d.MipLevels=d.ArraySize=1;
                d.Format=DXGI_FORMAT_NV12;d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;
                d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
                check(device->CreateTexture2D(&d,nullptr,&inputs[i]),"Analysis NV12");
                for(unsigned p=0;p<2;++p){
                    D3D11_RENDER_TARGET_VIEW_DESC v{};v.Format=p?DXGI_FORMAT_R8G8_UNORM:DXGI_FORMAT_R8_UNORM;v.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
                    check(device->CreateRenderTargetView(inputs[i].Get(),&v,&inputTargets[i][p]),"Analysis RTV");
                }
                d.Width=(aw+grid-1)/grid;d.Height=(ah+grid-1)/grid;d.Format=DXGI_FORMAT_R16G16_SINT;
                d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
                check(device->CreateTexture2D(&d,nullptr,&flows[i]),"Flow texture");
                fvs[i]=srv(flows[i].Get(),d.Format);
                d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
                check(device->CreateTexture2D(&d,nullptr,&repairedTextures[i]),"Repaired flow texture");
                check(device->CreateRenderTargetView(repairedTextures[i].Get(),nullptr,&repairedTargets[i]),"Repaired flow RTV");
                repairedViews[i]=srv(repairedTextures[i].Get(),d.Format);
                check(device->CreateTexture2D(&d,nullptr,&repairScratch[i]),"Repair scratch texture");
                check(device->CreateRenderTargetView(repairScratch[i].Get(),nullptr,&repairScratchTargets[i]),"Repair scratch RTV");
                repairScratchViews[i]=srv(repairScratch[i].Get(),d.Format);
                if(costFormat!=DXGI_FORMAT_UNKNOWN) {
                    d.Format=costFormat;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
                    check(device->CreateTexture2D(&d,nullptr,&costs[i]),"Cost texture");
                    costViews[i]=srv(costs[i].Get(),d.Format);
                    of(api.nvOFRegisterResourceD3D11(handle,costs[i].Get(),&ch[i]),"Register cost");
                }
                of(api.nvOFRegisterResourceD3D11(handle,inputs[i].Get(),&ih[i]),"Register analysis");
                of(api.nvOFRegisterResourceD3D11(handle,flows[i].Get(),&fh[i]),"Register flow");
            }
        }catch(...){reset();throw;}
    }
    void assess_motion() {
        const UINT zero[4]{};context->ClearUnorderedAccessViewUint(riskUav.Get(),zero);
        float data[8]={float(w),float(h),float(aw),float(ah),float(grid),0,0,0};
        context->UpdateSubresource(constants.Get(),0,nullptr,data,0,0);
        auto cb=constants.Get();auto sp=sampler.Get();auto output=riskUav.Get();
        ID3D11ShaderResourceView* views[]={ys[0].Get(),ys[1].Get(),uvs[0].Get(),uvs[1].Get(),fvs[0].Get(),fvs[1].Get()};
        context->CSSetShader(assess.Get(),nullptr,0);context->CSSetConstantBuffers(0,1,&cb);context->CSSetSamplers(0,1,&sp);
        context->CSSetShaderResources(0,6,views);context->CSSetUnorderedAccessViews(0,1,&output,nullptr);context->Dispatch(16,9,1);
        ID3D11ShaderResourceView* empty[6]{};ID3D11UnorderedAccessView* none=nullptr;
        context->CSSetShaderResources(0,6,empty);context->CSSetUnorderedAccessViews(0,1,&none,nullptr);context->CSSetShader(nullptr,nullptr,0);
        context->CopyResource(riskReadback.Get(),riskBuffer.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context->Map(riskReadback.Get(),0,D3D11_MAP_READ,0,&mapped),"Read motion assessment");
        const uint32_t bad=*static_cast<const uint32_t*>(mapped.pData);context->Unmap(riskReadback.Get(),0);
        trustworthy=bad<=uint32_t(128*72*2*0.08);
    }
    void draw(ID3D11RenderTargetView* target,unsigned plane,bool down,unsigned source=0,float fraction=0.5f,bool mapping=false,unsigned stationaryStage=0,bool repairing=false) {
        float data[8]={float(w),float(h),float(aw),float(ah),float(grid),float(plane),float(source),fraction};
        context->UpdateSubresource(constants.Get(),0,nullptr,data,0,0);
        ID3D11ShaderResourceView* views[]={ys[0].Get(),ys[1].Get(),uvs[0].Get(),uvs[1].Get(),down?nullptr:(repairing?(fraction>0?fvs[0].Get():repairScratchViews[0].Get()):repairedViews[0].Get()),down?nullptr:(repairing?(fraction>0?fvs[1].Get():repairScratchViews[1].Get()):repairedViews[1].Get()),down||mapping||stationaryStage||repairing?nullptr:warpViews[0].Get(),down||mapping||stationaryStage||repairing?nullptr:warpViews[1].Get(),stationaryStage>1?layerViews[stationaryStage-2].Get():(down||mapping||stationaryStage||repairing?nullptr:layerViews[0].Get()),mapping?costViews[0].Get():nullptr,mapping?costViews[1].Get():nullptr,mapping&&costMode!=MotionCostMode::disabled?fvs[0].Get():nullptr,mapping&&costMode!=MotionCostMode::disabled?fvs[1].Get():nullptr};
        auto cb=constants.Get();auto sp=sampler.Get();
        context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex.Get(),nullptr,0);context->HSSetShader(nullptr,nullptr,0);
        context->DSSetShader(nullptr,nullptr,0);context->GSSetShader(nullptr,nullptr,0);
        context->PSSetShader(repairing?repair.Get():stationaryStage?(stationaryStage==1?stationary.Get():expandStationary.Get()):(down?reduce.Get():(mapping?invert.Get():synth.Get())),nullptr,0);
        context->PSSetConstantBuffers(0,1,&cb);context->PSSetSamplers(0,1,&sp);
        context->RSSetState(raster.Get());context->OMSetBlendState(nullptr,nullptr,0xffffffff);context->OMSetDepthStencilState(nullptr,0);
        D3D11_VIEWPORT vp{0,0,float((down||mapping?aw:w)/(plane?2:1)),float((down||mapping?ah:h)/(plane?2:1)),0,1};
        if(repairing){vp.Width=float((aw+grid-1)/grid);vp.Height=float((ah+grid-1)/grid);}
        context->RSSetViewports(1,&vp);
        if(mapping){ID3D11RenderTargetView* targets[]={warpTargets[0].Get(),warpTargets[1].Get()};context->OMSetRenderTargets(2,targets,nullptr);}
        else context->OMSetRenderTargets(1,&target,nullptr);
        context->PSSetShaderResources(0,13,views);context->Draw(3,0);
        ID3D11ShaderResourceView* empty[13]{};context->PSSetShaderResources(0,13,empty);context->OMSetRenderTargets(0,nullptr,nullptr);
    }
    void validate(ID3D11Texture2D* t,int width,int height) {
        if(!t||width<4||height<4||width>8192||height>8192||(width&1)||(height&1))
            throw std::invalid_argument("Invalid synthesis dimensions");
        D3D11_TEXTURE2D_DESC d{};t->GetDesc(&d);ComPtr<ID3D11Device> owner;t->GetDevice(&owner);
        if(owner.Get()!=device.Get()||d.Format!=DXGI_FORMAT_NV12||d.Width!=UINT(width)||d.Height!=UINT(height)||
           d.ArraySize!=1||d.MipLevels!=1||d.SampleDesc.Count!=1)
            throw std::invalid_argument("Synthesis needs exact-size single-slice NV12");
    }
    void validate_target(ID3D11RenderTargetView* t,unsigned plane) {
        if(!t)throw std::invalid_argument("Null synthesis target");
        D3D11_RENDER_TARGET_VIEW_DESC vd{};t->GetDesc(&vd);
        if(vd.Format!=(plane?DXGI_FORMAT_R8G8_UNORM:DXGI_FORMAT_R8_UNORM)||vd.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D||vd.Texture2D.MipSlice!=0)
            throw std::invalid_argument("Invalid synthesis target view");
        ComPtr<ID3D11Resource> resource;t->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;check(resource.As(&texture),"Synthesis target texture");
        validate(texture.Get(),w,h);
        for(const auto& s:sources)if(s.Get()==texture.Get())throw std::invalid_argument("Synthesis output aliases source");
    }
};

MotionSynthesizer::MotionSynthesizer(ID3D11Device* d,ID3D11DeviceContext* c,unsigned limit,MotionCostMode costMode,MotionFlowOptions options):impl_(std::make_unique<Impl>(d,c,limit,costMode,options)){}
MotionSynthesizer::~MotionSynthesizer()=default;
bool MotionSynthesizer::cost_map_active() const noexcept {return impl_->prepared&&impl_->costFormat==DXGI_FORMAT_R8_UINT;}
MotionAnalysisInfo MotionSynthesizer::analysis_info() const noexcept {
    return {unsigned(impl_->aw),unsigned(impl_->ah),impl_->grid,unsigned(impl_->ch[0]!=nullptr)+unsigned(impl_->ch[1]!=nullptr)};
}
bool MotionSynthesizer::reliable() const noexcept {return impl_->prepared&&impl_->trustworthy;}
void MotionSynthesizer::reset() noexcept {impl_->reset();}
// Merely toggling disableTemporalHints retained wrong-direction motion in
// the seek/reversal regression on RTX 5090 / 617.42. Recreate the NVOFA session
// on discontinuities until a resource-preserving reset is independently proven.
// Shaders and the player's output surface pool stay alive.
void MotionSynthesizer::invalidate_history() noexcept {impl_->reset();}
void MotionSynthesizer::prepare(ID3D11Texture2D* a,ID3D11Texture2D* b,int w,int h) {
    // Reusing the NVOFA session produced stale/inconsistent motion on the
    // opening-pan and variable-speed regressions (RTX 5090 / 617.42).
    // Input rotation and completion waits alone were insufficient. Keep shader
    // objects and the player's output pool, but isolate each analysis pair until
    // session reuse passes these quality checks. This has a measured time cost.
    impl_->reset();
    impl_->prepared=false;impl_->validate(a,w,h);impl_->validate(b,w,h);impl_->configure(w,h);
    ID3D11Texture2D* sources[]={a,b};
    for(unsigned i=0;i<2;++i){
        impl_->sources[i]=sources[i];impl_->ys[i]=impl_->srv(sources[i],DXGI_FORMAT_R8_UNORM);
        impl_->uvs[i]=impl_->srv(sources[i],DXGI_FORMAT_R8G8_UNORM);
        if(w==impl_->aw&&h==impl_->ah)impl_->context->CopyResource(impl_->inputs[i].Get(),sources[i]);
        else {impl_->draw(impl_->inputTargets[i][0].Get(),0,true,i);impl_->draw(impl_->inputTargets[i][1].Get(),1,true,i);}
    }
    // Complete the new analysis inputs before the first execute of this session.
    if(!impl_->history) {
        D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT,0};ComPtr<ID3D11Query> done;
        check(impl_->device->CreateQuery(&desc,&done),"Cold analysis completion query");
        impl_->context->End(done.Get());impl_->context->Flush();
        const ULONGLONG deadline=GetTickCount64()+1000;
        for(;;) {
            HRESULT hr=impl_->context->GetData(done.Get(),nullptr,0,0);
            check(hr,"Cold analysis completion");
            if(hr==S_OK)break;
            if(GetTickCount64()>=deadline)throw std::runtime_error("Cold analysis completion timed out");
            SwitchToThread();
        }
    }
    NV_OF_EXECUTE_INPUT_PARAMS in{};in.inputFrame=impl_->ih[0];in.referenceFrame=impl_->ih[1];
    in.disableTemporalHints=impl_->history?NV_OF_FALSE:NV_OF_TRUE;
    NV_OF_EXECUTE_OUTPUT_PARAMS out{};out.outputBuffer=impl_->fh[0];out.bwdOutputBuffer=impl_->fh[1];
    out.outputCostBuffer=impl_->ch[0];out.bwdOutputCostBuffer=impl_->ch[1];
    try{
        impl_->of(impl_->api.nvOFExecute(impl_->handle,&in,&out),"Estimate motion");
    }
    catch(...){impl_->reset();throw;}
    impl_->assess_motion();
    if(impl_->trustworthy) {
        impl_->draw(impl_->repairScratchTargets[0].Get(),0,false,0,0.5f,false,0,true);
        impl_->draw(impl_->repairScratchTargets[1].Get(),0,false,1,0.5f,false,0,true);
        // A bounded second pass can use matches recovered by the first pass.
        // Require repeated source detail to avoid propagating through ordinary
        // occlusions. Both directions read the same completed pass; resources
        // are unbound by draw(), and no NVOFA execute follows on this pair.
        // Preserve hardware vectors: their Cost Map must never be attributed
        // to a different vector produced by our repair passes.
        impl_->draw(impl_->repairedTargets[0].Get(),0,false,0,0.0f,false,0,true);
        impl_->draw(impl_->repairedTargets[1].Get(),0,false,1,0.0f,false,0,true);
        impl_->draw(impl_->layerTargets[0].Get(),0,false,0,0.5f,false,1);
        impl_->draw(impl_->layerTargets[1].Get(),0,false,0,0.5f,false,2);
        impl_->draw(impl_->layerTargets[0].Get(),0,false,1,0.5f,false,3);
    }
    impl_->prepared=impl_->history=true;
}
void MotionSynthesizer::render_phase(ID3D11RenderTargetView* y,ID3D11RenderTargetView* uv,float fraction) {
    if(!impl_->prepared)throw std::logic_error("Prepare a pair before synthesis");
    if(!std::isfinite(fraction)||fraction<=0||fraction>=1)throw std::invalid_argument("Phase must be inside the source pair");
    impl_->validate_target(y,0);impl_->validate_target(uv,1);
    impl_->draw(nullptr,0,false,0,fraction,true);
    impl_->draw(y,0,false,0,fraction);impl_->draw(uv,1,false,0,fraction);
}
void MotionSynthesizer::render_midpoint(ID3D11RenderTargetView* y,ID3D11RenderTargetView* uv) {
    render_phase(y,uv,0.5f);
}
}
