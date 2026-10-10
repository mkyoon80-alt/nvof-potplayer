// Local diagnosis: dump GPU masks and source coordinates for a supplied NV12 pair.
#define wmain original_gpu_smoke_entry
#include "gpu_engine_smoke.cpp"
#undef wmain
#include <fstream>
#include <filesystem>
#include "../src/motion_synthesizer.cpp"
namespace nvof {
struct MotionSessionTestAccess {
 static void dump(MotionSynthesizer& synth,const std::filesystem::path& folder) {
  auto& s=*synth.impl_;
  auto save=[&](ID3D11Texture2D* texture,const char* name,int bytes) {
   D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=d.MiscFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
   ComPtr<ID3D11Texture2D> copy;nvof::check(s.device->CreateTexture2D(&d,nullptr,&copy),"Staging");s.context->CopyResource(copy.Get(),texture);
   D3D11_MAPPED_SUBRESOURCE m{};nvof::check(s.context->Map(copy.Get(),0,D3D11_MAP_READ,0,&m),"Readback");std::ofstream f(folder/name,std::ios::binary);
   for(unsigned y=0;y<d.Height;++y)f.write(static_cast<const char*>(m.pData)+y*m.RowPitch,d.Width*bytes);
   s.context->Unmap(copy.Get(),0);
  };
  save(s.glyphTexture.Get(),"glyph.bin",16);
  save(s.layerTextures[0].Get(),"mask.bin",2);
  save(s.warpMaps[0].Get(),"offsets.bin",16);save(s.warpMaps[1].Get(),"weights.bin",16);
  save(s.flows[0].Get(),"raw-fw.bin",4);save(s.flows[1].Get(),"raw-bw.bin",4);
  save(s.repairedTextures[0].Get(),"fw.bin",4);save(s.repairedTextures[1].Get(),"bw.bin",4);
 }
};
}
int wmain(int argc,wchar_t** argv){try {
 require(argc==5,"native_overlay_probe <pair.nv12> <output-folder> <width> <height>");
 int w=std::stoi(argv[3]),h=std::stoi(argv[4]);require(w>0&&h>0&&w%2==0&&h%2==0,"Dimensions");
 std::filesystem::path folder=argv[2];std::filesystem::create_directories(folder);
 ComPtr<IDXGIFactory1> factory;nvof::check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI");ComPtr<IDXGIAdapter1> adapter;
 for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> a;if(factory->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 d{};a->GetDesc1(&d);if(d.VendorId==0x10de){adapter=a;break;}}
 require(bool(adapter),"NVIDIA required");ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
 nvof::check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"Device");
 std::ifstream f(argv[1],std::ios::binary);std::vector<uint8_t> raw(size_t(w)*h*3);f.read(reinterpret_cast<char*>(raw.data()),raw.size());require(size_t(f.gcount())==raw.size(),"Two complete frames required");
 ComPtr<ID3D11Texture2D> sources[2];
 D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=1;d.Format=DXGI_FORMAT_NV12;d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
 for(int i=0;i<2;++i){D3D11_SUBRESOURCE_DATA initial{raw.data()+size_t(i)*w*h*3/2,UINT(w),0};nvof::check(device->CreateTexture2D(&d,&initial,&sources[i]),"Source");}
 MotionSynthesizer synth(device.Get(),context.Get());synth.prepare(sources[0].Get(),sources[1].Get(),w,h);require(synth.reliable(),"Unreliable pair");
 d.BindFlags=D3D11_BIND_RENDER_TARGET;ComPtr<ID3D11Texture2D> output;nvof::check(device->CreateTexture2D(&d,nullptr,&output),"Output");
 ComPtr<ID3D11RenderTargetView> y,uv;D3D11_RENDER_TARGET_VIEW_DESC v{};v.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;v.Format=DXGI_FORMAT_R8_UNORM;
 nvof::check(device->CreateRenderTargetView(output.Get(),&v,&y),"Y RTV");v.Format=DXGI_FORMAT_R8G8_UNORM;nvof::check(device->CreateRenderTargetView(output.Get(),&v,&uv),"UV RTV");
 synth.render_midpoint(y.Get(),uv.Get());nvof::MotionSessionTestAccess::dump(synth,folder);
 std::cout<<"PASS probe"<<std::endl;return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
