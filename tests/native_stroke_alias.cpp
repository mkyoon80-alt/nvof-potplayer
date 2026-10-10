// A repeated bar alias must be corrected only with independent image evidence.
#define wmain original_gpu_smoke_entry
#include "gpu_engine_smoke.cpp"
#undef wmain
#include "motion_shaders.hpp"
int wmain(){try {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"WARP");
    constexpr int w=384,h=384,g=4,gw=w/g,gh=h/g;
    auto texture=[&](DXGI_FORMAT fmt,int width,int height,int pitch,const void* pixels,UINT bind) {
        D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.MipLevels=d.ArraySize=1;d.Format=fmt;d.SampleDesc.Count=1;d.BindFlags=bind;
        D3D11_SUBRESOURCE_DATA data{pixels,UINT(pitch),0};ComPtr<ID3D11Texture2D> t;
        check(device->CreateTexture2D(&d,pixels?&data:nullptr,&t),"Texture");return t;
    };
    std::array<ComPtr<ID3D11Texture2D>,6> input;
    std::array<ComPtr<ID3D11ShaderResourceView>,6> views;
    std::vector<float> zero(w*h*2,0.6f);
    for(int i=0;i<6;++i) {
        if(i<2)input[i]=texture(DXGI_FORMAT_R32_FLOAT,w,h,w*4,zero.data(),D3D11_BIND_SHADER_RESOURCE);
        else if(i<4)input[i]=texture(DXGI_FORMAT_R32G32_FLOAT,w,h,w*8,zero.data(),D3D11_BIND_SHADER_RESOURCE);
        else input[i]=texture(DXGI_FORMAT_R16G16_SINT,gw,gh,0,nullptr,D3D11_BIND_SHADER_RESOURCE);
        check(device->CreateShaderResourceView(input[i].Get(),nullptr,&views[i]),"SRV");
    }
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
    check(device->CreateVertexShader(shaders::motion::vs,sizeof(shaders::motion::vs),nullptr,&vs),"VS");
    check(device->CreatePixelShader(shaders::motion::repairMotion,sizeof(shaders::motion::repairMotion),nullptr,&ps),"PS");
    D3D11_BUFFER_DESC cd{};cd.ByteWidth=32;cd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;ComPtr<ID3D11Buffer> cb;
    check(device->CreateBuffer(&cd,nullptr,&cb),"Constants");
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sp;check(device->CreateSamplerState(&sd,&sp),"Sampler");
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;ComPtr<ID3D11RasterizerState> rs;
    check(device->CreateRasterizerState(&rd,&rs),"Raster");
    auto output=texture(DXGI_FORMAT_R16G16_SINT,gw,gh,0,nullptr,D3D11_BIND_RENDER_TARGET);
    ComPtr<ID3D11RenderTargetView> rt;check(device->CreateRenderTargetView(output.Get(),nullptr,&rt),"RTV");
    D3D11_TEXTURE2D_DESC td{};output->GetDesc(&td);td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stage;check(device->CreateTexture2D(&td,nullptr,&stage),"Staging");
    auto run=[&](bool horizontal,int scenario) {
        const float motion=scenario==1?15.f:-1.f;
        std::vector<float> image(w*h);
        for(int n=0;n<2;++n) {
            for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
                double q=(horizontal?y:x)-motion*n;
                image[y*w+x]=float(140+50*std::cos(q*6.283185307/16)+(scenario==2?0:12*std::cos(q*6.283185307/64)))/255.f;
                if(scenario>=4) {
                    double u=x+n,v=y-n;
                    bool ink=u>=160&&u<210&&v>=100&&v<280&&!(u>=170&&u<198&&v>=112&&v<268);
                    image[y*w+x]=float(40+10*std::cos(u*6.283185307/5)+10*std::cos(v*6.283185307/5)+(scenario==4&&ink?150:0))/255.f;
                }
            }
            context->UpdateSubresource(input[n].Get(),0,nullptr,image.data(),w*4,0);
            std::vector<int16_t> vectors(gw*gh*2);
            for(int y=0;y<gh;++y)for(int x=0;x<gw;++x) {
                float v=motion;
                if(scenario!=1&&x>=40&&x<56&&y>=40&&y<56)v=15;
                if(scenario==3&&(horizontal?x:y)<40)v=4;
                vectors[(y*gw+x)*2+(horizontal?1:0)]=int16_t(std::lround((n?-v:v)*32));
                if(scenario>=4) {
                    bool alias=x>=40&&x<56&&y>=40&&y<56;
                    vectors[(y*gw+x)*2]=int16_t((n?-1:1)*(alias?19:-1)*32);
                    vectors[(y*gw+x)*2+1]=int16_t((n?-1:1)*(alias?11:1)*32);
                }
            }
            context->UpdateSubresource(input[n+4].Get(),0,nullptr,vectors.data(),gw*4,0);
        }
        float constants[]={float(w),float(h),float(w),float(h),float(g),0,0,1};context->UpdateSubresource(cb.Get(),0,nullptr,constants,0,0);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);
        auto c=cb.Get();auto s=sp.Get();context->PSSetConstantBuffers(0,1,&c);context->PSSetSamplers(0,1,&s);
        ID3D11ShaderResourceView* raw[6]{};for(int i=0;i<6;++i)raw[i]=views[i].Get();context->PSSetShaderResources(0,6,raw);
        D3D11_VIEWPORT vp{0,0,float(gw),float(gh),0,1};context->RSSetViewports(1,&vp);context->RSSetState(rs.Get());auto r=rt.Get();context->OMSetRenderTargets(1,&r,nullptr);context->Draw(3,0);
        ID3D11ShaderResourceView* empty[6]{};context->PSSetShaderResources(0,6,empty);context->OMSetRenderTargets(0,nullptr,nullptr);
        context->CopyResource(stage.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(stage.Get(),0,D3D11_MAP_READ,0,&mapped),"Map");
        auto row=reinterpret_cast<const int16_t*>(static_cast<const uint8_t*>(mapped.pData)+48*mapped.RowPitch);
        int value=row[48*2+(horizontal?1:0)],vx=row[48*2],vy=row[48*2+1];context->Unmap(stage.Get(),0);
        std::cout<<"axis="<<horizontal<<" scenario="<<scenario<<" motion="<<vx/32.f<<","<<vy/32.f<<std::endl;
        if(scenario==4)require(std::abs(vx+32)<=4&&std::abs(vy-32)<=4,"Periodic screen displaced the glyph instead of following its true motion");
        else if(scenario==5)require(vx==19*32&&vy==11*32,"Indistinguishable screen pattern gained unsupported correction");
        else require(value==(scenario==0?-32:480),scenario==0?"Disambiguated bar still follows its neighbor":"Valid or ambiguous motion was replaced without evidence");
    };
    for(bool horizontal:{false,true})for(int scenario=0;scenario<6;++scenario)run(horizontal,scenario);
    std::cout<<"PASS both axes: alias, genuine fast motion, indistinguishable bars, disagreeing donors, screen glyph, ambiguous microtexture"<<std::endl;return 0;
}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
