// Hardware integration + deterministic shader oracle for cost provenance.
#define wmain original_gpu_smoke_entry
#include "gpu_engine_smoke.cpp"
#undef wmain
#include "nvof/motion_synthesizer.hpp"
#include "motion_shaders.hpp"
int wmain(){try {
    ComPtr<IDXGIFactory1> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI");
    ComPtr<IDXGIAdapter1> adapter;
    for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> a;if(factory->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 d{};a->GetDesc1(&d);if(d.VendorId==0x10de){adapter=a;break;}}
    require(bool(adapter),"NVIDIA missing");ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"D3D11");
    auto texture=[&](DXGI_FORMAT fmt,int w,int h,int pitch,const void* pixels,UINT bind) {
        D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=1;d.Format=fmt;d.SampleDesc.Count=1;d.BindFlags=bind;
        D3D11_SUBRESOURCE_DATA data{pixels,UINT(pitch),0};ComPtr<ID3D11Texture2D> t;check(device->CreateTexture2D(&d,pixels?&data:nullptr,&t),"Texture");return t;
    };
    auto aBytes=moving_color(640,360,0,false),bBytes=moving_color(640,360,12,false);
    auto a=texture(DXGI_FORMAT_NV12,640,360,640,aBytes.data(),D3D11_BIND_SHADER_RESOURCE);
    auto b=texture(DXGI_FORMAT_NV12,640,360,640,bBytes.data(),D3D11_BIND_SHADER_RESOURCE);
    MotionSynthesizer actual(device.Get(),context.Get());actual.prepare(a.Get(),b.Get(),640,360);
    require(actual.cost_map_active()&&actual.reliable(),"Hardware 8-bit bidirectional cost path inactive");
    actual.reset();require(!actual.cost_map_active(),"Reset retained cost state");
    actual.prepare(a.Get(),b.Get(),640,360);require(actual.cost_map_active(),"Cost path lost after reset");actual.reset();
    std::cout<<"PASS real NVOFA 8-bit bidirectional cost execution and reset"<<std::endl;

    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
    check(device->CreateVertexShader(shaders::motion::vs,sizeof(shaders::motion::vs),nullptr,&vs),"VS");
    check(device->CreatePixelShader(shaders::motion::inverseMapBlend,sizeof(shaders::motion::inverseMapBlend),nullptr,&ps),"PS");
    D3D11_BUFFER_DESC cd{};cd.ByteWidth=32;cd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    float constants[]={8,8,8,8,4,0,0,.5f};D3D11_SUBRESOURCE_DATA cdata{constants,0,0};ComPtr<ID3D11Buffer> cb;
    check(device->CreateBuffer(&cd,&cdata,&cb),"Constants");
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sp;check(device->CreateSamplerState(&sd,&sp),"Sampler");
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;ComPtr<ID3D11RasterizerState> rs;
    check(device->CreateRasterizerState(&rd,&rs),"Raster");
    std::array<ComPtr<ID3D11Texture2D>,13> input;std::array<ComPtr<ID3D11ShaderResourceView>,13> views;
    float gray[64];std::fill(std::begin(gray),std::end(gray),.5f);float uv[128];std::fill(std::begin(uv),std::end(uv),.5f);
    int16_t zero[8]{};uint8_t cost[4]{};
    for(int slot: {0,1,2,3,4,5,9,10,11,12}) {
        if(slot<2)input[slot]=texture(DXGI_FORMAT_R32_FLOAT,8,8,32,gray,D3D11_BIND_SHADER_RESOURCE);
        else if(slot<4)input[slot]=texture(DXGI_FORMAT_R32G32_FLOAT,8,8,64,uv,D3D11_BIND_SHADER_RESOURCE);
        else if(slot==9||slot==10)input[slot]=texture(DXGI_FORMAT_R8_UINT,2,2,2,cost,D3D11_BIND_SHADER_RESOURCE);
        else input[slot]=texture(DXGI_FORMAT_R16G16_SINT,2,2,8,zero,D3D11_BIND_SHADER_RESOURCE);
        check(device->CreateShaderResourceView(input[slot].Get(),nullptr,&views[slot]),"SRV");
    }
    auto offsets=texture(DXGI_FORMAT_R32G32B32A32_FLOAT,8,8,0,nullptr,D3D11_BIND_RENDER_TARGET);
    auto weights=texture(DXGI_FORMAT_R32G32B32A32_FLOAT,8,8,0,nullptr,D3D11_BIND_RENDER_TARGET);
    ComPtr<ID3D11RenderTargetView> rt0,rt1;check(device->CreateRenderTargetView(offsets.Get(),nullptr,&rt0),"RTV");check(device->CreateRenderTargetView(weights.Get(),nullptr,&rt1),"RTV");
    D3D11_TEXTURE2D_DESC td{};weights->GetDesc(&td);td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> staging;
    check(device->CreateTexture2D(&td,nullptr,&staging),"Readback");
    auto render=[&](bool enabled) {
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);
        ID3D11ShaderResourceView* raw[13]{};for(int i=0;i<13;++i)raw[i]=views[i].Get();if(!enabled)raw[9]=raw[10]=nullptr;
        auto c=cb.Get();auto s=sp.Get();context->PSSetConstantBuffers(0,1,&c);context->PSSetSamplers(0,1,&s);context->PSSetShaderResources(0,13,raw);
        D3D11_VIEWPORT vp{0,0,8,8,0,1};context->RSSetViewports(1,&vp);context->RSSetState(rs.Get());ID3D11RenderTargetView* rt[]={rt0.Get(),rt1.Get()};context->OMSetRenderTargets(2,rt,nullptr);context->Draw(3,0);
        ID3D11ShaderResourceView* empty[13]{};context->PSSetShaderResources(0,13,empty);context->OMSetRenderTargets(0,nullptr,nullptr);
        context->CopyResource(staging.Get(),weights.Get());D3D11_MAPPED_SUBRESOURCE map{};check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map),"Readback");
        std::array<float,256> output{};for(int y=0;y<8;++y)memcpy(output.data()+y*32,static_cast<uint8_t*>(map.pData)+y*map.RowPitch,128);context->Unmap(staging.Get(),0);return output;
    };
    auto base=render(true);uint8_t high[]={255,255,255,255};context->UpdateSubresource(input[9].Get(),0,nullptr,high,2,0);auto penalized=render(true);auto absent=render(false);
    for(int i=0;i<64;++i) {
        require(base[4*i+2]==1&&base[4*i+3]==1,"Zero cost is not neutral");
        require(penalized[4*i+2]==.25f&&penalized[4*i+3]==1,"Cost direction or range wrong");
        require(base[4*i]==penalized[4*i]&&base[4*i+1]==penalized[4*i+1],"Cost changed geometric evidence");
        require(absent[4*i+2]==1&&absent[4*i+3]==1,"Absent map changed baseline weights");
    }
    int16_t corrected[]={32,0,32,0,32,0,32,0};context->UpdateSubresource(input[4].Get(),0,nullptr,corrected,8,0);auto repaired=render(true);
    for(int i=0;i<64;++i)require(repaired[4*i+2]==1,"Hardware cost incorrectly assigned to repaired vector");
    context->UpdateSubresource(input[4].Get(),0,nullptr,zero,8,0);uint8_t mixed[]={0,255,255,255};context->UpdateSubresource(input[9].Get(),0,nullptr,mixed,2,0);auto filtered=render(true);
    // At p=(3.5,3.5), the top-left grid weight is (1-.375)^2.
    require(std::abs(filtered[(3*8+3)*4+2]-(.25f+.75f*.625f*.625f))<1e-6f,"Cost sampling is not aligned and bilinear");
    std::cout<<"PASS zero/absent cost, forward/backward separation, bounded cost, unchanged geometry, repaired-vector provenance, bilinear grid alignment"<<std::endl;

    // Perfect correspondence is not weakened by unrelated hardware cost.
    ps.Reset();
    check(device->CreatePixelShader(shaders::motion::inverseMap,sizeof(shaders::motion::inverseMap),nullptr,&ps),"Fusion PS");
    context->UpdateSubresource(input[9].Get(),0,nullptr,high,2,0);
    auto exact=render(true);
    for(int i=0;i<64;++i)require(std::abs(exact[4*i+2]-1)<1e-6f&&exact[4*i+3]==1,"Cost degraded exact local correspondence");

    // Ambiguous image agreement gets a directional, bounded penalty, without
    // changing the independent geometry score or boosting the other direction.
    float uncertain[64];std::fill(std::begin(uncertain),std::end(uncertain),.54f);
    context->UpdateSubresource(input[1].Get(),0,nullptr,uncertain,32,0);
    auto weak=render(true),without=render(false);
    for(int i=0;i<64;++i) {
        require(weak[4*i+2]>=.10f&&weak[4*i+2]<.95f&&weak[4*i+3]==1,"Ambiguous evidence did not use directional cost");
        require(weak[4*i]==without[4*i]&&weak[4*i+1]==without[4*i+1],"Cost overwrote independent evidence");
        require(without[4*i+2]==1&&without[4*i+3]==1,"Missing cost was not neutral in fusion");
    }
    // A large mismatch remains invalid; low hardware cost cannot rescue it.
    std::fill(std::begin(uncertain),std::end(uncertain),.9f);
    context->UpdateSubresource(input[1].Get(),0,nullptr,uncertain,32,0);
    auto invalid=render(true);
    for(int i=0;i<64;++i)require(invalid[4*i]<.001f&&invalid[4*i+1]<.001f&&invalid[4*i+2]>.99f,"Cost overruled invalid image evidence");

    context->UpdateSubresource(input[4].Get(),0,nullptr,corrected,8,0);
    auto fixed=render(true);
    for(int i=0;i<64;++i)require(fixed[4*i+2]==1,"Fusion used cost from a different repaired vector");
    std::cout<<"PASS fusion: strong agreement protected, ambiguous evidence weighted, invalid match not rescued, absent cost and repaired-vector provenance"<<std::endl;
    return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
