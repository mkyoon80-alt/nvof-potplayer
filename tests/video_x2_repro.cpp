// Local video regression: raw frames remain in ignored build output only.
#define wmain old_gpu_test_entry
#include "gpu_engine_smoke.cpp"
#undef wmain
#include "nvof/gpu_phase_pipeline.hpp"
#include <d3d10_1.h>
#include <fstream>
int wmain(int argc,wchar_t** argv){try{
 require(argc>=5,"video_x2_repro <runtime> <raw nv12> <timestamps> <output folder> [baseline]");
 const int w=1920,h=1080;const size_t bytes=size_t(w)*h*3/2;
 const bool optimized=argc<6;const auto directory=std::filesystem::path(argv[4]);std::filesystem::create_directories(directory);
 ComPtr<IDXGIFactory1> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI");ComPtr<IDXGIAdapter1> adapter;
 for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> a;if(factory->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 d{};a->GetDesc1(&d);if(d.VendorId==0x10de){adapter=a;break;}}
 require(bool(adapter),"NVIDIA missing");ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
 check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_VIDEO_SUPPORT|D3D11_CREATE_DEVICE_BGRA_SUPPORT,levels,2,D3D11_SDK_VERSION,&device,nullptr,&context),"D3D");
 ComPtr<ID3D10Multithread> lock;check(context.As(&lock),"context protection");lock->SetMultithreadProtected(TRUE);
 GpuFrucEngine engine(std::filesystem::path(argv[1]),device.Get(),context.Get(),nullptr,optimized?GpuCompletionMode::context_ordered:GpuCompletionMode::blocking,optimized,optimized);
 std::ifstream input(std::filesystem::path(argv[2]),std::ios::binary),timestamps{std::filesystem::path(argv[3])};require(bool(input)&&bool(timestamps),"Input missing");
 std::ofstream raw(directory/L"output.nv12",std::ios::binary),events(directory/L"pairs.csv"),outtimes(directory/L"output.csv");
 events<<"source_index,previous_pts,current_pts,phase_pts,scene_cut,repeated_mask,identical_skipped,midpoint_pass,process_ms\n";
 outtimes<<"output_index,pts,stop,discontinuity\n";
 int source_index=0,output_index=0,cut=0,repeated=0,skipped=0,originals=0;std::vector<uint8_t> current(bytes);
 GpuPhasePipeline pipeline({48000,1001},[&](const GpuFrame&a,const GpuFrame&b,const std::vector<int64_t>& times){
  const auto start=std::chrono::steady_clock::now();auto batch=engine.interpolate_pair(a,b,times);
  const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
  events<<source_index<<','<<a.pts<<','<<b.pts<<','<<(times.empty()?-1:times[0])<<','<<batch.quality.scene_cut<<','<<batch.quality.repeated_mask<<','<<batch.quality.identical_warp_skipped<<','<<batch.quality.midpoint_stabilized_mask<<','<<ms<<'\n';
  cut+=batch.quality.scene_cut;repeated+=batch.quality.repeated_mask!=0;skipped+=batch.quality.identical_warp_skipped;
  return batch;
 },{24000,1001});
 const auto emit=[&](const GpuOutputFrame& f){auto image=readback(device.Get(),context.Get(),f.frame);raw.write(reinterpret_cast<const char*>(image.data()),image.size());
  if((output_index&1)==0){require(image==current,"Original source altered or wrong frame order");++originals;}
  outtimes<<output_index<<','<<f.frame.pts<<','<<f.stop<<','<<f.discontinuity<<'\n';++output_index;return bool(raw);
 };
 int64_t pts=0;
 while(timestamps>>pts){input.read(reinterpret_cast<char*>(current.data()),bytes);require(input.gcount()==bytes,"Short raw frame");
  D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=1;d.Format=DXGI_FORMAT_NV12;d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA initial{current.data(),UINT(w),UINT(bytes)};ComPtr<ID3D11Texture2D> texture;check(device->CreateTexture2D(&d,&initial,&texture),"input frame");
  pipeline.push(engine.copy({texture,0,w,h,pts}),source_index==0,emit);++source_index;
 }
 pipeline.finish(417083,emit);engine.reset();require(bool(raw),"Output write failure");
 std::cout<<"PASS source="<<source_index<<" output="<<output_index<<" originals_exact="<<originals<<" cuts="<<cut<<" repeats="<<repeated<<" skipped="<<skipped<<" optimized="<<optimized<<std::endl;return 0;
}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
