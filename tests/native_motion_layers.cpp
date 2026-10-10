// Hardware regressions: static curved overlays, repeated moving bars, and
// bright moving foregrounds and thin curved lines that must not be frozen as text.
#define wmain original_gpu_smoke_entry
#include "gpu_engine_smoke.cpp"
#undef wmain
#include <d3d10_1.h>
namespace {
GpuFrame uploadGuard(ID3D11Device* device,const std::vector<uint8_t>& bytes,int w,int h,int64_t pts) {
    D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=1;
    d.Format=DXGI_FORMAT_NV12;d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{bytes.data(),UINT(w),UINT(bytes.size())};ComPtr<ID3D11Texture2D> t;
    check(device->CreateTexture2D(&d,&initial,&t),"Upload guard fixture");return {t,0,w,h,pts};
}
}
int wmain(int argc,wchar_t** argv){try{
    require(argc>=2,"native_motion_layers <runtime>");
    ComPtr<IDXGIFactory1> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI");ComPtr<IDXGIAdapter1> adapter;
    for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> a;if(factory->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND)break;
        DXGI_ADAPTER_DESC1 d{};a->GetDesc1(&d);if(d.VendorId==0x10de){adapter=a;break;}}
    require(bool(adapter),"NVIDIA missing");ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
    check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_VIDEO_SUPPORT|D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels,2,D3D11_SDK_VERSION,&device,nullptr,&context),"D3D11");
    ComPtr<ID3D10Multithread> protection;check(context.As(&protection),"Context protection");protection->SetMultithreadProtected(TRUE);
    GpuFrucEngine engine(argv[1],device.Get(),context.Get(),nullptr,GpuCompletionMode::context_ordered,true,false,false,GpuInterpolationBackend::native_experimental,1920,argc>2?static_cast<MotionCostMode>(std::stoi(argv[2])):MotionCostMode::disabled,MotionFlowOptions{argc>3?unsigned(std::stoul(argv[3])):0u,argc>4&&std::wstring(argv[4])==L"slow"?MotionFlowQuality::slow:MotionFlowQuality::medium});
    const int w=960,h=540;
    auto fixture=[&](double t,int scene) {
        auto image=moving_color(w,h,t*12,false);
        for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
            auto& v=image[size_t(y)*w+x];
            if(scene==0) {
                double radius=std::hypot(x-300.0,y-265.0);
                double alpha=std::clamp(2.5-std::abs(radius-170),0.0,1.0);
                if(x>550&&x<850&&y>180&&y<340&&((x-550)%65)<10)alpha=1;
                v=uint8_t(std::lround(v*(1-alpha)+235*alpha));
            } else if(scene==1) {
                double shift=x<w/2?-9*t:9*t;
                double phase=std::remainder(x-shift,52.0);
                double bars=std::exp(-phase*phase/12.0);
                v=uint8_t(std::lround(225-90*bars+7*std::sin(y*.05)));
                if((x>200&&x<340&&y>130)||(x>600&&x<750&&y>100))v=70+uint8_t(20*color_field(x,y,765432));
            } else if(scene==6) {
                // A small foreground moves against the camera pan. Its leading
                // occlusion edge must not pull dark pixels onto the road.
                double qx=x+18*t,qy=y+2*t;
                double body=std::hypot((qx-480)/2.8,qy-270);
                double distance=body-13;
                for(double px:{450.,510.})for(double py:{258.,282.})
                    distance=(std::min)(distance,std::hypot(qx-px,qy-py)-7);
                double alpha=std::clamp(.5-distance,0.0,1.0);
                double road=100+8*std::sin((x-16*t)*.03+(y-10*t)*.11);
                double car=25+8*std::sin(qx*.14)*std::cos(qy*.12);
                v=uint8_t(std::lround(road*(1-alpha)+car*alpha));
            } else if(scene==5) {
                // A curved foreground crosses a rapidly moving background.
                // The exact fractional image tests occlusion inversion without
                // relying on a particular source clip or on the flow algorithm.
                double qx=x-3*t,qy=y-18*t;
                double radius=std::hypot((qx-490)*.9,qy-210);
                double rim=160+9*std::sin(std::atan2(qy-210,qx-490)*11);
                double alpha=std::clamp(rim-radius,0.0,1.0);
                double value=210+8*std::sin(qx*.027)*std::sin(qy*.035);
                double ink=std::clamp(1.5-std::abs(std::remainder(qx+12*std::sin(qy*.018),35.0)),0.0,1.0);
                value-=120*ink;
                double background=85+30*color_field(x-48*t,y,123456);
                v=uint8_t(std::lround(background*(1-alpha)+value*alpha));
            } else if(scene==4) {
                // Repeated glyph-like strokes move together. Both dark strokes
                // and flat light interiors must retain the intermediate position.
                double qx=x-1.5*t,qy=y+6.5*t;
                double alpha=std::clamp((std::min)({qx-220.0,750.0-qx,qy-40.0,500.0-qy}),0.0,1.0);
                double distance=(std::min)(std::abs(qx-235.0),std::abs(qx-730.0));
                for(int stroke=0;stroke<6;++stroke)distance=(std::min)(distance,std::abs(qy-55.0-stroke*86.0));
                double ink=1.0/(1.0+std::exp((distance-8.0)*.7));
                double value=230-177*ink;
                v=uint8_t(std::lround(v*(1-alpha)+value*alpha));
            } else if(scene==3) {
                double qx=x-5*t,qy=y-3*t;
                double alpha=std::clamp((std::min)({qx-220.0,740.0-qx,qy-70.0,470.0-qy}),0.0,1.0);
                double value=225;
                for(int strand=0;strand<18;++strand) {
                    double center=245+26*strand+13*std::sin(qy*.012+strand*.3);
                    double ink=std::clamp(1.7-std::abs(qx-center),0.0,1.0);
                    value=(std::min)(value,225-170*ink);
                }
                v=uint8_t(std::lround(v*(1-alpha)+value*alpha));
            } else {
                double left=170+48*t,right=left+260;
                double alpha=std::clamp((std::min)({x+1-left,right-x,y-100.0,440.0-y}),0.0,1.0);
                double value=230;
                if(std::abs(x-(left+120))<4&&y>180&&y<320)value=70;
                v=uint8_t(std::lround(v*(1-alpha)+value*alpha));
            }
        }
        std::fill(image.begin()+size_t(w)*h,image.end(),128);return image;
    };
    bool qualityPassed=true;
    for(int multiple : {2,4,5}) for(int scene=0;scene<7;++scene) {
        double sum=0,held=0;uint64_t ghosts=0,band=0;
        for(int i=0;i<8;++i) {
            double t=i*.125;auto a=fixture(t,scene),b=fixture(t+1,scene);
            auto first=engine.copy(uploadGuard(device.Get(),a,w,h,0));auto next=engine.copy(uploadGuard(device.Get(),b,w,h,400000));
            std::vector<int64_t> times;for(int j=1;j<multiple;++j)times.push_back(400000LL*j/multiple);
            auto batch=engine.interpolate_pair(first,next,times);
            require(batch.frames.size()==size_t(multiple-1)&&!batch.quality.scene_cut&&batch.quality.repeated_mask==0,"Layer fixture lost interpolation");
            for(int phase=0;phase<multiple-1;++phase) {
                auto expected=fixture(t+double(phase+1)/multiple,scene),actual=readback(device.Get(),context.Get(),batch.frames[phase]);
                sum+=mse(actual,expected,w,h);held+=mse(a,expected,w,h);
                if(scene==0)for(int y=50;y<480;++y)for(int x=90;x<510;++x) {
                    double d=std::abs(std::hypot(x-300.0,y-265.0)-170);
                    if(d>3&&d<25){size_t k=size_t(y)*w+x;++band;ghosts+=actual[k]>200&&int(actual[k])-expected[k]>24;}
                }
            }
            require(readback(device.Get(),context.Get(),first)==a,"Layer source modified");
        }
        std::cout<<"LAYERS multiple="<<multiple<<" scene="<<scene<<" mse="<<sum/(8*(multiple-1))<<" held="<<held/(8*(multiple-1))<<" exterior_ghosts="<<ghosts<<" band="<<band<<std::endl;
        if(!(sum<held*.6)){qualityPassed=false;std::cout<<"REGRESSION scene="<<scene<<" moving layer degraded to a hold or crossfade"<<std::endl;}
        if(scene==0&&!(ghosts<double(band)*.0001)){qualityPassed=false;std::cout<<"REGRESSION stationary outline leaked into moving background"<<std::endl;}
    }
    require(qualityPassed,"One or more layer quality regressions");
    std::cout<<"PASS curved stationary outline exterior, repeated bars, moving bright object, moving thin curves, repeated moving glyph strokes, curved moving occluder, small moving foreground, x2/x4/x5 phases and intact sources"<<std::endl;return 0;
}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
