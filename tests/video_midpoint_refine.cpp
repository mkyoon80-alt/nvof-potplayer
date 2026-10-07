// Explicit local NV12 quality diagnostic; CPU readback exists only in this test.
#define wmain old_gpu_entry
#include "gpu_engine_smoke.cpp"
#undef wmain
#include "nvof/fractional_refiner.hpp"
#include <fstream>
int wmain(int argc,wchar_t** argv){try{
 require(argc==7||argc==8,"video_midpoint_refine <input.nv12> <fallback.nv12> <output.nv12> <width> <height> <frames> [unprotected]");
 int w=std::stoi(argv[4]),h=std::stoi(argv[5]),frames=std::stoi(argv[6]);size_t bytes=size_t(w)*h*3/2;
 ComPtr<IDXGIFactory1> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI");ComPtr<IDXGIAdapter1> adapter;
 for(UINT i=0;;++i){ComPtr<IDXGIAdapter1>a;if(factory->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 d{};a->GetDesc1(&d);if(d.VendorId==0x10de){adapter=a;break;}}
 ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
 check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_VIDEO_SUPPORT,levels,2,D3D11_SDK_VERSION,&device,nullptr,&context),"D3D");
 FractionalRefiner refiner(device.Get(),context.Get(),argc==7||std::wstring(argv[7])==L"stats");
 const auto texture=[&](){D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=1;d.Format=DXGI_FORMAT_NV12;d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;ComPtr<ID3D11Texture2D> t;check(device->CreateTexture2D(&d,nullptr,&t),"texture");return t;};
 auto a=texture(),b=texture(),fallback=texture(),output=texture();
 ComPtr<ID3D11RenderTargetView> oy,ouv;ComPtr<ID3D11ShaderResourceView> fy,fuv;
 D3D11_RENDER_TARGET_VIEW_DESC rt{};rt.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;rt.Format=DXGI_FORMAT_R8_UNORM;check(device->CreateRenderTargetView(output.Get(),&rt,&oy),"Y RTV");rt.Format=DXGI_FORMAT_R8G8_UNORM;check(device->CreateRenderTargetView(output.Get(),&rt,&ouv),"UV RTV");
 D3D11_SHADER_RESOURCE_VIEW_DESC sv{};sv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sv.Texture2D.MipLevels=1;sv.Format=DXGI_FORMAT_R8_UNORM;check(device->CreateShaderResourceView(fallback.Get(),&sv,&fy),"Y SRV");sv.Format=DXGI_FORMAT_R8G8_UNORM;check(device->CreateShaderResourceView(fallback.Get(),&sv,&fuv),"UV SRV");
 std::ifstream input(std::filesystem::path(argv[1]),std::ios::binary),base(std::filesystem::path(argv[2]),std::ios::binary);std::ofstream out(std::filesystem::path(argv[3]),std::ios::binary);
 std::vector<uint8_t> pa(bytes),pb(bytes),pm(bytes);input.read((char*)pa.data(),bytes);double elapsed=0;int count=0;
 for(int i=0;i<frames-1;++i){input.read((char*)pb.data(),bytes);require(input.gcount()==bytes,"short input");base.seekg((2*size_t(i)+1)*bytes);base.read((char*)pm.data(),bytes);require(base.gcount()==bytes,"short fallback");
 out.write((char*)pa.data(),bytes);
 if(pa==pb||pm==pa){out.write((char*)pm.data(),bytes);refiner.invalidate_history();}else{
 context->UpdateSubresource(a.Get(),0,nullptr,pa.data(),w,UINT(bytes));context->UpdateSubresource(b.Get(),0,nullptr,pb.data(),w,UINT(bytes));context->UpdateSubresource(fallback.Get(),0,nullptr,pm.data(),w,UINT(bytes));
 auto start=std::chrono::steady_clock::now();refiner.prepare(a.Get(),b.Get(),w,h);refiner.render_midpoint(oy.Get(),ouv.Get(),fy.Get(),fuv.Get());if(argc==8&&std::wstring(argv[7])==L"stats"){auto evidence=refiner.diagnostic_motion_evidence();std::cout<<"EVIDENCE "<<i;for(auto value:evidence)std::cout<<" "<<value;std::cout<<std::endl;}auto pixels=readback(device.Get(),context.Get(),{output,0,w,h,0});elapsed+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();++count;out.write((char*)pixels.data(),bytes);
 }pa.swap(pb);
 }
 out.write((char*)pa.data(),bytes);out.write((char*)pa.data(),bytes);require(bool(out),"write failed");std::cout<<"PASS midpoint frames="<<count<<" grid="<<refiner.grid_size()<<" refine_and_test_readback_ms="<<elapsed/std::max(1,count)<<std::endl;
 return 0;
}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
