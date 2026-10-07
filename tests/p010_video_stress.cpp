// Local media is supplied explicitly; no media is checked in or distributed.
#define wmain old_interop_stress_entry
#include "gpu_interop_stress.cpp"
#undef wmain
#include "nvof/gpu_phase_pipeline.hpp"
#include <fstream>
#include <deque>
int wmain(int argc,wchar_t**argv){try{
 require(argc>=3,"p010_video_stress <runtime> <raw p010> [blocking|queued] [extras|basic] [p010|nv12] [concurrent|single]");
 const bool queued=argc<4||std::wstring(argv[3])==L"queued";
 const bool extras=argc<5||std::wstring(argv[4])==L"extras";
 const bool ten=argc<6||std::wstring(argv[5])==L"p010";
 const bool concurrent=argc<7||std::wstring(argv[6])==L"concurrent";
 constexpr int w=1920,h=1080,padded=1152,arrays=22;const size_t words=size_t(w)*h*3/2;
 std::ifstream input(std::filesystem::path(argv[2]),std::ios::binary);require(bool(input),"Open P010 input");
 std::vector<std::vector<uint16_t>> frames;
 for(;;){std::vector<uint16_t> f(words);input.read(reinterpret_cast<char*>(f.data()),words*2);if(!input.gcount())break;require(input.gcount()==words*2,"Short frame");frames.push_back(std::move(f));}
 require(frames.size()>10,"Need at least eleven frames");
 ComPtr<IDXGIFactory1> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI");ComPtr<IDXGIAdapter1> adapter;
 for(UINT i=0;;++i){ComPtr<IDXGIAdapter1>a;if(factory->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 d{};a->GetDesc1(&d);if(d.VendorId==0x10de){adapter=a;break;}}
 require(bool(adapter),"NVIDIA missing");ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
 check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_VIDEO_SUPPORT|D3D11_CREATE_DEVICE_BGRA_SUPPORT,levels,2,D3D11_SDK_VERSION,&device,nullptr,&context),"D3D");
 ComPtr<ID3D10Multithread> protection;check(context.As(&protection),"Protection");protection->SetMultithreadProtected(TRUE);
 std::unique_ptr<ConcurrentRenderer> renderer;if(concurrent)renderer=std::make_unique<ConcurrentRenderer>(device.Get(),context.Get(),protection.Get(),nullptr);
 GpuFrucEngine engine(argv[1],device.Get(),context.Get(),nullptr,queued?nvof::GpuCompletionMode::context_ordered:nvof::GpuCompletionMode::blocking,true,extras,extras);
 D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=padded;d.MipLevels=1;d.ArraySize=arrays;d.Format=ten?DXGI_FORMAT_P010:DXGI_FORMAT_NV12;d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_DECODER|D3D11_BIND_SHADER_RESOURCE;d.MiscFlags=D3D11_RESOURCE_MISC_SHARED;
 ComPtr<ID3D11Texture2D> texture;check(device->CreateTexture2D(&d,nullptr,&texture),"Decoder array");
 std::vector<uint8_t> upload(size_t(w)*padded*3/2*(ten?2:1));std::deque<GpuFrame> outputs;
 int count=0,pairs=0,emitted=0;
 nvof::GpuPhasePipeline pipeline({48000,1001},[&](const GpuFrame&a,const GpuFrame&b,const std::vector<int64_t>&times){++pairs;return engine.interpolate_pair(a,b,times);},{24000,1001});
 const auto emit=[&](const nvof::GpuOutputFrame& f){outputs.push_back(f.frame);if(outputs.size()>6)outputs.pop_front();++emitted;return true;};
 for(int round=0;round<4;++round){
  pipeline.reset();engine.reset();outputs.clear();
  for(size_t i=0;i<frames.size();++i){
   const auto& f=frames[i];for(int y=0;y<h*3/2;++y){const size_t row=size_t(y<h?y:padded+y-h)*w;if(ten)memcpy(upload.data()+row*2,f.data()+size_t(y)*w,w*2);else for(int x=0;x<w;++x)upload[row+x]=uint8_t((std::min)(255u,((unsigned(f[size_t(y)*w+x])>>6)+2u)>>2));}
   const UINT slice=UINT(i%arrays);{ContextLock lock(protection.Get(),nullptr);context->UpdateSubresource(texture.Get(),slice,nullptr,upload.data(),w*(ten?2:1),0);}
   if(renderer)renderer->stage("P010 capture");auto captured=engine.copy({texture,slice,w,h,int64_t(i)*10010000/24});
   if(renderer)renderer->stage("P010 interpolate");pipeline.push(captured,i==0,emit);++count;
   if(renderer)renderer->rethrow_failure();if(i%8==0)std::cout<<"frame="<<count<<" output="<<emitted<<std::endl;
  }
  pipeline.finish(417083,emit);
 }
 if(renderer){renderer->stop();renderer->rethrow_failure();}check(device->GetDeviceRemovedReason(),"Device removed");
 std::cout<<"PASS frames="<<count<<" output="<<emitted<<" pairs="<<pairs<<" queued="<<queued<<" extras="<<extras<<" p010="<<ten<<" concurrent="<<concurrent<<std::endl;return 0;
}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
