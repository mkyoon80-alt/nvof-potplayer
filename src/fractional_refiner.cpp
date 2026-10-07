#include "nvof/fractional_refiner.hpp"
#include <windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <nvOpticalFlowD3D11.h>

namespace nvof {
namespace {
using Microsoft::WRL::ComPtr;
void check(HRESULT value, const char* action) {
    if (FAILED(value)) throw std::runtime_error(std::string(action) +
        " failed, HRESULT=" + std::to_string(static_cast<unsigned long>(value)));
}
const char shader[] = R"(
Texture2D<float> ay:register(t0);
Texture2D<float> by:register(t1);
Texture2D<float2> auv:register(t2);
Texture2D<float2> buv:register(t3);
Texture2D<int2> forwardFlow:register(t4);
Texture2D<int2> backwardFlow:register(t5);
Texture2D<float2> fallbackPlane:register(t6);
Texture2D<uint> protectionMask:register(t7);
RWTexture2D<uint> protectionOutput:register(u0);
Texture2D<float4> appearanceGuide:register(t8);
RWTexture2D<float4> guideOutput:register(u1);
Texture2D<uint> denseMask:register(t9);
RWTexture2D<uint> denseOutput:register(u2);
Texture2D<float> horizontalMask:register(t10);
RWTexture2D<float> horizontalOutput:register(u3);
Texture2D<uint> motionStats:register(t11);
RWTexture2D<uint> motionStatsOutput:register(u4);
SamplerState linearClamp:register(s0);
cbuffer Parameters:register(b0) {
    float4 dimensions; // width, height, grid, phase
    uint4 flags;      // chroma plane, appearance protection, midpoint correction
};
float4 vs(uint vertex:SV_VertexID):SV_Position {
    float2 p=vertex==0?float2(-1,-1):(vertex==1?float2(-1,3):float2(3,-1));
    return float4(p,0,1);
}
float2 flowAt(float2 p, bool backwards) {
    float2 g=p/dimensions.z-0.5;
    int2 i=int2(floor(g));
    float2 f=frac(g);
    int2 hi=int2(ceil(dimensions.xy/dimensions.z))-1;
    int2 p00=clamp(i,int2(0,0),hi),p10=clamp(i+int2(1,0),int2(0,0),hi);
    int2 p01=clamp(i+int2(0,1),int2(0,0),hi),p11=clamp(i+int2(1,1),int2(0,0),hi);
    float2 a,b,c,d;
    if(backwards) {
        a=float2(backwardFlow.Load(int3(p00,0)));b=float2(backwardFlow.Load(int3(p10,0)));
        c=float2(backwardFlow.Load(int3(p01,0)));d=float2(backwardFlow.Load(int3(p11,0)));
    } else {
        a=float2(forwardFlow.Load(int3(p00,0)));b=float2(forwardFlow.Load(int3(p10,0)));
        c=float2(forwardFlow.Load(int3(p01,0)));d=float2(forwardFlow.Load(int3(p11,0)));
    }
    return lerp(lerp(a,b,f.x),lerp(c,d,f.x),f.y)/32.0;
}
bool inside(float2 p) { return all(p>=0.5)&&all(p<=dimensions.xy-0.5); }
float luminance(float2 p,bool next) {
    return next?by.SampleLevel(linearClamp,p/dimensions.xy,0):ay.SampleLevel(linearClamp,p/dimensions.xy,0);
}
float2 color(float2 p,bool next) {
    if(flags.x!=0) return next?buv.SampleLevel(linearClamp,p/dimensions.xy,0):auv.SampleLevel(linearClamp,p/dimensions.xy,0);
    return float2(luminance(p,next),0);
}
bool unchanged(float2 p) {
    int2 center=clamp(int2(floor(p)),int2(1,1),int2(dimensions.xy)-2);
    [unroll]for(int j=-1;j<=1;++j) [unroll]for(int i=-1;i<=1;++i) {
        int3 q=int3(center+int2(i,j),0);
        if(ay.Load(q)!=by.Load(q)) return false;
    }
    int3 uv=int3(clamp(int2(floor(p*0.5)),int2(0,0),int2(dimensions.xy*0.5)-1),0);
    return all(auv.Load(uv)==buv.Load(uv));
}
float2 original(int2 outputPixel,bool next) {
    if(flags.x!=0) return next?buv.Load(int3(outputPixel,0)):auv.Load(int3(outputPixel,0));
    return float2(next?by.Load(int3(outputPixel,0)):ay.Load(int3(outputPixel,0)),0);
}
float4 ps(float4 screen:SV_Position):SV_Target {
    int2 outputPixel=int2(screen.xy);
    float2 fallback=fallbackPlane.Load(int3(outputPixel,0));
    float t=dimensions.w;
    if(t<=0) return float4(original(outputPixel,false),0,1);
    if(t>=1) return float4(original(outputPixel,true),0,1);
    float2 p=screen.xy*(flags.x!=0?2.0:1.0);
    float2 firstFlow=flowAt(p,false),lastFlow=flowAt(p,true);
    if(dot(firstFlow,firstFlow)<0.25&&dot(lastFlow,lastFlow)<0.25&&unchanged(p))
        return float4(original(outputPixel,false),0,1);
    float2 a=p,b=p;
    [unroll]for(int iteration=0;iteration<3;++iteration) {
        a=p-t*flowAt(a,false);
        b=p-(1-t)*flowAt(b,true);
    }
    if(!inside(a)||!inside(b)) return float4(fallback,0,1);
    float2 f=flowAt(a,false),r=flowAt(b,true);
    if(!inside(a+f)||!inside(b+r)) return float4(fallback,0,1);
    float2 errorA=f+flowAt(a+f,true),errorB=r+flowAt(b+r,false);
    float fb=max(length(errorA),length(errorB));
    float motion=max(length(f),length(r));
    float residual=max(length(a+t*f-p),length(b+(1-t)*r-p));
    float correspondence=max(length(a+f-b),length(b+r-a));
    float photo=max(abs(luminance(a,false)-luminance(b,true)),
        max(abs(luminance(a,false)-luminance(a+f,true)),
            abs(luminance(b,true)-luminance(b+r,false))))*255.0;
    float2 chromaDifference=abs(auv.SampleLevel(linearClamp,a/dimensions.xy,0)-
        buv.SampleLevel(linearClamp,b/dimensions.xy,0))*255.0;
    photo=max(photo,max(chromaDifference.x,chromaDifference.y));
    // Conservative confidence derives from geometry and actual image agreement,
    // avoiding an undocumented absolute hardware-cost scale.
    float geometricLimit=0.75+0.02*motion;
    if(fb>geometricLimit||residual>0.5||correspondence>geometricLimit||photo>12)
        return float4(fallback,0,1);
    float confidence=(1-smoothstep(0.25,geometricLimit,fb))*
        (1-smoothstep(0.15,0.5,residual))*(1-smoothstep(4.0,12.0,photo));
    float2 warped=lerp(color(a,false),color(b,true),t);
    return float4(lerp(fallback,warped,confidence),0,1);
}
// Appearance discontinuities have no trustworthy continuous warp. Aggregate
// independent mismatch evidence, then protect a neighborhood of the defect.
groupshared uint changedCount;
groupshared uint invalidCount,minA,maxA,minB,maxB;
[numthreads(4,4,1)]
void csProtection(uint3 id:SV_DispatchThreadID,uint3 group:SV_GroupID,uint index:SV_GroupIndex) {
    if(index==0){invalidCount=0;changedCount=0;minA=minB=255;maxA=maxB=0;}
    GroupMemoryBarrierWithGroupSync();
    if(all(id.xy<uint2(dimensions.xy))) {
        float2 p=float2(id.xy)+0.5,a=p,b=p;
        uint va=uint(round(luminance(p,false)*255)),vb=uint(round(luminance(p,true)*255));
        if(abs(int(va)-int(vb))>20)InterlockedAdd(changedCount,1);
        InterlockedMin(minA,va);InterlockedMax(maxA,va);InterlockedMin(minB,vb);InterlockedMax(maxB,vb);
        [unroll]for(int k=0;k<3;++k){a=p-0.5*flowAt(a,false);b=p-0.5*flowAt(b,true);}
        float2 f=flowAt(a,false),r=flowAt(b,true);
        float fb=max(length(f+flowAt(a+f,true)),length(r+flowAt(b+r,false)));
        float correspondence=max(length(a+f-b),length(b+r-a));
        float difference=abs(luminance(a,false)-luminance(b,true))*255.0;
        if(inside(a)&&inside(b)&&difference>20&&(fb>0.75||correspondence>0.75))
            InterlockedAdd(invalidCount,1);
    }
    GroupMemoryBarrierWithGroupSync();
    // Require a flat endpoint: ordinary textured motion is not a drawing edit.
    if(index==0){
        protectionOutput[group.xy]=invalidCount>=1&&changedCount>=6&&min(maxA-minA,maxB-minB)<=24?1:0;
        float2 p=(float2(group.xy)+0.5)*4.0,f=flowAt(p,false);
        float gradient=abs(luminance(p+float2(2,0),false)-luminance(p-float2(2,0),false))+
                       abs(luminance(p+float2(0,2),false)-luminance(p-float2(0,2),false));
        if(inside(p+f)&&gradient>=0.025&&length(f+flowAt(p+f,true))<=0.6&&
           abs(luminance(p,false)-luminance(p+f,true))*255.0<=6){
            InterlockedAdd(motionStatsOutput[uint2(0,0)],1);
            float speed=length(f);
            if(speed>0.35){
                InterlockedAdd(motionStatsOutput[uint2(1,0)],1);
                uint direction=uint(floor((atan2(f.y,f.x)+3.14159265)*8.0/6.2831853+0.5))%8;
                uint magnitude=uint(clamp(floor(log2(speed/0.35)),0.0,5.0));
                InterlockedAdd(motionStatsOutput[uint2(2+direction*6+magnitude,0)],1);
            }
        }
    }
}
// Four-pixel evidence tiles catch small mouths. Separable dilation keeps the
// 96-pixel protected neighborhood inexpensive, without a large nested search.
[numthreads(8,8,1)]
void csDense(uint3 id:SV_DispatchThreadID) {
    int2 size=int2(ceil(dimensions.xy/4.0));if(any(id.xy>=uint2(size)))return;
    // Require a predominantly still view. A camera pan (or substantial
    // object motion) must never be converted into repeated source contours.
    uint tracked=motionStats.Load(int3(0,0,0)),moving=motionStats.Load(int3(1,0,0));
    uint coherent=0;
    [loop]for(uint direction=0;direction<8;++direction)[unroll]for(uint magnitude=0;magnitude<6;++magnitude){
        uint votes=motionStats.Load(int3(2+direction*6+magnitude,0,0));
        votes+=motionStats.Load(int3(2+((direction+1)%8)*6+magnitude,0,0));
        coherent=max(coherent,votes);
    }
    if(tracked<64||moving*2>tracked||(moving*50>tracked&&coherent*5>=moving*4)){denseOutput[id.xy]=0;return;}
    uint dense=0;
    [unroll]for(int y=-2;y<=2;++y)[unroll]for(int x=-2;x<=2;++x) {
        int2 q=int2(id.xy)+int2(x,y);
        if(all(q>=0)&&all(q<size))dense+=protectionMask.Load(int3(q,0));
    }
    if(dense<12){denseOutput[id.xy]=0;return;}
    denseOutput[id.xy]=1;
}
[numthreads(8,8,1)]
void csDilateX(uint3 id:SV_DispatchThreadID) {
    int2 size=int2(ceil(dimensions.xy/4.0));if(any(id.xy>=uint2(size)))return;
    float protection=0;
    [loop]for(int x=-24;x<=24;++x) {
        int2 q=int2(id.xy)+int2(x,0);
        if(all(q>=0)&&all(q<size)&&denseMask.Load(int3(q,0))!=0)
            protection=max(protection,saturate((24.5-abs(x))/4.0));
    }
    horizontalOutput[id.xy]=protection;
}
[numthreads(8,8,1)]
void csGuide(uint3 id:SV_DispatchThreadID) {
    int2 size=int2(ceil(dimensions.xy/4.0));
    if(any(id.xy>=uint2(size)))return;
    float protection=0;
    [loop]for(int y=-24;y<=24;++y) {
        int2 q=int2(id.xy)+int2(0,y);
        if(all(q>=0)&&all(q<size))protection=max(protection,
            min(horizontalMask.Load(int3(q,0)),saturate((24.5-abs(y))/4.0)));
    }
    if(protection==0){guideOutput[id.xy]=0;return;}
    float2 p=(float2(id.xy)+0.5)*4.0,velocity=0;float weights=0;float squaredMotion=0;
    // Infer rigid local motion from nearby features that survive the edit.
    // Flat patches are not motion evidence; avoid using their arbitrary flow.
    [loop]for(int y=-4;y<=4;++y)[loop]for(int x=-4;x<=4;++x) {
        float2 delta=float2(x,y)*16.0,q=p+delta;
        float2 f=flowAt(q,false);
        if(!inside(q)||!inside(q+f)||length(f)>4)continue;
        float fb=length(f+flowAt(q+f,true));
        float photo=0;
        [unroll]for(int j=-1;j<=1;++j)[unroll]for(int i=-1;i<=1;++i) {
            float2 d=float2(i,j)*2;
            photo=max(photo,abs(luminance(q+d,false)-luminance(q+f+d,true))*255.0);
        }
        float gradient=abs(luminance(q+float2(1,0),false)-luminance(q-float2(1,0),false))+
                       abs(luminance(q+float2(0,1),false)-luminance(q-float2(0,1),false));
        if(fb>0.35||photo>6||gradient<0.025)continue;
        float weight=min(gradient,0.25)/(1+dot(delta,delta)*0.004);
        velocity+=f*weight;squaredMotion+=dot(f,f)*weight;weights+=weight;
    }

    if(weights>0){velocity/=weights;if(squaredMotion/weights-dot(velocity,velocity)>0.25)velocity=0;}
    guideOutput[id.xy]=float4(velocity*protection,0,protection);
}
// Catmull-Rom reconstruction avoids the extra blur of linear sampling at
// half-pixel motion. Clamp to the sampled footprint to bound ringing and
// preserve the source's stored Y/UV range (there is no RGB conversion).
float4 cubicWeights(float t) {
    float t2=t*t,t3=t2*t;
    return float4(-0.5*t+t2-0.5*t3,1-2.5*t2+1.5*t3,
                  0.5*t+2*t2-1.5*t3,-0.5*t2+0.5*t3);
}
float2 stableColor(float2 p,bool next) {
    float scale=flags.x!=0?2.0:1.0;
    float2 q=p/scale-0.5;int2 i=int2(floor(q));float2 f=frac(q);
    float4 wx=cubicWeights(f.x),wy=cubicWeights(f.y);
    int2 hi=int2(dimensions.xy/scale)-1;
    float2 value=0,lo=1,upper=0;
    [unroll]for(int y=0;y<4;++y)[unroll]for(int x=0;x<4;++x) {
        int2 at=clamp(i+int2(x-1,y-1),int2(0,0),hi);
        float2 v=original(at,next);value+=v*wx[x]*wy[y];
        lo=min(lo,v);upper=max(upper,v);
    }
    return clamp(value,lo,upper);
}
// A different path from ps: exactly one midpoint, only bounded and
// bidirectionally consistent motion. Never mix an unwarped FRUC picture with
// a corrected picture: accepted pixels use two aligned source samples.
float4 slowMidpoint(float4 screen) {
    int2 pixel=int2(screen.xy);
    float2 fallback=fallbackPlane.Load(int3(pixel,0));
    float2 p=screen.xy*(flags.x!=0?2.0:1.0);
    float2 a=p,b=p;
    [unroll]for(int iteration=0;iteration<3;++iteration) {
        a=p-0.5*flowAt(a,false);
        b=p-0.5*flowAt(b,true);
    }
    float2 f=flowAt(a,false),r=flowAt(b,true);
    if(!inside(a)||!inside(b)||!inside(a+f)||!inside(b+r)||
       max(length(f),length(r))>16.0) return float4(fallback,0,1);
    float fb=max(length(f+flowAt(a+f,true)),length(r+flowAt(b+r,false)));
    float correspondence=max(length(a+f-b),length(b+r-a));
    // A center-only check can accept alternating fine stripes while erasing
    // their dark centers. Require agreement throughout a 3x3 luma patch.
    float photo=0;
    [unroll]for(int j=-1;j<=1;++j)[unroll]for(int i=-1;i<=1;++i) {
        float2 delta=float2(i,j);
        photo=max(photo,abs(luminance(a+delta,false)-luminance(b+delta,true))*255.0);
    }
    float2 chroma=abs(auv.SampleLevel(linearClamp,a/dimensions.xy,0)-
        buv.SampleLevel(linearClamp,b/dimensions.xy,0))*255.0;
    if(fb>0.6||correspondence>0.6||photo>12||max(chroma.x,chroma.y)>8)
        return float4(fallback,0,1);
    return float4(0.5*(stableColor(a,false)+stableColor(b,true)),0,1);
}
float4 psSlow(float4 screen:SV_Position):SV_Target {
    float4 regular=float4(fallbackPlane.Load(int3(int2(screen.xy),0)),0,1);
    if(flags.z!=0)regular=slowMidpoint(screen);
    if(flags.y==0)return regular;
    float2 p=screen.xy*(flags.x!=0?2.0:1.0);
    float4 guide=appearanceGuide.SampleLevel(linearClamp,p/(ceil(dimensions.xy/4.0)*4.0),0);
    if(guide.w<=0)return regular;
    float2 a=p-0.5*guide.xy/max(guide.w,0.0001);
    float2 held=stableColor(a,false);
    // The protected core includes surrounding contours; feather only its
    // outer margin, away from the abruptly changing drawing.
    float strength=saturate(guide.w);
    return float4(lerp(regular.xy,held,strength),0,1);
}

)";
ComPtr<ID3DBlob> compile(const char* entry,const char* target) {
    ComPtr<ID3DBlob> code,errors;
    HRESULT result=D3DCompile(shader,sizeof(shader)-1,"nvof-fractional-refiner",
        nullptr,nullptr,entry,target,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
    if(FAILED(result)) throw std::runtime_error(std::string("Fractional shader compilation: ")+
        (errors?static_cast<const char*>(errors->GetBufferPointer()):"unknown failure"));
    return code;
}
}

struct FractionalRefiner::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    HMODULE module=nullptr;
    NV_OF_D3D11_API_FUNCTION_LIST api{};
    NvOFHandle handle=nullptr;
    std::array<ComPtr<ID3D11Texture2D>,2> inputs,flows;
    std::array<NvOFGPUBufferHandle,2> inputHandles{},flowHandles{};
    std::array<ComPtr<ID3D11ShaderResourceView>,2> yViews,uvViews,flowViews;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel,slowPixel;
    ComPtr<ID3D11ComputeShader> protectionShader,denseShader,dilateShader,guideShader;
    ComPtr<ID3D11Texture2D> protectionTexture,denseTexture,horizontalTexture,guideTexture,motionStatsTexture;
    ComPtr<ID3D11ShaderResourceView> protectionView,denseView,horizontalView,guideView,motionStatsView;
    ComPtr<ID3D11UnorderedAccessView> protectionUav,denseUav,horizontalUav,guideUav,motionStatsUav;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> rasterizer;
    int width=0,height=0;
    uint32_t grid=0;
    bool prepared=false,hasHistory=false;
    bool appearanceProtection=true;
    bool midpointCorrection=true;

    Impl(ID3D11Device* d,ID3D11DeviceContext* c,bool protect,bool stabilize):device(d),context(c),appearanceProtection(protect),midpointCorrection(stabilize) {
        if(!d||!c) throw std::invalid_argument("Fractional refiner requires a D3D11 device and context");
        ComPtr<ID3D11Device> owner;c->GetDevice(&owner);
        if(owner.Get()!=d||c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)
            throw std::invalid_argument("Fractional refiner requires the device's immediate context");
        try {
            module=LoadLibraryExW(L"nvofapi64.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
            if(!module) throw std::runtime_error("NVIDIA optical-flow driver DLL is unavailable in System32");
            using Create=NV_OF_STATUS(NVOFAPI*)(uint32_t,NV_OF_D3D11_API_FUNCTION_LIST*);
            auto create=reinterpret_cast<Create>(GetProcAddress(module,"NvOFAPICreateInstanceD3D11"));
            if(!create) throw std::runtime_error("NVIDIA D3D11 optical-flow entry point unavailable");
            of(create(NV_OF_API_VERSION,&api),"Create optical-flow API");
            if(!api.nvCreateOpticalFlowD3D11||!api.nvOFGetCaps||!api.nvOFInit||
               !api.nvOFExecute||!api.nvOFDestroy||!api.nvOFRegisterResourceD3D11||
               !api.nvOFUnregisterResourceD3D11||!api.nvOFGetSurfaceFormatCountD3D11||
               !api.nvOFGetSurfaceFormatD3D11) throw std::runtime_error("Incomplete optical-flow driver API");
            auto vs=compile("vs","vs_5_0"),ps=compile("ps","ps_5_0");
            check(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex),"Create fractional vertex shader");
            check(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&pixel),"Create fractional pixel shader");
            auto cs=compile("csProtection","cs_5_0");
            check(device->CreateComputeShader(cs->GetBufferPointer(),cs->GetBufferSize(),nullptr,&protectionShader),"Create appearance protection shader");
            auto dense=compile("csDense","cs_5_0"),dilate=compile("csDilateX","cs_5_0");
            check(device->CreateComputeShader(dense->GetBufferPointer(),dense->GetBufferSize(),nullptr,&denseShader),"Create appearance density shader");
            check(device->CreateComputeShader(dilate->GetBufferPointer(),dilate->GetBufferSize(),nullptr,&dilateShader),"Create appearance dilation shader");
            auto guide=compile("csGuide","cs_5_0");
            check(device->CreateComputeShader(guide->GetBufferPointer(),guide->GetBufferSize(),nullptr,&guideShader),"Create appearance guide shader");
            auto slow=compile("psSlow","ps_5_0");
            check(device->CreatePixelShader(slow->GetBufferPointer(),slow->GetBufferSize(),nullptr,&slowPixel),"Create slow midpoint shader");
            D3D11_BUFFER_DESC desc{};desc.ByteWidth=32;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            check(device->CreateBuffer(&desc,nullptr,&constants),"Create fractional parameters");
            D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
            check(device->CreateSamplerState(&sd,&sampler),"Create fractional sampler");
            D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
            check(device->CreateRasterizerState(&rd,&rasterizer),"Create fractional rasterizer");
        } catch(...) {reset();if(module){FreeLibrary(module);module=nullptr;}throw;}
    }
    ~Impl(){reset();if(module)FreeLibrary(module);}
    void of(NV_OF_STATUS status,const char* action) {
        if(status==NV_OF_SUCCESS)return;
        char detail[256]{};uint32_t size=sizeof(detail);
        if(handle&&api.nvOFGetLastError)api.nvOFGetLastError(handle,detail,&size);
        detail[sizeof(detail)-1]=0;
        throw std::runtime_error(std::string(action)+" failed, NVOF="+std::to_string(status)+" "+detail);
    }
    void reset() noexcept {
        prepared=false;hasHistory=false;
        if(handle) {
            for(auto& h:flowHandles)if(h){api.nvOFUnregisterResourceD3D11(h);h=nullptr;}
            for(auto& h:inputHandles)if(h){api.nvOFUnregisterResourceD3D11(h);h=nullptr;}
            api.nvOFDestroy(handle);handle=nullptr;
        }
        for(auto& v:flowViews)v.Reset();
        for(auto& v:yViews)v.Reset();
        for(auto& v:uvViews)v.Reset();
        for(auto& t:flows)t.Reset();
        for(auto& t:inputs)t.Reset();
        motionStatsUav.Reset();motionStatsView.Reset();motionStatsTexture.Reset();
        denseUav.Reset();denseView.Reset();denseTexture.Reset();
        horizontalUav.Reset();horizontalView.Reset();horizontalTexture.Reset();
        guideUav.Reset();guideView.Reset();guideTexture.Reset();
        protectionUav.Reset();protectionView.Reset();protectionTexture.Reset();
        width=height=0;grid=0;
    }
    bool supports(NV_OF_BUFFER_USAGE usage,DXGI_FORMAT format) {
        uint32_t count=0;of(api.nvOFGetSurfaceFormatCountD3D11(handle,usage,NV_OF_MODE_OPTICALFLOW,&count),"Query optical-flow format count");
        if(!count||count>256)throw std::runtime_error("Invalid optical-flow format count");
        std::vector<DXGI_FORMAT> formats(count);
        of(api.nvOFGetSurfaceFormatD3D11(handle,usage,NV_OF_MODE_OPTICALFLOW,formats.data()),"Query optical-flow formats");
        return std::find(formats.begin(),formats.end(),format)!=formats.end();
    }
    ComPtr<ID3D11Texture2D> texture(DXGI_FORMAT format,UINT w,UINT h) {
        D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.MipLevels=1;desc.ArraySize=1;
        desc.Format=format;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;
        desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        if(format!=DXGI_FORMAT_NV12)desc.BindFlags|=D3D11_BIND_UNORDERED_ACCESS;
        ComPtr<ID3D11Texture2D> result;check(device->CreateTexture2D(&desc,nullptr,&result),"Create fractional texture");return result;
    }
    ComPtr<ID3D11ShaderResourceView> view(ID3D11Texture2D* texture,DXGI_FORMAT format) {
        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};desc.Format=format;desc.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MipLevels=1;ComPtr<ID3D11ShaderResourceView> result;
        check(device->CreateShaderResourceView(texture,&desc,&result),"Create fractional texture view");return result;
    }
    void configure(int w,int h) {
        if(handle&&width==w&&height==h)return;
        reset();
        try {
            of(api.nvCreateOpticalFlowD3D11(device.Get(),context.Get(),&handle),"Create D3D11 optical flow");
            uint32_t count=0;
            of(api.nvOFGetCaps(handle,NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES,nullptr,&count),"Query flow grids");
            if(!count||count>32)throw std::runtime_error("Invalid optical-flow grid count");
            std::vector<uint32_t> grids(count);
            of(api.nvOFGetCaps(handle,NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES,grids.data(),&count),"Read flow grids");
            for(uint32_t candidate:{2u,1u,4u})if(std::find(grids.begin(),grids.end(),candidate)!=grids.end()){grid=candidate;break;}
            if(!grid||!supports(NV_OF_BUFFER_USAGE_INPUT,DXGI_FORMAT_NV12)||
               !supports(NV_OF_BUFFER_USAGE_OUTPUT,DXGI_FORMAT_R16G16_SINT))
                throw std::runtime_error("Native NV12 optical flow is unavailable");
            NV_OF_INIT_PARAMS init{};init.width=w;init.height=h;
            init.outGridSize=static_cast<NV_OF_OUTPUT_VECTOR_GRID_SIZE>(grid);
            init.mode=NV_OF_MODE_OPTICALFLOW;init.perfLevel=NV_OF_PERF_LEVEL_MEDIUM;
            init.predDirection=NV_OF_PRED_DIRECTION_BOTH;init.inputBufferFormat=NV_OF_BUFFER_FORMAT_NV12;
            of(api.nvOFInit(handle,&init),"Initialize bidirectional flow");
            for(size_t i=0;i<2;++i) {
                inputs[i]=texture(DXGI_FORMAT_NV12,w,h);
                flows[i]=texture(DXGI_FORMAT_R16G16_SINT,(w+grid-1)/grid,(h+grid-1)/grid);
                yViews[i]=view(inputs[i].Get(),DXGI_FORMAT_R8_UNORM);
                uvViews[i]=view(inputs[i].Get(),DXGI_FORMAT_R8G8_UNORM);
                flowViews[i]=view(flows[i].Get(),DXGI_FORMAT_R16G16_SINT);
                of(api.nvOFRegisterResourceD3D11(handle,inputs[i].Get(),&inputHandles[i]),"Register fractional input");
                of(api.nvOFRegisterResourceD3D11(handle,flows[i].Get(),&flowHandles[i]),"Register fractional flow");
            }
            protectionTexture=texture(DXGI_FORMAT_R32_UINT,(w+3)/4,(h+3)/4);
            protectionView=view(protectionTexture.Get(),DXGI_FORMAT_R32_UINT);
            check(device->CreateUnorderedAccessView(protectionTexture.Get(),nullptr,&protectionUav),"Create appearance protection UAV");
            guideTexture=texture(DXGI_FORMAT_R32G32B32A32_FLOAT,(w+3)/4,(h+3)/4);
            guideView=view(guideTexture.Get(),DXGI_FORMAT_R32G32B32A32_FLOAT);
            check(device->CreateUnorderedAccessView(guideTexture.Get(),nullptr,&guideUav),"Create appearance guide UAV");
            denseTexture=texture(DXGI_FORMAT_R32_UINT,(w+3)/4,(h+3)/4);denseView=view(denseTexture.Get(),DXGI_FORMAT_R32_UINT);
            check(device->CreateUnorderedAccessView(denseTexture.Get(),nullptr,&denseUav),"Create density UAV");
            horizontalTexture=texture(DXGI_FORMAT_R32_FLOAT,(w+3)/4,(h+3)/4);horizontalView=view(horizontalTexture.Get(),DXGI_FORMAT_R32_FLOAT);
            check(device->CreateUnorderedAccessView(horizontalTexture.Get(),nullptr,&horizontalUav),"Create dilation UAV");
            motionStatsTexture=texture(DXGI_FORMAT_R32_UINT,50,1);motionStatsView=view(motionStatsTexture.Get(),DXGI_FORMAT_R32_UINT);
            check(device->CreateUnorderedAccessView(motionStatsTexture.Get(),nullptr,&motionStatsUav),"Create motion statistics UAV");
            width=w;height=h;
        } catch(...) {reset();throw;}
    }
    void validate(ID3D11Texture2D* texture,int w,int h) {
        if(!texture||w<4||h<4||w>8192||h>8192||(w&1)||(h&1))
            throw std::invalid_argument("Fractional inputs require even NV12 dimensions from 4 through 8192");
        D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);ComPtr<ID3D11Device> owner;texture->GetDevice(&owner);
        if(owner.Get()!=device.Get()||desc.Format!=DXGI_FORMAT_NV12||desc.Width<UINT(w)||desc.Height<UINT(h)||
           desc.ArraySize!=1||desc.MipLevels!=1||desc.SampleDesc.Count!=1)
            throw std::invalid_argument("Fractional input must be a single-slice NV12 texture on this device");
    }
    void checkViews(ID3D11RenderTargetView* target,ID3D11ShaderResourceView* fallback,DXGI_FORMAT format) {
        if(!target||!fallback)throw std::invalid_argument("Fractional render requires both target and fallback views");
        D3D11_RENDER_TARGET_VIEW_DESC td{};target->GetDesc(&td);
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};fallback->GetDesc(&sd);
        if(td.Format!=format||sd.Format!=format||td.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D||
           sd.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D||sd.Texture2D.MostDetailedMip!=0)
            throw std::invalid_argument("Fractional view formats must be native NV12 plane views");
        ComPtr<ID3D11Resource> tr,fr;target->GetResource(&tr);fallback->GetResource(&fr);
        if(tr.Get()==fr.Get())throw std::invalid_argument("Fractional fallback and output cannot alias");
        ComPtr<ID3D11Device> tdvc,fdvc;target->GetDevice(&tdvc);fallback->GetDevice(&fdvc);
        if(tdvc.Get()!=device.Get()||fdvc.Get()!=device.Get())
            throw std::invalid_argument("Fractional views belong to another device");
    }
    void classify_appearance() {
        struct Parameters {float dimensions[4];uint32_t flags[4];};
        const Parameters params{{float(width),float(height),float(grid),0.5f},{0,0,0,0}};
        context->UpdateSubresource(constants.Get(),0,nullptr,&params,0,0);
        ID3D11ShaderResourceView* inputs[]={yViews[0].Get(),yViews[1].Get(),uvViews[0].Get(),uvViews[1].Get(),flowViews[0].Get(),flowViews[1].Get()};
        ID3D11Buffer* cb=constants.Get();ID3D11SamplerState* sp=sampler.Get();ID3D11UnorderedAccessView* target=protectionUav.Get();
        context->CSSetShader(protectionShader.Get(),nullptr,0);context->CSSetConstantBuffers(0,1,&cb);context->CSSetSamplers(0,1,&sp);
        context->CSSetShaderResources(0,6,inputs);context->CSSetUnorderedAccessViews(0,1,&target,nullptr);
        UINT zeros[4]{};context->ClearUnorderedAccessViewUint(motionStatsUav.Get(),zeros);
        auto stats=motionStatsUav.Get();context->CSSetUnorderedAccessViews(4,1,&stats,nullptr);
        context->Dispatch((width+3)/4,(height+3)/4,1);
        ID3D11ShaderResourceView* empty[12]{};ID3D11UnorderedAccessView* none=nullptr;
        context->CSSetShaderResources(0,6,empty);context->CSSetUnorderedAccessViews(0,1,&none,nullptr);
        context->CSSetUnorderedAccessViews(4,1,&none,nullptr);
        auto statsView=motionStatsView.Get();context->CSSetShaderResources(11,1,&statsView);
        const auto dispatch=[&](ID3D11ComputeShader* cs,UINT inputSlot,ID3D11ShaderResourceView* input,UINT outputSlot,ID3D11UnorderedAccessView* output){
            context->CSSetShader(cs,nullptr,0);context->CSSetShaderResources(inputSlot,1,&input);context->CSSetUnorderedAccessViews(outputSlot,1,&output,nullptr);
            context->Dispatch((width+31)/32,(height+31)/32,1);
            context->CSSetShaderResources(inputSlot,1,empty);context->CSSetUnorderedAccessViews(outputSlot,1,&none,nullptr);
        };
        context->CSSetShaderResources(0,6,inputs);
        dispatch(denseShader.Get(),7,protectionView.Get(),2,denseUav.Get());
        dispatch(dilateShader.Get(),9,denseView.Get(),3,horizontalUav.Get());
        context->CSSetShaderResources(0,6,inputs);
        dispatch(guideShader.Get(),10,horizontalView.Get(),1,guideUav.Get());
        context->CSSetShaderResources(0,12,empty);context->CSSetShader(nullptr,nullptr,0);
    }
    void draw(float t,unsigned plane,ID3D11RenderTargetView* target,ID3D11ShaderResourceView* fallback,bool slow=false) {
        struct Parameters {float dimensions[4];uint32_t flags[4];};
        const Parameters parameters{{float(width),float(height),float(grid),t},{plane,appearanceProtection?1u:0u,midpointCorrection?1u:0u,0}};
        context->UpdateSubresource(constants.Get(),0,nullptr,&parameters,0,0);
        ID3D11ShaderResourceView* views[]={yViews[0].Get(),yViews[1].Get(),uvViews[0].Get(),uvViews[1].Get(),flowViews[0].Get(),flowViews[1].Get(),fallback,protectionView.Get(),guideView.Get()};
        ID3D11Buffer* cb=constants.Get();ID3D11SamplerState* sp=sampler.Get();
        context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex.Get(),nullptr,0);context->HSSetShader(nullptr,nullptr,0);
        context->DSSetShader(nullptr,nullptr,0);context->GSSetShader(nullptr,nullptr,0);
        context->PSSetShader(slow?slowPixel.Get():pixel.Get(),nullptr,0);context->PSSetConstantBuffers(0,1,&cb);
        context->PSSetSamplers(0,1,&sp);context->RSSetState(rasterizer.Get());
        context->OMSetBlendState(nullptr,nullptr,0xffffffff);context->OMSetDepthStencilState(nullptr,0);
        const D3D11_VIEWPORT viewport{0,0,float(plane?width/2:width),float(plane?height/2:height),0,1};
        context->RSSetViewports(1,&viewport);context->OMSetRenderTargets(1,&target,nullptr);
        context->PSSetShaderResources(0,9,views);context->Draw(3,0);
        // Both planes belong to one NV12 resource; unbind between plane draws.
        ID3D11ShaderResourceView* empty[9]{};context->PSSetShaderResources(0,9,empty);
        context->OMSetRenderTargets(0,nullptr,nullptr);
    }
};

FractionalRefiner::FractionalRefiner(ID3D11Device* d,ID3D11DeviceContext* c,bool protect,bool stabilize):impl_(std::make_unique<Impl>(d,c,protect,stabilize)){}
FractionalRefiner::~FractionalRefiner()=default;
void FractionalRefiner::reset() noexcept {impl_->reset();}
void FractionalRefiner::invalidate_history() noexcept {impl_->hasHistory=false;impl_->prepared=false;}
bool FractionalRefiner::ready() const noexcept {return impl_->prepared;}
uint32_t FractionalRefiner::grid_size() const noexcept {return impl_->grid;}
std::array<uint32_t,8> FractionalRefiner::diagnostic_motion_evidence() const {
    if(!impl_->prepared||!impl_->appearanceProtection)return {};
    D3D11_TEXTURE2D_DESC d{};impl_->motionStatsTexture->GetDesc(&d);
    d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;check(impl_->device->CreateTexture2D(&d,nullptr,&staging),"Diagnostic count texture");
    impl_->context->CopyResource(staging.Get(),impl_->motionStatsTexture.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
    check(impl_->context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Read diagnostic motion counts");
    std::array<uint32_t,50> bins{};std::memcpy(bins.data(),mapped.pData,sizeof(bins));impl_->context->Unmap(staging.Get(),0);
    std::array<uint32_t,8> result{};result[0]=bins[0];result[1]=bins[1];for(unsigned d=0;d<8;++d)for(unsigned m=0;m<6;++m)result[2+m]=(std::max)(result[2+m],bins[2+d*6+m]+bins[2+((d+1)%8)*6+m]);
    return result;
}
void FractionalRefiner::prepare(ID3D11Texture2D* a,ID3D11Texture2D* b,int w,int h) {
    impl_->prepared=false;impl_->validate(a,w,h);impl_->validate(b,w,h);impl_->configure(w,h);
    D3D11_BOX box{0,0,0,UINT(w),UINT(h),1};
    impl_->context->CopySubresourceRegion(impl_->inputs[0].Get(),0,0,0,0,a,0,&box);
    impl_->context->CopySubresourceRegion(impl_->inputs[1].Get(),0,0,0,0,b,0,&box);
    NV_OF_EXECUTE_INPUT_PARAMS input{};input.inputFrame=impl_->inputHandles[0];input.referenceFrame=impl_->inputHandles[1];
    input.disableTemporalHints=impl_->hasHistory?NV_OF_FALSE:NV_OF_TRUE;
    NV_OF_EXECUTE_OUTPUT_PARAMS output{};output.outputBuffer=impl_->flowHandles[0];output.bwdOutputBuffer=impl_->flowHandles[1];
    try {impl_->of(impl_->api.nvOFExecute(impl_->handle,&input,&output),"Estimate fractional motion");}
    catch(...) {impl_->reset();throw;}
    if(impl_->appearanceProtection)impl_->classify_appearance();
    impl_->hasHistory=true;impl_->prepared=true;
}
void FractionalRefiner::render(float t,ID3D11RenderTargetView* y,ID3D11RenderTargetView* uv,
                              ID3D11ShaderResourceView* fallbackY,ID3D11ShaderResourceView* fallbackUV) {
    if(!impl_->prepared)throw std::logic_error("Prepare a source pair before fractional rendering");
    if(!std::isfinite(t)||t<0||t>1)throw std::invalid_argument("Fractional phase must be in [0,1]");
    impl_->checkViews(y,fallbackY,DXGI_FORMAT_R8_UNORM);
    impl_->checkViews(uv,fallbackUV,DXGI_FORMAT_R8G8_UNORM);
    impl_->draw(t,0,y,fallbackY);impl_->draw(t,1,uv,fallbackUV);
}
void FractionalRefiner::render_midpoint(ID3D11RenderTargetView* y,ID3D11RenderTargetView* uv,
                                      ID3D11ShaderResourceView* fy,ID3D11ShaderResourceView* fuv) {
    if(!impl_->prepared)throw std::logic_error("Prepare a source pair before midpoint rendering");
    impl_->checkViews(y,fy,DXGI_FORMAT_R8_UNORM);impl_->checkViews(uv,fuv,DXGI_FORMAT_R8G8_UNORM);
    impl_->draw(0.5f,0,y,fy,true);impl_->draw(0.5f,1,uv,fuv,true);
}

}
