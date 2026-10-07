#define wmain old_gpu_test_entry
#include "gpu_engine_smoke.cpp"
#undef wmain
#include <d3d10_1.h>
#include <iomanip>

int wmain(int argc,wchar_t** argv){try{
 require(argc>=2,"gpu_x2_benchmark <runtime> [width height pairs rounds hold_frames skip_identical]");
 const int w=argc>2?_wtoi(argv[2]):1920,h=argc>3?_wtoi(argv[3]):1080;
 const int count=argc>4?_wtoi(argv[4]):36,rounds=argc>5?_wtoi(argv[5]):3;
 const int hold=argc>6?_wtoi(argv[6]):1;
 const bool quality_compare=argc>9&&std::wstring(argv[9])==L"stability";
 require(hold>=1&&hold<=1000,"Invalid hold count");
 require(count>=12&&count<=120&&rounds>=1&&rounds<=5,"Invalid benchmark bounds");
 ComPtr<IDXGIFactory1> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI");ComPtr<IDXGIAdapter1> adapter;
 for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> candidate;if(factory->EnumAdapters1(i,&candidate)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 d{};candidate->GetDesc1(&d);if(d.VendorId==0x10de){adapter=candidate;break;}}
 require(bool(adapter),"NVIDIA missing");ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
 check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_VIDEO_SUPPORT|D3D11_CREATE_DEVICE_BGRA_SUPPORT,levels,2,D3D11_SDK_VERSION,&device,nullptr,&context),"D3D");
 ComPtr<ID3D10Multithread> lock;check(context.As(&lock),"context protection");lock->SetMultithreadProtected(TRUE);
 std::vector<GpuFrame> source;
 for(int i=0;i<=count;++i){const int shift=(i/hold)*(quality_compare?1:4);auto bytes=pixels(w,h,shift);
 for(int y=0;y<h/2;++y)for(int x=0;x<w;x+=2){
  const auto at=size_t(w)*h+size_t(y)*w+x;
  bytes[at]=uint8_t(128+48*std::sin((x-shift)*.025)+16*std::sin(y*.08));
  bytes[at+1]=uint8_t(128+48*std::cos((x-shift)*.03)+16*std::cos(y*.06));
 }
D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=1;d.Format=DXGI_FORMAT_NV12;d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;D3D11_SUBRESOURCE_DATA initial{bytes.data(),UINT(w),UINT(bytes.size())};ComPtr<ID3D11Texture2D> texture;check(device->CreateTexture2D(&d,&initial,&texture),"input");source.push_back({texture,0,w,h,int64_t(i)*1000000});}
 readback(device.Get(),context.Get(),source.back());
 std::vector<std::vector<uint8_t>> reference;
 std::cout<<std::fixed<<std::setprecision(4);
 for(int round=0;round<rounds;++round)for(int order=0;order<2;++order){
  const bool optimized=(order+(round&1))%2;
  const bool control=argc>8&&!quality_compare;
  GpuFrucEngine engine(std::filesystem::path(argv[1]),device.Get(),context.Get(),nullptr,(quality_compare||optimized&&!control)?GpuCompletionMode::context_ordered:GpuCompletionMode::blocking,quality_compare||optimized&&argc>7&&!control,!quality_compare||optimized);
  std::vector<double> submit,held_cost,motion_cost;auto previous=engine.copy(source[0]);GpuFrame last;std::vector<GpuFrame> outputs;size_t skipped=0;
  auto total=std::chrono::steady_clock::now();
  for(int i=1;i<=count;++i){
   const auto start=std::chrono::steady_clock::now();auto next=engine.copy(source[i]);auto batch=engine.interpolate_pair(previous,next,{next.pts-500000});
   require(!batch.quality.scene_cut&&!batch.quality.repeated_mask&&batch.frames.size()==1,"Midpoint unexpectedly rejected");
   if(i>5){const auto cost=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();submit.push_back(cost);
    ((i/hold)==((i-1)/hold)?held_cost:motion_cost).push_back(cost);
   }
   last=batch.frames[0];previous=next;outputs.push_back(last);skipped+=batch.quality.identical_warp_skipped;
   if(i==5)total=std::chrono::steady_clock::now();
  }
  auto final_bytes=readback(device.Get(),context.Get(),last);
  double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-total).count()/(count-5);
  uint64_t changed=0,absolute=0;int maximum=0;
  for(size_t i=0;i<outputs.size();++i){auto bytes=readback(device.Get(),context.Get(),outputs[i]);
   if(!optimized&&round==0)reference.push_back(bytes);
   else {uint64_t different=0,inner=0,uv=0;int localmax=0;double mse_diff=0;
    for(size_t k=0;k<bytes.size();++k){const int d=std::abs(int(bytes[k])-int(reference[i][k]));changed+=d!=0;absolute+=d;maximum=(std::max)(maximum,d);different+=d!=0;localmax=(std::max)(localmax,d);mse_diff+=d*d;
     if(d){const int x=int(k%w),y=int(k/w);uv+=y>=h;if(x>=64&&x<w-64&&y>=32&&y<h-32)++inner;}
    }
    // Independent unmodified FRUC sessions also vary at sparse image
    // boundaries. Identical pictures still require exact stored bytes.
    const bool identical=((i+1)/hold)==(i/hold);
    if(identical)require(!different,"Identical frame was modified");
    if(!quality_compare)require(double(different)/bytes.size()<.0002&&mse_diff/bytes.size()<.005&&inner<8,
            "Moving output exceeded baseline FRUC variation bound");
   }
  }
  std::cout<<"X2_PIXELS round="<<round<<" optimized="<<optimized<<" frames="<<outputs.size()<<" hold="<<hold<<" changed_bytes="<<changed<<" mean_byte_difference="<<double(absolute)/(size_t(w)*h*3/2*outputs.size())<<" max_difference="<<maximum<<" skipped="<<skipped<<std::endl;

  const auto average=[](const std::vector<double>& v){return v.empty()?0.:std::accumulate(v.begin(),v.end(),0.)/v.size();};
  std::cout<<"X2_CATEGORY held_ms="<<average(held_cost)<<" motion_ms="<<average(motion_cost)<<std::endl;
  std::sort(submit.begin(),submit.end());std::cout<<"X2_PERF round="<<round<<" path="<<"cuda-plane-interop"<<" hold="<<hold<<" skipped="<<skipped<<" stability_compare="<<quality_compare<<" stability="<<(!quality_compare||optimized)<<" queued="<<engine.queued_completion()<<" size="<<w<<'x'<<h<<" pairs="<<submit.size()<<" submit_mean_ms="<<std::accumulate(submit.begin(),submit.end(),0.0)/submit.size()<<" p95_ms="<<submit[size_t((submit.size()-1)*.95)]<<" drained_mean_ms="<<elapsed<<std::endl;
 }
 std::cout<<"PASS x2 performance, static byte equality, conditional legacy moving Y/UV bounds, retained outputs\n";return 0;
}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
