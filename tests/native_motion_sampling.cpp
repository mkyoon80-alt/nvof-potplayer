// A known motion discontinuity must not sample a third, unrelated source position.
#define wmain original_gpu_smoke_entry
#include "gpu_engine_smoke.cpp"
#undef wmain
#include "motion_shaders.hpp"
int wmain(){try {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"WARP");
    constexpr int w=64,h=16,aw=32,ah=8;
    auto texture=[&](DXGI_FORMAT fmt,int width,int height,int pitch,const void* pixels,UINT bind) {
        D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.MipLevels=d.ArraySize=1;d.Format=fmt;d.SampleDesc.Count=1;d.BindFlags=bind;
        D3D11_SUBRESOURCE_DATA data{pixels,UINT(pitch),0};ComPtr<ID3D11Texture2D> t;
        check(device->CreateTexture2D(&d,pixels?&data:nullptr,&t),"Texture");return t;
    };
    std::vector<float> luma(w*h,.2f),uv(w*h*2,.5f),mask(w*h,0),offsets(aw*ah*4),weights(aw*ah*4);
    for(int y=0;y<h;++y)for(int x=35;x<47;++x)luma[y*w+x]=.9f;
    for(int y=0;y<ah;++y)for(int x=0;x<aw;++x){auto i=(y*aw+x)*4;offsets[i]=offsets[i+2]=x<16?0.f:16.f;weights[i]=weights[i+1]=.5f;weights[i+2]=weights[i+3]=1.f;}
    std::array<ComPtr<ID3D11Texture2D>,9> input;
    std::array<ComPtr<ID3D11ShaderResourceView>,9> views;
    for(int slot:{0,1,2,3,6,7,8}) {
        if(slot<2)input[slot]=texture(DXGI_FORMAT_R32_FLOAT,w,h,w*4,luma.data(),D3D11_BIND_SHADER_RESOURCE);
        else if(slot<4)input[slot]=texture(DXGI_FORMAT_R32G32_FLOAT,w,h,w*8,uv.data(),D3D11_BIND_SHADER_RESOURCE);
        else if(slot==8)input[slot]=texture(DXGI_FORMAT_R32_FLOAT,w,h,w*4,mask.data(),D3D11_BIND_SHADER_RESOURCE);
        else input[slot]=texture(DXGI_FORMAT_R32G32B32A32_FLOAT,aw,ah,aw*16,slot==6?offsets.data():weights.data(),D3D11_BIND_SHADER_RESOURCE);
        check(device->CreateShaderResourceView(input[slot].Get(),nullptr,&views[slot]),"SRV");
    }
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
    check(device->CreateVertexShader(shaders::motion::vs,sizeof(shaders::motion::vs),nullptr,&vs),"VS");
    check(device->CreatePixelShader(shaders::motion::midpoint,sizeof(shaders::motion::midpoint),nullptr,&ps),"PS");
    D3D11_BUFFER_DESC cd{};cd.ByteWidth=32;cd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;ComPtr<ID3D11Buffer> cb;
    check(device->CreateBuffer(&cd,nullptr,&cb),"Constants");
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sp;check(device->CreateSamplerState(&sd,&sp),"Sampler");
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;ComPtr<ID3D11RasterizerState> rs;
    check(device->CreateRasterizerState(&rd,&rs),"Raster");
    auto output=texture(DXGI_FORMAT_R32G32B32A32_FLOAT,w,h,0,nullptr,D3D11_BIND_RENDER_TARGET);
    ComPtr<ID3D11RenderTargetView> rt;check(device->CreateRenderTargetView(output.Get(),nullptr,&rt),"RTV");
    D3D11_TEXTURE2D_DESC td{};output->GetDesc(&td);td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stage;check(device->CreateTexture2D(&td,nullptr,&stage),"Staging");
    auto render=[&](float phase) {
        float constants[]={float(w),float(h),float(aw),float(ah),4,0,0,phase};context->UpdateSubresource(cb.Get(),0,nullptr,constants,0,0);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);
        auto c=cb.Get();auto s=sp.Get();context->PSSetConstantBuffers(0,1,&c);context->PSSetSamplers(0,1,&s);
        ID3D11ShaderResourceView* raw[9]{};for(int i=0;i<9;++i)raw[i]=views[i].Get();context->PSSetShaderResources(0,9,raw);
        D3D11_VIEWPORT vp{0,0,float(w),float(h),0,1};context->RSSetViewports(1,&vp);context->RSSetState(rs.Get());auto r=rt.Get();context->OMSetRenderTargets(1,&r,nullptr);context->Draw(3,0);
        ID3D11ShaderResourceView* empty[9]{};context->PSSetShaderResources(0,9,empty);context->OMSetRenderTargets(0,nullptr,nullptr);
        context->CopyResource(stage.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(stage.Get(),0,D3D11_MAP_READ,0,&mapped),"Map");
        std::vector<float> result(w*h*4);for(int y=0;y<h;++y)memcpy(result.data()+y*w*4,static_cast<uint8_t*>(mapped.pData)+y*mapped.RowPitch,w*16);context->Unmap(stage.Get(),0);return result;
    };
    for(float phase:{.25f,.5f,.75f}) {
        auto result=render(phase);
        for(int y=0;y<h;++y)for(int x:{31,32})require(std::abs(result[(y*w+x)*4]-.2f)<1e-5f,"Inverse branch averaging sampled unrelated bright stripe");
    }
    // Smooth motion retains subpixel translation; do not fix discontinuities
    // by globally snapping offsets or holding original pixels.
    for(int y=0;y<h;++y)for(int x=0;x<w;++x)luma[y*w+x]=float(x)/w;
    for(int i=0;i<aw*ah;++i)offsets[i*4]=offsets[i*4+2]=2.25f;
    for(int slot:{0,1})context->UpdateSubresource(input[slot].Get(),0,nullptr,luma.data(),w*4,0);
    context->UpdateSubresource(input[6].Get(),0,nullptr,offsets.data(),aw*16,0);
    auto smooth=render(.5f);
    for(int y=0;y<h;++y)for(int x=2;x<w-4;++x)require(std::abs(smooth[(y*w+x)*4]-(x+2.25f)/w)<1e-5f,"Smooth subpixel motion changed");
    std::cout<<"PASS discontinuous inverse branches at 2x scale, three phases, smooth subpixel translation"<<std::endl;return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
