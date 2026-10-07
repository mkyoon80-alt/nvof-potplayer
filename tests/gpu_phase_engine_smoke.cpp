#define wmain legacy_gpu_engine_smoke_entry
#include "gpu_engine_smoke.cpp"
#undef wmain

#include "nvof/scene_cut.hpp"
#include <set>
#include <d3d10_1.h>
int wmain(int argc,wchar_t**argv){
 try {
  if(argc<2){std::cerr<<"gpu_phase_engine_smoke <runtime> [width height]\n";return 2;}
  const int w=argc>2?_wtoi(argv[2]):640,h=argc>3?_wtoi(argv[3]):360;
  ComPtr<IDXGIFactory1> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI");ComPtr<IDXGIAdapter1> adapter;
  for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> candidate;if(factory->EnumAdapters1(i,&candidate)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 d{};candidate->GetDesc1(&d);if(d.VendorId==0x10de){adapter=candidate;break;}}
  require(bool(adapter),"NVIDIA adapter missing");ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
  check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_VIDEO_SUPPORT|D3D11_CREATE_DEVICE_BGRA_SUPPORT,levels,2,D3D11_SDK_VERSION,&device,nullptr,&context),"D3D11 device");
  ComPtr<ID3D10Multithread> protected_context;context.As(&protected_context);if(protected_context)protected_context->SetMultithreadProtected(TRUE);
  D3D11_VIEWPORT state{11,13,123,145,0,1};context->RSSetViewports(1,&state);
  GpuFrucEngine engine(std::filesystem::path(argv[1]),device.Get(),context.Get(),nullptr,argc>4?GpuCompletionMode::context_ordered:GpuCompletionMode::blocking);
  const auto upload=[&](const std::vector<uint8_t>& data,int64_t pts){D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=1;d.ArraySize=1;d.Format=DXGI_FORMAT_NV12;d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;D3D11_SUBRESOURCE_DATA initial{data.data(),UINT(w),UINT(data.size())};ComPtr<ID3D11Texture2D> texture;check(device->CreateTexture2D(&d,&initial,&texture),"Test frame");return engine.copy({texture,0,w,h,pts});};
  // The appearance passes use high SRV/UAV slots on the shared context.
  D3D11_TEXTURE2D_DESC marker_desc{};marker_desc.Width=marker_desc.Height=4;marker_desc.MipLevels=marker_desc.ArraySize=1;marker_desc.Format=DXGI_FORMAT_R32_FLOAT;marker_desc.SampleDesc.Count=1;marker_desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  ComPtr<ID3D11Texture2D> marker_input,marker_output;check(device->CreateTexture2D(&marker_desc,nullptr,&marker_input),"state input");
  marker_desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;check(device->CreateTexture2D(&marker_desc,nullptr,&marker_output),"state output");
  ComPtr<ID3D11ShaderResourceView> marker_srv;ComPtr<ID3D11UnorderedAccessView> marker_uav;
  check(device->CreateShaderResourceView(marker_input.Get(),nullptr,&marker_srv),"state SRV");check(device->CreateUnorderedAccessView(marker_output.Get(),nullptr,&marker_uav),"state UAV");
  auto state_srv=marker_srv.Get();auto state_uav=marker_uav.Get();context->CSSetShaderResources(11,1,&state_srv);context->CSSetUnorderedAccessViews(4,1,&state_uav,nullptr);
  const auto compute_state=[&](){ComPtr<ID3D11ShaderResourceView> srv;ComPtr<ID3D11UnorderedAccessView> uav;context->CSGetShaderResources(11,1,&srv);context->CSGetUnorderedAccessViews(4,1,&uav);require(srv.Get()==marker_srv.Get()&&uav.Get()==marker_uav.Get(),"Appearance pass did not restore renderer compute bindings");};
  auto a=pixels(w,h,0),b=pixels(w,h,10);auto left=upload(a,0),right=upload(b,1000000);
  auto batch=engine.interpolate_pair(left,right,{200000,400000,600000,800000});validate_state(context.Get());
  require(batch.quality.repetition_known,"Missing actual FRUC repetition metadata");require(batch.quality.subpixel_refined_mask==(NVOF_ENABLE_EXPERIMENTAL_SUBPIXEL?15u:0u)&&!batch.quality.subpixel_unavailable,"Native fractional refinement does not match the explicit build option");require(!batch.quality.scene_cut&&!batch.quality.repeated_mask,"Coherent motion unexpectedly rejected");require(batch.frames.size()==4,"Missing phases");
  std::vector<std::vector<uint8_t>> images;
  for(size_t i=0;i<4;++i){auto image=readback(device.Get(),context.Get(),batch.frames[i]);auto truth=pixels(w,h,int(i+1)*2);auto blend=a;for(size_t p=0;p<a.size();++p)blend[p]=uint8_t((unsigned(a[p])*(4-i)+unsigned(b[p])*(i+1)+2)/5);
   const auto error=mse(image,truth,w,h),blend_error=mse(blend,truth,w,h);std::cout<<"PHASE "<<(i+1)*20<<" MSE "<<error<<" BLEND_MSE "<<blend_error<<'\n';require(error<blend_error,"Motion phase did not improve known translation over blend");require(image!=a&&image!=b,"Phase repeated original");images.push_back(std::move(image));}
  for(size_t i=0;i<4;++i)for(size_t j=i+1;j<4;++j)require(images[i]!=images[j],"Motion phases were duplicate");
  auto middle=engine.interpolate_pair(left,right,{500000});
  require(middle.quality.subpixel_refined_mask==0&&!middle.quality.subpixel_unavailable,"Midpoint accidentally enabled arbitrary-phase refinement");
  require(middle.quality.midpoint_stabilized_mask==1&&!middle.quality.midpoint_stabilization_unavailable,"Midpoint stability pass missing");
  compute_state();
  if(int64_t(w)*h>1920LL*1080){
   engine.reset();auto fast_a=left,fast_b=right;fast_a.pts=0;fast_b.pts=166833;
   auto bounded=engine.interpolate_pair(fast_a,fast_b,{83416});
   require(bounded.quality.midpoint_budget_limited&&bounded.quality.midpoint_stabilized_mask==0,"Large high-rate picture exceeded stability budget");
   engine.reset();fast_b.pts=417083;
   auto film=engine.interpolate_pair(fast_a,fast_b,{208541});
   require(!film.quality.midpoint_budget_limited&&film.quality.midpoint_stabilized_mask==1,"Large film-rate midpoint did not stabilize");
   engine.reset();fast_b.pts=333333;auto thirty=engine.interpolate_pair(fast_a,fast_b,{166666});
   require(!thirty.quality.midpoint_budget_limited&&thirty.quality.midpoint_stabilized_mask==1,"4K 30p correction was incorrectly bypassed");
   compute_state();
  }

  auto held=batch.frames[0];auto held_bytes=images[0];batch.frames.clear();
  std::vector<double> costs;auto previous=right;
  for(int i=2;i<22;++i){auto data=pixels(w,h,i*10);const auto start=std::chrono::steady_clock::now();auto next=upload(data,int64_t(i)*1000000);std::vector<int64_t> times;for(int p=1;p<5;++p)times.push_back(previous.pts+p*200000);auto generated=engine.interpolate_pair(previous,next,times);require(!generated.quality.scene_cut,"Translation classified as cut");if(i>4)costs.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());previous=next;}
  std::sort(costs.begin(),costs.end());std::cout<<"GPU_PAIR_4_PHASES "<<w<<'x'<<h<<" MEAN_MS "<<std::accumulate(costs.begin(),costs.end(),0.0)/costs.size()<<" P95_MS "<<costs[size_t((costs.size()-1)*.95)]<<" (input snapshot + detector + FRUC + output; excludes decoder/VSR)\n";
  require(readback(device.Get(),context.Get(),held)==held_bytes,"Output pool overwrote held renderer frame");
  engine.reset();auto cut_a=a,cut_b=b;
  for(size_t i=0;i<size_t(w)*h;++i){const auto block=unsigned((std::max)(32,w/20));const auto x=unsigned(i%w),y=unsigned(i/w);cut_a[i]=uint8_t(16+hash((x/block)*733U+(y/block)*193U)%96);cut_b[i]=uint8_t(176+hash((x/block)*313U+(y/block)*977U+1U)%60);}
  auto cut_left=upload(cut_a,0),cut_right=upload(cut_b,1000000);auto cut=engine.interpolate_pair(cut_left,cut_right,{200000,400000,600000,800000});
  require(cut.quality.scene_cut&&cut.frames.empty(),"Hard cut did not bypass motion generation");
  auto later=cut_right;later.pts=2000000;auto resumed=engine.interpolate_pair(cut_right,later,{1200000,1400000,1600000,1800000});require(!resumed.quality.scene_cut,"Cut recovery failed");
  auto flash=a;for(size_t i=0;i<size_t(w)*h;++i)flash[i]=uint8_t((std::min)(255,int(flash[i])+25));auto flash_result=engine.interpolate_pair(upload(a,0),upload(flash,1000000),{500000});require(!flash_result.quality.scene_cut,"Uniform flash misclassified as hard cut");
  auto saw=a;for(int y=0;y<h;++y)for(int x=0;x<w;++x)saw[size_t(y)*w+x]=uint8_t((x*3+y*5)&255);
  for(int y=0;y<h/2;++y)for(int x=0;x<w;x+=2){saw[size_t(w)*h+size_t(y)*w+x]=uint8_t((x*7+y*9)&255);saw[size_t(w)*h+size_t(y)*w+x+1]=uint8_t((x*11+y*3)&255);}
  for(const auto& colors:{color_bars(w,h),gray_ramp(w,h,true),chroma_detail(w,h),saw}){
   engine.reset();auto ca=upload(colors,0),cb=upload(colors,1000000);auto static_batch=engine.interpolate_pair(ca,cb,{200000,400000,600000,800000});require(!static_batch.quality.scene_cut,"Static color cut");require(static_batch.quality.identical_warp_skipped&&!static_batch.quality.repetition_known,"Identical warp not skipped or repetition metadata fabricated");require(static_batch.quality.subpixel_refined_mask==0,"Identical originals unnecessarily refined");
   for(auto& output:static_batch.frames)require(readback(device.Get(),context.Get(),output)==colors,"Static NV12 color not byte-exact");
  }
  for(size_t offset:{size_t(w)*10+12,size_t(w)*h+12,size_t(w)*h+13}){
   engine.reset();auto changed=a;changed[offset]^=1;auto nonidentical=engine.interpolate_pair(upload(a,0),upload(changed,1000000),{500000});
   require(!nonidentical.quality.identical_warp_skipped,"Near-identical Y/UV treated as exact duplicate");
  }
  for(int i=0;i<4;++i){engine.reset();auto re=engine.interpolate_pair(left,right,{200000,400000,600000,800000});require(re.frames.size()==4&&!re.quality.scene_cut&&!re.quality.repeated_mask,"Seek re-prime failed");for(size_t p=0;p<4;++p){auto actual=readback(device.Get(),context.Get(),re.frames[p]);auto truth=pixels(w,h,int(p+1)*2);require(actual!=a&&actual!=b&&mse(actual,truth,w,h)<10,"Seek re-prime returned an original instead of motion");}}
  require(readback(device.Get(),context.Get(),held)==held_bytes,"Reset corrupted retained output");
  validate_state(context.Get());engine.reset();compute_state();std::cout<<"PASS: four genuine phases, shared protected context, quality flag, scene cut/recovery, flash rejection, static color exact, retained pool lease, seek/reset, shutdown\n";return 0;
 }catch(const std::exception&e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
