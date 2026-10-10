// Local video regression: raw frames remain in ignored build output only.
#define wmain old_gpu_test_entry
#include "gpu_engine_smoke.cpp"
#undef wmain
#include "nvof/gpu_phase_pipeline.hpp"
#include <d3d10_1.h>
#include <fstream>
int wmain(int argc,wchar_t** argv){try{
 require(argc>=5,"video_x2_repro <runtime> <raw nv12> <timestamps> <output folder> [baseline|native] [nominal-duration-100ns] [nv12|p010] [width height] [flow-dimension] [cost-mode: 0=off 1=blend 2=fusion] [grid: 0=auto 4 2 1] [medium|slow]");
 const int w=argc>8?std::stoi(argv[8]):1920,h=argc>9?std::stoi(argv[9]):1080;
 require(w>=4&&h>=4&&w<=8192&&h<=8192&&!(w&1)&&!(h&1),"Invalid raw dimensions");
 const unsigned flowDimension=argc>10?std::stoul(argv[10]):1920;
 const unsigned costMode=argc>11?std::stoul(argv[11]):0;
 require(costMode<=2,"Invalid cost mode");
 const unsigned grid=argc>12?std::stoul(argv[12]):0;
 const std::wstring quality=argc>13?argv[13]:L"medium";
 require(quality==L"medium"||quality==L"slow","Invalid flow quality");
 const MotionFlowOptions options{grid,quality==L"slow"?MotionFlowQuality::slow:MotionFlowQuality::medium};
 const int64_t duration=argc>6?std::stoll(argv[6]):417083;
 const bool p010=argc>7&&std::wstring(argv[7])==L"p010";
 const size_t bytes=size_t(w)*h*3/2*(p010?2:1);
 const bool native=argc>5&&std::wstring(argv[5])==L"native";
 const bool optimized=argc<6||native;const auto directory=std::filesystem::path(argv[4]);std::filesystem::create_directories(directory);
 ComPtr<IDXGIFactory1> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI");ComPtr<IDXGIAdapter1> adapter;
 for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> a;if(factory->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 d{};a->GetDesc1(&d);if(d.VendorId==0x10de){adapter=a;break;}}
 require(bool(adapter),"NVIDIA missing");ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
 check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_VIDEO_SUPPORT|D3D11_CREATE_DEVICE_BGRA_SUPPORT,levels,2,D3D11_SDK_VERSION,&device,nullptr,&context),"D3D");
 ComPtr<ID3D10Multithread> lock;check(context.As(&lock),"context protection");lock->SetMultithreadProtected(TRUE);
 GpuFrucEngine engine(std::filesystem::path(argv[1]),device.Get(),context.Get(),nullptr,optimized?GpuCompletionMode::context_ordered:GpuCompletionMode::blocking,optimized,optimized&&!native,optimized&&!native,native?GpuInterpolationBackend::native_experimental:GpuInterpolationBackend::fruc,flowDimension,static_cast<MotionCostMode>(costMode),options);
 std::ifstream input(std::filesystem::path(argv[2]),std::ios::binary),timestamps{std::filesystem::path(argv[3])};require(bool(input)&&bool(timestamps),"Input missing");
 std::ofstream raw(directory/L"output.nv12",std::ios::binary),events(directory/L"pairs.csv"),outtimes(directory/L"output.csv");
 events<<"source_index,previous_pts,current_pts,phase_pts,scene_cut,repeated_mask,identical_skipped,midpoint_pass,process_ms,completed_ms,analysis_width,analysis_height,grid,cost_buffers\n";
 outtimes<<"output_index,pts,stop,discontinuity\n";
 D3D11_QUERY_DESC queryDesc{D3D11_QUERY_EVENT,0};ComPtr<ID3D11Query> complete;
 check(device->CreateQuery(&queryDesc,&complete),"Benchmark completion query");
 int source_index=0,output_index=0,cut=0,repeated=0,skipped=0,originals=0;std::vector<uint8_t> current(bytes);
 GpuPhasePipeline pipeline(double_source_rate(duration),[&](const GpuFrame&a,const GpuFrame&b,const std::vector<int64_t>& times){
  const auto start=std::chrono::steady_clock::now();auto batch=engine.interpolate_pair(a,b,times);
  const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
  context->End(complete.Get());context->Flush();
  const auto deadline=GetTickCount64()+10000;
  for(;;){auto status=context->GetData(complete.Get(),nullptr,0,0);check(status,"Benchmark completion");if(status==S_OK)break;require(GetTickCount64()<deadline,"Benchmark completion timed out");SwitchToThread();}
  const auto completedMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
  const auto info=engine.analysis_info();
  require(costMode!=0||info.cost_buffers==0,"Disabled cost was allocated");
  require(!info.grid||!grid||info.grid==grid,"Requested grid was not applied");
  events<<source_index<<','<<a.pts<<','<<b.pts<<','<<(times.empty()?-1:times[0])<<','<<batch.quality.scene_cut<<','<<batch.quality.repeated_mask<<','<<batch.quality.identical_warp_skipped<<','<<batch.quality.midpoint_stabilized_mask<<','<<ms<<','<<completedMs<<','<<info.width<<','<<info.height<<','<<info.grid<<','<<info.cost_buffers<<'\n';
  cut+=batch.quality.scene_cut;repeated+=batch.quality.repeated_mask!=0;skipped+=batch.quality.identical_warp_skipped;
  return batch;
 },canonical_source_rate(duration));
 GpuFrame captured;bool capturedChecked=false;std::vector<uint8_t> capturedBytes;int64_t previousStop=-1;
 const auto emit=[&](const GpuOutputFrame& f){auto image=readback(device.Get(),context.Get(),f.frame);raw.write(reinterpret_cast<const char*>(image.data()),image.size());
  require(f.stop>f.frame.pts&&(previousStop<0||f.frame.pts==previousStop),"Output clock gap/overlap");previousStop=f.stop;
  if(f.frame.texture.Get()==captured.texture.Get()) {require(image==capturedBytes,"Original GPU capture changed");if(!capturedChecked){++originals;capturedChecked=true;}}
  outtimes<<output_index<<','<<f.frame.pts<<','<<f.stop<<','<<f.discontinuity<<'\n';++output_index;return bool(raw);
 };
 int64_t pts=0;
 while(timestamps>>pts){input.read(reinterpret_cast<char*>(current.data()),bytes);require(input.gcount()==bytes,"Short raw frame");
  D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=1;d.Format=p010?DXGI_FORMAT_P010:DXGI_FORMAT_NV12;d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA initial{current.data(),UINT(w*(p010?2:1)),UINT(bytes)};ComPtr<ID3D11Texture2D> texture;check(device->CreateTexture2D(&d,&initial,&texture),"input frame");
  captured=engine.copy({texture,0,w,h,pts});capturedChecked=false;capturedBytes=readback(device.Get(),context.Get(),captured);
  pipeline.push(captured,source_index==0,emit);++source_index;
 }
 pipeline.finish(duration,emit);engine.reset();require(bool(raw),"Output write failure");
 std::cout<<"PASS source="<<source_index<<" output="<<output_index<<" originals_exact="<<originals<<" cuts="<<cut<<" repeats="<<repeated<<" skipped="<<skipped<<" optimized="<<optimized<<" p010="<<p010<<" nominal_duration="<<duration<<std::endl;return 0;
}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
