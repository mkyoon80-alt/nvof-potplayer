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
    // A nearly black picture need not be Y=16, and neutral ink can scroll
    // over a changing colored picture. Background color is not glyph motion.
    auto layered=[&](int shift,int frame,float background,bool colored) {
        auto pixels=source(shift,true);std::vector<float> chroma(w*h*2,128.f/255.f);
        for(int y=0;y<h;++y)for(int x=0;x<w;++x)if(pixels[y*w+x]<.5f) {
            pixels[y*w+x]=(background+(colored?20.f*std::sin(y*.4f+x*.2f+frame*.6f):0.f))/255.f;
            if(colored){chroma[(y*w+x)*2]=110.f/255.f;chroma[(y*w+x)*2+1]=143.f/255.f;}
        }
        context->UpdateSubresource(input[frame].Get(),0,nullptr,pixels.data(),w*4,0);
        context->UpdateSubresource(input[frame+2].Get(),0,nullptr,chroma.data(),w*8,0);
        return pixels;
    };
    for(float black:{20.f,45.f}) {
        upload(8,true,true,true);layered(0,0,black,false);layered(8,1,black+2,false);model=fit();
        require(model[3]==2&&std::abs(model[1]+8)<.26f,"Non-reference black/fading background lost glyph motion");
    }
    upload(8,true,true,true);auto aLayer=layered(0,0,100,true),bLayer=layered(8,1,100,true);model=fit();
    require(model[3]==2&&std::abs(model[0])<.26f&&std::abs(model[1]+8)<.26f,"Colored moving background hid neutral glyphs");
    layered(0,1,100,true);require(fit()[3]==0,"Background movement activated fixed white text");
    bLayer=layered(8,1,100,true);
    // Exercise final compositing: coherent neighboring models repair the glyph
    // footprint; pixels away from ink retain the ordinary mapping byte-for-byte.
    std::vector<float> offsets(aw*ah*4,0),weights(aw*ah*4,1),mask(w*h*2,0);
    for(int i=0;i<aw*ah;++i)weights[i*4]=weights[i*4+1]=.5f;
    auto ot=texture(DXGI_FORMAT_R32G32B32A32_FLOAT,aw,ah,aw*16,offsets.data(),D3D11_BIND_SHADER_RESOURCE);
    auto wt=texture(DXGI_FORMAT_R32G32B32A32_FLOAT,aw,ah,aw*16,weights.data(),D3D11_BIND_SHADER_RESOURCE);
    auto mt=texture(DXGI_FORMAT_R32G32_FLOAT,w,h,w*8,mask.data(),D3D11_BIND_SHADER_RESOURCE);
    ComPtr<ID3D11ShaderResourceView> ov,wv,mv,gv;
    check(device->CreateShaderResourceView(ot.Get(),nullptr,&ov),"Offsets view");check(device->CreateShaderResourceView(wt.Get(),nullptr,&wv),"Weights view");
    check(device->CreateShaderResourceView(mt.Get(),nullptr,&mv),"Mask view");check(device->CreateShaderResourceView(models.Get(),nullptr,&gv),"Models view");
    ComPtr<ID3D11PixelShader> compose;check(device->CreatePixelShader(shaders::motion::midpoint,sizeof(shaders::motion::midpoint),nullptr,&compose),"Compose");
    auto composed=texture(DXGI_FORMAT_R32_FLOAT,w,h,0,nullptr,D3D11_BIND_RENDER_TARGET);
    ComPtr<ID3D11RenderTargetView> composedRt;check(device->CreateRenderTargetView(composed.Get(),nullptr,&composedRt),"Compose RTV");
    composed->GetDesc(&td);td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> composedStage;check(device->CreateTexture2D(&td,nullptr,&composedStage),"Compose staging");
    auto composeModels=[&](const std::vector<float>& values) {
        context->UpdateSubresource(models.Get(),0,nullptr,values.data(),3*16,0);
        float constants[]={float(w),float(h),float(aw),float(ah),4,0,0,.5f};context->UpdateSubresource(cb.Get(),0,nullptr,constants,0,0);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(compose.Get(),nullptr,0);
        auto c=cb.Get();auto sampler=sp.Get();context->PSSetConstantBuffers(0,1,&c);context->PSSetSamplers(0,1,&sampler);
        ID3D11ShaderResourceView* raw[14]{};for(int i=0;i<6;++i)raw[i]=views[i].Get();raw[6]=ov.Get();raw[7]=wv.Get();raw[8]=mv.Get();raw[13]=gv.Get();context->PSSetShaderResources(0,14,raw);
        D3D11_VIEWPORT vp{0,0,float(w),float(h),0,1};context->RSSetViewports(1,&vp);context->RSSetState(rs.Get());auto r=composedRt.Get();context->OMSetRenderTargets(1,&r,nullptr);context->Draw(3,0);
        ID3D11ShaderResourceView* empty[14]{};context->PSSetShaderResources(0,14,empty);context->OMSetRenderTargets(0,nullptr,nullptr);
        context->CopyResource(composedStage.Get(),composed.Get());D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(composedStage.Get(),0,D3D11_MAP_READ,0,&mapped),"Compose map");
        std::vector<float> out(w*h);for(int y=0;y<h;++y)memcpy(out.data()+y*w,static_cast<uint8_t*>(mapped.pData)+y*mapped.RowPitch,w*4);context->Unmap(composedStage.Get(),0);return out;
    };
    std::vector<float> modelValues(36,0);auto ordinary=composeModels(modelValues);
    for(int i=0;i<9;++i){modelValues[i*4+1]=-8;modelValues[i*4+3]=2;}
    auto corrected=composeModels(modelValues),middle=source(4,true);int repaired=0,backgroundPixels=0;
    for(int y=48;y<144;++y)for(int x=48;x<144;++x) {
        int i=y*w+x;
        if(middle[i]>.8f){require(std::abs(corrected[i]-middle[i])<.001f,"Verified glyph was not placed at its true midpoint");if(ordinary[i]<.8f)++repaired;}
        bool clear=true;
        for(int yy=y-6;yy<=y+6;++yy)for(int xx=x-2;xx<=x+2;++xx)if(aLayer[yy*w+xx]>.63f||bLayer[yy*w+xx]>.63f)clear=false;
        if(clear){require(corrected[i]==ordinary[i],"Unrelated background was moved at glyph speed");++backgroundPixels;}
    }
    require(repaired>100&&backgroundPixels>100,"Layer compositing test did not exercise glyph/background separation");
    std::fill(modelValues.begin(),modelValues.end(),0);modelValues[4*4+1]=-8;modelValues[4*4+3]=2;
    require(composeModels(modelValues)==ordinary,"An isolated foreground match bypassed spatial support");
    for(int i=0;i<9;++i){modelValues[i*4+1]=i%3==1?8.f:-8.f;modelValues[i*4+3]=2;}
    auto conflict=composeModels(modelValues);
    for(int y=64;y<128;++y)for(int x=64;x<128;++x)require(conflict[y*w+x]==ordinary[y*w+x],"Conflicting neighboring text motions were blended");
    std::cout<<"PASS glyph row-alias correction in both directions, small/fast scroll, flat area, unsupported trajectory; rigid/subpixel group, fixed/colored/mixed rejection; non-reference black, colored moving background, glyph-only composition, isolated/conflicting support rejection"<<std::endl;return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
