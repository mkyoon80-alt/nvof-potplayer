// Known glyph motion with an injected one-row alias; also retain legitimate
// fast scrolling, non-text motion, small motion and unsupported candidates.
#define wmain original_gpu_smoke_entry
#include "gpu_engine_smoke.cpp"
#undef wmain
#include "motion_shaders.hpp"
int wmain(){try {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"WARP");
    constexpr int w=192,h=192,aw=96,ah=96;
    auto texture=[&](DXGI_FORMAT fmt,int width,int height,int pitch,const void* pixels,UINT bind) {
        D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.MipLevels=d.ArraySize=1;d.Format=fmt;d.SampleDesc.Count=1;d.BindFlags=bind;
        D3D11_SUBRESOURCE_DATA data{pixels,UINT(pitch),0};ComPtr<ID3D11Texture2D> t;
        check(device->CreateTexture2D(&d,pixels?&data:nullptr,&t),"Texture");return t;
    };
    std::vector<float> luma(w*h,.2f),uv(w*h*2,128.f/255.f);
    std::array<ComPtr<ID3D11Texture2D>,6> input;
    std::array<ComPtr<ID3D11ShaderResourceView>,6> views;
    std::vector<int16_t> flowData(24*24*2);
    for(int slot:{0,1,2,3,4,5}) {
        if(slot<2)input[slot]=texture(DXGI_FORMAT_R32_FLOAT,w,h,w*4,luma.data(),D3D11_BIND_SHADER_RESOURCE);
        else if(slot<4)input[slot]=texture(DXGI_FORMAT_R32G32_FLOAT,w,h,w*8,uv.data(),D3D11_BIND_SHADER_RESOURCE);
        else if(slot<6) {
            input[slot]=texture(DXGI_FORMAT_R16G16_SINT,24,24,24*4,flowData.data(),D3D11_BIND_SHADER_RESOURCE);
        }
        check(device->CreateShaderResourceView(input[slot].Get(),nullptr,&views[slot]),"SRV");
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
    auto output=texture(DXGI_FORMAT_R16G16_SINT,24,24,0,nullptr,D3D11_BIND_RENDER_TARGET);
    ComPtr<ID3D11RenderTargetView> rt;check(device->CreateRenderTargetView(output.Get(),nullptr,&rt),"RTV");
    D3D11_TEXTURE2D_DESC td{};output->GetDesc(&td);td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stage;check(device->CreateTexture2D(&td,nullptr,&stage),"Staging");
    auto render=[&](float direction) {
        float constants[]={float(w),float(h),float(aw),float(ah),4,0,direction,1};context->UpdateSubresource(cb.Get(),0,nullptr,constants,0,0);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);
        auto c=cb.Get();auto s=sp.Get();context->PSSetConstantBuffers(0,1,&c);context->PSSetSamplers(0,1,&s);
        ID3D11ShaderResourceView* raw[6]{};for(int i=0;i<6;++i)raw[i]=views[i].Get();context->PSSetShaderResources(0,6,raw);
        D3D11_VIEWPORT vp{0,0,24,24,0,1};context->RSSetViewports(1,&vp);context->RSSetState(rs.Get());auto r=rt.Get();context->OMSetRenderTargets(1,&r,nullptr);context->Draw(3,0);
        ID3D11ShaderResourceView* empty[6]{};context->PSSetShaderResources(0,6,empty);context->OMSetRenderTargets(0,nullptr,nullptr);
        context->CopyResource(stage.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(stage.Get(),0,D3D11_MAP_READ,0,&mapped),"Map");
        std::vector<int16_t> result(24*24*2);for(int y=0;y<24;++y)memcpy(result.data()+y*24*2,static_cast<uint8_t*>(mapped.pData)+y*mapped.RowPitch,24*4);context->Unmap(stage.Get(),0);return result;
    };
    auto source=[&](int shift,bool glyph) {
        std::vector<float> pixels(w*h,16.f/255.f);
        for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
            int q=y+shift;
            if(q<0||q>=h)continue;
            // Different block letters on equally spaced rows: distinguishable,
            // so preferring a neighboring row is provably the wrong motion.
            int row=q/26,col=x/16,xx=x%16,yy=q%26;
            bool ink=yy<12 && xx<12 && (xx<4 || yy<4 || ((col+row)%3==0&&yy>7) || ((col+row)%3==1&&xx>7));
            if(glyph&&ink)pixels[y*w+x]=235.f/255.f;
        }
        return pixels;
    };
    auto upload=[&](int shift,bool glyph,bool alias,bool supported) {
        auto a=source(0,glyph),b=source(shift,glyph);
        context->UpdateSubresource(input[0].Get(),0,nullptr,a.data(),w*4,0);
        context->UpdateSubresource(input[1].Get(),0,nullptr,b.data(),w*4,0);
        for(int direction=0;direction<2;++direction) {
            for(int y=0;y<24;++y)for(int x=0;x<24;++x) {
                bool broken=alias && x>=6&&x<=10&&y>=8&&y<=12;
                float v=broken?58.f:float(shift);
                if(!supported&&!broken&&direction==1)v=0;
                flowData[(y*24+x)*2]=0;flowData[(y*24+x)*2+1]=int16_t((direction?1:-1)*v*16);
            }
            context->UpdateSubresource(input[4+direction].Get(),0,nullptr,flowData.data(),24*4,0);
        }
    };
    auto at=[](const std::vector<int16_t>& v,int row=10){return v[(row*24+8)*2+1];};
    upload(8,true,true,true);
    require(at(render(0))==-128,"One-row alias failed to recover known forward scroll");
    require(at(render(1),9)==128,"One-row alias failed to recover known backward scroll");
    upload(8,true,false,true);
    require(at(render(0))==-128,"Valid small motion changed");
    upload(58,true,false,true);
    require(at(render(0))==-928,"Legitimate fast glyph scrolling was shortened");
    upload(0,true,true,true);
    require(at(render(0))==-928,"Fixed graphic entered scrolling-only repair");
    upload(8,false,true,true);
    require(at(render(0))==-928,"Flat dark area was mistaken for glyph evidence");
    upload(8,true,true,false);
    require(at(render(0))==-928,"Unsupported neighboring trajectory was accepted");
    // Common-motion layer: recover a whole glyph group even when individual
    // vectors point into other rows; reject fixed, fast, colored or mixed layers.
    ComPtr<ID3D11ComputeShader> groups;
    check(device->CreateComputeShader(shaders::motion::glyphGroup,sizeof(shaders::motion::glyphGroup),nullptr,&groups),"Glyph groups CS");
    auto models=texture(DXGI_FORMAT_R32G32B32A32_FLOAT,3,3,0,nullptr,D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE);
    ComPtr<ID3D11UnorderedAccessView> modelUav;check(device->CreateUnorderedAccessView(models.Get(),nullptr,&modelUav),"Glyph groups UAV");
    D3D11_TEXTURE2D_DESC md{};models->GetDesc(&md);md.BindFlags=0;md.Usage=D3D11_USAGE_STAGING;md.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> modelStage;check(device->CreateTexture2D(&md,nullptr,&modelStage),"Model staging");
    auto fit=[&]() {
        float constants[]={float(w),float(h),float(aw),float(ah),4,0,0,.5f};context->UpdateSubresource(cb.Get(),0,nullptr,constants,0,0);
        ID3D11ShaderResourceView* raw[6]{};for(int i=0;i<6;++i)raw[i]=views[i].Get();
        auto c=cb.Get();auto sampler=sp.Get();auto target=modelUav.Get();context->CSSetShader(groups.Get(),nullptr,0);
        context->CSSetConstantBuffers(0,1,&c);context->CSSetSamplers(0,1,&sampler);context->CSSetShaderResources(0,6,raw);context->CSSetUnorderedAccessViews(1,1,&target,nullptr);
        context->Dispatch(3,3,1);ID3D11ShaderResourceView* empty[6]{};target=nullptr;context->CSSetShaderResources(0,6,empty);context->CSSetUnorderedAccessViews(1,1,&target,nullptr);context->CSSetShader(nullptr,nullptr,0);
        context->CopyResource(modelStage.Get(),models.Get());D3D11_MAPPED_SUBRESOURCE data{};check(context->Map(modelStage.Get(),0,D3D11_MAP_READ,0,&data),"Model map");
        std::array<float,4> result{};memcpy(result.data(),static_cast<uint8_t*>(data.pData)+data.RowPitch+16,16);context->Unmap(modelStage.Get(),0);return result;
    };
    upload(8,true,true,true);auto model=fit();
    require(model[3]==1&&std::abs(model[0])<.26f&&std::abs(model[1]+8)<.26f,"Common glyph translation failed to reject row aliases");
    auto b8=source(8,true),b9=source(9,true);for(size_t i=0;i<b8.size();++i)b8[i]=b8[i]*.75f+b9[i]*.25f;
    context->UpdateSubresource(input[1].Get(),0,nullptr,b8.data(),w*4,0);model=fit();
    require(model[3]==1&&std::abs(model[1]+8.25f)<.26f,"Fractional glyph translation was lost");
    upload(0,true,true,true);require(fit()[3]==0,"Fixed text entered common scroll layer");
    upload(58,true,false,true);require(fit()[3]==0,"Fast scroll was shortened by common motion layer");
    upload(8,false,true,true);require(fit()[3]==0,"Flat black accepted as text");
    upload(8,true,true,true);std::fill(uv.begin(),uv.end(),180.f/255.f);context->UpdateSubresource(input[2].Get(),0,nullptr,uv.data(),w*8,0);
    require(fit()[3]==0,"Colored source accepted as monochrome credits");
    std::fill(uv.begin(),uv.end(),128.f/255.f);context->UpdateSubresource(input[2].Get(),0,nullptr,uv.data(),w*8,0);
    auto mixed=source(8,true),opposite=source(-8,true);for(int y=0;y<h;++y)for(int x=w/2;x<w;++x)mixed[y*w+x]=opposite[y*w+x];
    context->UpdateSubresource(input[1].Get(),0,nullptr,mixed.data(),w*4,0);
    require(fit()[3]==0,"Opposing text layers incorrectly share one translation");
    std::cout<<"PASS glyph row-alias correction in both directions, small/fast scroll, flat area, unsupported trajectory; rigid/subpixel group, fixed/colored/mixed rejection"<<std::endl;return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
