#define NVOF_CPU10_HELPERS_ONLY
#include "directshow_cpu10.cpp"
#include <optional>
#include "nvof/cpu_upload.hpp"
#include "nvof/mf_bridge.hpp"
std::vector<uint16_t> read_upload_frame(nvof::MfD3d11Bridge& bridge,const nvof::GpuFrame& frame) {
    D3D11_TEXTURE2D_DESC d{};frame.texture->GetDesc(&d);
    if(d.Format!=DXGI_FORMAT_P010)throw std::runtime_error("Seek lost 10-bit output");
    d.BindFlags=d.MiscFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;check(bridge.device()->CreateTexture2D(&d,nullptr,&staging),"seek readback texture");
    bridge.context()->CopyResource(staging.Get(),frame.texture.Get());D3D11_MAPPED_SUBRESOURCE map{};
    check(bridge.context()->Map(staging.Get(),0,D3D11_MAP_READ,0,&map),"seek readback");
    std::vector<uint16_t> pixels(size_t(frame.width)*frame.height*3/2);
    for(int y=0;y<frame.height*3/2;++y)memcpy(pixels.data()+size_t(y)*frame.width,static_cast<BYTE*>(map.pData)+size_t(y)*map.RowPitch,frame.width*2);
    bridge.context()->Unmap(staging.Get(),0);return pixels;
}
void check_comb_detection(nvof::MfD3d11Bridge& bridge) {
    nvof::CpuUpload upload(bridge.device(),bridge.context(),bridge.mutex());
    nvof::CpuVideoLayout l{256,128,512,nvof::CpuVideoFormat::p010};
    std::vector<uint16_t> words(nvof::cpu_video_bytes(l)/2,512<<6);
    auto fill=[&](int frame,bool fields,bool blend,bool stripes){
        std::vector<int> y(256*128,64);
        for(int r=0;r<128;++r)for(int x=0;x<256;++x){
            const int offset=(frame*4)%80+(fields&&(r&1)?16:0);
            y[r*256+x]=stripes?((r&1)?700:200):r>12&&r<116&&x>=32+offset&&x<96+offset?700:64;
        }
        for(int r=0;r<128;++r)for(int x=0;x<256;++x)
            words[r*256+x]=uint16_t((blend?(y[(std::max)(0,r-1)*256+x]+2*y[r*256+x]+y[(std::min)(127,r+1)*256+x]+2)/4:y[r*256+x])<<6);
        upload.begin(reinterpret_cast<const uint8_t*>(words.data()),words.size()*2,l,frame==0);
        return upload.needs_field_reconstruction();
    };
    for(int mode=0;mode<3;++mode){upload.reset();for(int f=0;f<10;++f)
        if(fill(f,mode==1,mode==1,mode==2))throw std::runtime_error("Progressive/blended/static detail misdetected as interlaced");}
    upload.reset();for(int f=0;f<10;++f){bool detected=fill(f,true,false,false);
        if(f>=2&&!detected)throw std::runtime_error("Moving field combs not detected");}
    for(int f=10;f<30;++f){bool detected=fill(f,false,false,false);
        if(f>=26&&detected)throw std::runtime_error("Field decision did not release on progressive frames");}
    // Reproduce repeated seeks after this stream has proved to contain raw
    // fields. No pre-seek picture may be needed to reconstruct the first one.
    for(int f=30;f<36;++f)fill(f,true,false,false);
    for(int jump:{61,7,45,1,72}) {
        // Flush, end-flush, new-segment and preroll can each reset temporal
        // state before the first visible picture from a rapid seek.
        for(int reset=0;reset<4;++reset)upload.reset(true);
        if(!fill(jump,true,false,false))throw std::runtime_error("Raw WEAVE fields escaped reconstruction on first post-seek picture");
        for(int parity:{0,1}) {
            auto pixels=read_upload_frame(bridge,upload.render(parity,0));
            const int offset=(jump*4)%80+(parity?16:0);
            // Independent oracle: the rectangle has one position at each
            // field time. Opposite-field or pre-seek pixels fail this check.
            for(int y=20;y<100;++y)for(int x=0;x<256;++x) {
                const uint16_t expected=uint16_t((x>=32+offset&&x<96+offset?700:64)<<6);
                if(pixels[y*256+x]!=expected)throw std::runtime_error("First post-seek picture contains combs or stale pixels");
            }
        }
    }
    upload.reset(true);
    if(fill(80,true,true,false))throw std::runtime_error("Seek hint forced reconstruction of an already blended picture");
    upload.reset();if(fill(0,false,false,false))throw std::runtime_error("Field decision survived reset");
    upload.reset();if(fill(0,true,false,false))throw std::runtime_error("Raw-field hint leaked into a different stream");
}
int wmain() {SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);try {
    nvof::MfD3d11Bridge bridge;kWidth=1024;kHeight=12;
    check_comb_detection(bridge);
    for(auto format:{nvof::CpuVideoFormat::p010,nvof::CpuVideoFormat::p016,nvof::CpuVideoFormat::p210,nvof::CpuVideoFormat::y210}) {
        nvof::CpuUpload upload(bridge.device(),bridge.context(),bridge.mutex());
        nvof::CpuVideoLayout l{kWidth,kHeight,kWidth*(format==nvof::CpuVideoFormat::y210?4:2)+32,format};
        std::vector<uint8_t> data(nvof::cpu_video_bytes(l),0xff);std::optional<nvof::CpuVideoPlanes> previous;
        for(int frame=0;frame<10;++frame) {
            // Moving/still pairs, ring reuse, both fields, all codes and borders.
            auto put=[&](size_t p,int v){uint16_t word=uint16_t((v&1023)<<6);memcpy(data.data()+p,&word,2);};
            for(int y=0;y<kHeight;++y)for(int x=0;x<kWidth;x+=2) {
                if(format==nvof::CpuVideoFormat::y210){size_t p=size_t(y)*l.pitch+x*4;put(p,x+y*17+frame/2);put(p+2,111+y*13);put(p+4,x+1+y*17+frame/2);put(p+6,731-y*11);}
                else{put(size_t(y)*l.pitch+x*2,x+y*17+frame/2);put(size_t(y)*l.pitch+x*2+2,x+1+y*17+frame/2);}
            }
            if(format!=nvof::CpuVideoFormat::y210)for(int y=0;y<(nvof::cpu_422(format)?kHeight:kHeight/2);++y)for(int x=0;x<kWidth;x+=2) {
                size_t p=size_t(kHeight+y)*l.pitch+x*2;put(p,111+y*13);put(p+2,731-y*11);
            }
            const bool disc=frame==0||frame==5;if(disc)previous.reset();
            upload.begin(data.data(),data.size(),l,disc);auto planes=nvof::unpack_cpu_video(data.data(),data.size(),l);
            for(int parity:{-1,0,1}) {
                // A renderer can retain its pipeline while presenting a paused
                // frame. Uploading either plane must leave that state untouched.
                auto* context=bridge.context();
                const D3D11_VIEWPORT sentinel{3,7,19,23,0.2f,0.8f};
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
                context->RSSetViewports(1,&sentinel);context->VSSetShader(nullptr,nullptr,0);context->PSSetShader(nullptr,nullptr,0);
                ID3D11Buffer* noBuffer=nullptr;context->PSSetConstantBuffers(0,1,&noBuffer);
                context->CSSetShader(nullptr,nullptr,0);upload.needs_field_reconstruction();
                auto gpu=upload.render(parity,frame*400000);
                D3D11_PRIMITIVE_TOPOLOGY topology{};context->IAGetPrimitiveTopology(&topology);
                UINT count=1;D3D11_VIEWPORT restored{};context->RSGetViewports(&count,&restored);
                ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;ComPtr<ID3D11Buffer> cb;
                context->VSGetShader(&vs,nullptr,nullptr);context->PSGetShader(&ps,nullptr,nullptr);context->PSGetConstantBuffers(0,1,&cb);
                ComPtr<ID3D11ComputeShader> cs;context->CSGetShader(&cs,nullptr,nullptr);
                if(topology!=D3D11_PRIMITIVE_TOPOLOGY_LINELIST || count!=1 || memcmp(&restored,&sentinel,sizeof(sentinel)) || vs || ps || cb || cs)
                    throw std::runtime_error("CPU upload changed the renderer context state");
                HRESULT hr=S_OK;auto* raw=new nvof::transport::SurfaceAllocator(&hr,true);ComPtr<IMemAllocator> allocator;
                check(raw->QueryInterface(IID_PPV_ARGS(&allocator)),"oracle allocator");ALLOCATOR_PROPERTIES req{1,kWidth*kHeight*3,1,0},actual;
                check(allocator->SetProperties(&req,&actual),"oracle properties");check(allocator->Commit(),"oracle commit");
                ComPtr<IMediaSample> sample;check(allocator->GetBuffer(&sample,nullptr,nullptr,0),"oracle sample");
                dynamic_cast<nvof::transport::SurfaceSample*>(sample.Get())->assign(gpu);
                auto got=read_output(sample.Get(),bridge.device(),bridge.context());auto expected=nvof::pack_cpu_p010(planes,parity,previous?&*previous:nullptr);
                if(got!=expected)throw std::runtime_error("GPU field differs: format="+std::to_string(int(format))+" frame="+std::to_string(frame)+" parity="+std::to_string(parity));
                sample.Reset();allocator->Decommit();
            }
            previous=std::move(planes);
        }
    }
    std::cout<<"PASS GPU/CPU reference: 4 formats, 1024 codes, both fields, motion/still history, reset, padding, pool reuse; rapid-seek first-picture field oracle\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
