// Explicit GPU startup diagnostic: excludes device/decoder startup and frame work.
#define wmain original_gpu_smoke_entry
#include "gpu_engine_smoke.cpp"
#undef wmain
#include <chrono>
int wmain(int argc,wchar_t** argv) { try {
 require(argc==2,"native_startup <runtime>");
 ComPtr<IDXGIFactory1> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI");
 ComPtr<IDXGIAdapter1> adapter;
 for(UINT i=0;;++i) { ComPtr<IDXGIAdapter1> a; if(factory->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND)break;
  DXGI_ADAPTER_DESC1 d{};a->GetDesc1(&d);if(d.VendorId==0x10de){adapter=a;break;} }
 require(bool(adapter),"NVIDIA missing");
 ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
 D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
 check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_VIDEO_SUPPORT|D3D11_CREATE_DEVICE_BGRA_SUPPORT,levels,2,D3D11_SDK_VERSION,&device,nullptr,&context),"D3D11");
 for(int i=0;i<5;++i) {
  auto start=std::chrono::steady_clock::now();
  GpuFrucEngine engine(argv[1],device.Get(),context.Get(),nullptr,GpuCompletionMode::context_ordered,true,false,false,GpuInterpolationBackend::native_experimental);
  auto end=std::chrono::steady_clock::now();
  std::cout<<"native_constructor_ms="<<std::chrono::duration<double,std::milli>(end-start).count()<<'\n';
 }
 return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;} }
