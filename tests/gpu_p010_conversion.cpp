#include "nvof/gpu_engine.hpp"
#include "nvof/mf_bridge.hpp"
#include <iostream>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <cstring>
#include <d3d10_1.h>
#include <cmath>
using Microsoft::WRL::ComPtr;
using namespace nvof;
namespace {
void check(HRESULT hr,const char* action){if(FAILED(hr))throw std::runtime_error(std::string(action)+" hr="+std::to_string(unsigned(hr)));}
void require(bool ok,const char* what){if(!ok)throw std::runtime_error(what);}
std::vector<uint16_t> download(ID3D11Device* d,ID3D11DeviceContext* c,const GpuFrame& frame){
 D3D11_TEXTURE2D_DESC desc{};frame.texture->GetDesc(&desc);
 require(desc.Format==DXGI_FORMAT_P010&&desc.Width==UINT(frame.width)&&desc.Height==UINT(frame.height),"Output is not visible-size P010");
 desc.BindFlags=0;desc.MiscFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
 ComPtr<ID3D11Texture2D> staging;check(d->CreateTexture2D(&desc,nullptr,&staging),"Create test readback");
 c->CopyResource(staging.Get(),frame.texture.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
 check(c->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Map test result");
 std::vector<uint16_t> result(size_t(frame.width)*frame.height*3/2);
 for(int row=0;row<frame.height*3/2;++row)memcpy(result.data()+size_t(row)*frame.width,static_cast<uint8_t*>(mapped.pData)+size_t(row)*mapped.RowPitch,frame.width*2);
 c->Unmap(staging.Get(),0);return result;
}
void test_case(MfD3d11Bridge& bridge,const std::filesystem::path& runtime,int w,int h,int padded,int arrays,bool queued,bool native){
 GpuFrucEngine engine(runtime,bridge.device(),bridge.context(),bridge.mutex(),queued?GpuCompletionMode::context_ordered:GpuCompletionMode::blocking,true,!native,!native,native?GpuInterpolationBackend::native_experimental:GpuInterpolationBackend::fruc);
 D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=padded;desc.ArraySize=arrays;desc.MipLevels=1;
 desc.Format=DXGI_FORMAT_P010;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_DECODER;
 ComPtr<ID3D11Texture2D> source;check(bridge.device()->CreateTexture2D(&desc,nullptr,&source),"Create P010 decoder array");
 std::vector<uint16_t> input(size_t(w)*padded*3/2,65535);
 std::vector<uint16_t> expected(size_t(w)*h*3/2);
 const auto code=[](int x,int row,int offset){return unsigned((x+row*29+offset)&1023);};
 const auto fill=[&](int offset){
  for(int row=0;row<h*3/2;++row)for(int x=0;x<w;++x){
   unsigned value=code(x,row,offset);size_t out=size_t(row)*w+x;
   size_t in=row<h?out:size_t(padded+row-h)*w+x;
   // Exercise ignored low-six storage bits, rounding boundaries and every
   // ten-bit code value. Padding and nonselected array slices must not leak.
   input[in]=uint16_t((value<<6)|unsigned((x+row)&63));
   expected[out]=uint16_t(value<<6);
  }
  bridge.context()->UpdateSubresource(source.Get(),arrays-1,nullptr,input.data(),w*2,0);
 };
 fill(0);auto firstExpected=expected;
 auto first=engine.copy({source,UINT(arrays-1),w,h,100});
 require(!engine.queued_completion(),"P010 must complete GPU work before sharing output");
 require(download(bridge.device(),bridge.context(),first)==firstExpected,"P010 precision/crop/UV mismatch");
 fill(173);auto second=engine.copy({source,UINT(arrays-1),w,h,417183});
 require(download(bridge.device(),bridge.context(),second)==expected,"P010 second snapshot mismatch");
 require(download(bridge.device(),bridge.context(),first)==firstExpected,"Decoder reuse changed retained original");
 require(first.pts==100&&second.pts==417183,"Normalization changed timestamps");
 D3D11_VIEWPORT viewport{11,13,123,145,0,1};bridge.context()->RSSetViewports(1,&viewport);
 auto third=engine.copy({source,UINT(arrays-1),w,h,834266});
 D3D11_VIEWPORT restored{};UINT count=1;bridge.context()->RSGetViewports(&count,&restored);
 require(count==1&&restored.TopLeftX==11&&restored.TopLeftY==13&&restored.Width==123&&restored.Height==145,"Conversion damaged shared renderer state");
 auto batch=engine.interpolate_pair(second,third,{625724});
 require(batch.quality.identical_warp_skipped,"Converted identical inputs changed content");
 auto bypass=engine.copy(second);require(download(bridge.device(),bridge.context(),bypass)==expected,"P010 recopy regression");
 engine.reset();auto afterSeek=engine.copy({source,UINT(arrays-1),w,h,0});
 require(download(bridge.device(),bridge.context(),afterSeek)==expected,"P010 conversion failed after seek reset");
 std::cout<<"PASS P010 "<<w<<'x'<<h<<" codedHeight="<<padded<<" array="<<arrays<<" queued="<<queued<<" all-1024-codes/crop/UV/decoder-reuse/state/native/seek=OK\n";
}
double field(double x,double y,unsigned seed) {
 x=(x+1024)/8;y=(y+1024)/8;const int gx=int(std::floor(x)),gy=int(std::floor(y));
 const double tx=x-gx,ty=y-gy;
 auto noise=[&](int dx,int dy) {unsigned v=unsigned(gx+dx)*733u+unsigned(gy+dy)*19349663u+seed;v^=v>>13;v*=1274126177u;return double(v%1024)/1023;};
 return (noise(0,0)*(1-tx)+noise(1,0)*tx)*(1-ty)+(noise(0,1)*(1-tx)+noise(1,1)*tx)*ty;
}
std::vector<uint16_t> moving10(int w,int h,double shift) {
 std::vector<uint16_t> p(size_t(w)*h*3/2);
 for(int y=0;y<h;++y)for(int x=0;x<w;++x)p[y*w+x]=uint16_t(std::lround(288+448*field(x-shift,y,13)))<<6;
 for(int y=0;y<h/2;++y)for(int x=0;x<w;x+=2)for(int c=0;c<2;++c)
  p[size_t(w)*h+y*w+x+c]=uint16_t(std::lround(416+192*field(x-shift,y*2,c?73:37)))<<6;
 return p;
}
double error10(const std::vector<uint16_t>& a,const std::vector<uint16_t>& b,int w,int h) {
 double error=0;size_t n=0;
 for(int y=48;y<h-48;++y)for(int x=48;x<w-48;++x){double d=int(a[y*w+x]>>6)-int(b[y*w+x]>>6);error+=d*d;++n;}
 return error/n;
}
void moving_case(MfD3d11Bridge& bridge,const std::filesystem::path& runtime,int w,int h) {
 GpuFrucEngine engine(runtime,bridge.device(),bridge.context(),bridge.mutex());
 auto upload=[&](double shift,int64_t pts) {
  auto pixels=moving10(w,h,shift);D3D11_TEXTURE2D_DESC desc{};
  desc.Width=w;desc.Height=h;desc.MipLevels=1;desc.ArraySize=1;desc.Format=DXGI_FORMAT_P010;desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA data{pixels.data(),UINT(w*2),0};ComPtr<ID3D11Texture2D> texture;
  check(bridge.device()->CreateTexture2D(&desc,&data,&texture),"moving P010 source");return engine.copy({texture,0,w,h,pts});
 };
 auto previous=upload(0,0),current=upload(20,400000);
 auto hdr=std::make_shared<HdrMetadata>();hdr->data[0].assign(80,0x42);previous.hdr=hdr;
 auto nextHdr=std::make_shared<HdrMetadata>();nextHdr->data[0].assign(80,0x24);current.hdr=nextHdr;
 auto batch=engine.interpolate_pair(previous,current,{80000,160000,240000,320000});
 require(!batch.quality.scene_cut && batch.frames.size()==4 && batch.quality.native_synthesized_mask==15,"Moving P010 phases were not synthesized");
 std::vector<std::vector<uint16_t>> retained;
 const auto left=moving10(w,h,0),right=moving10(w,h,20);
 for(int i=0;i<4;++i) {
  const auto phase=download(bridge.device(),bridge.context(),batch.frames[i]);auto truth=moving10(w,h,(i+1)*4);
  std::vector<uint16_t> blend(left.size());for(size_t k=0;k<blend.size();++k)blend[k]=uint16_t(std::lround(((left[k]>>6)*(4-i)+(right[k]>>6)*(i+1))/5.0))<<6;
  const double actual=error10(phase,truth,w,h),mixed=error10(blend,truth,w,h);
  require(actual<mixed*.4,"Moving P010 did not outperform ordinary blend");
  size_t lowBits=0;for(auto v:phase){require((v&63)==0,"P010 output has invalid low storage bits");lowBits+=(v&192)!=0;}
  require(lowBits>phase.size()/4,"Synthesized P010 was reduced to 8-bit precision");
  require(batch.frames[i].hdr==hdr,"Phase HDR metadata is not inherited from preceding frame");
  if(i)require(phase!=retained.back(),"Different motion phases have identical pixels");
  retained.push_back(phase);std::cout<<"P010 phase="<<(i+1)*.2<<" MSE="<<actual<<" blend="<<mixed<<" lowBits="<<lowBits<<'\n';
 }
 require(download(bridge.device(),bridge.context(),previous)==left && download(bridge.device(),bridge.context(),current)==right,"Synthesis modified original 10-bit pixels");
 require(engine.copy(current).hdr==nextHdr,"Copy lost HDR metadata");
 engine.reset();auto fresh=upload(0,0);require(!fresh.hdr,"Seek leaked old HDR metadata");
 for(int i=0;i<4;++i)require(download(bridge.device(),bridge.context(),batch.frames[i])==retained[i],"Retained phase changed after reset/reuse");
 std::cout<<"PASS P010 moving x5/originals/precision/HDR lifetime/seek\n";
}

}
int wmain(int argc,wchar_t** argv){
 if(argc<2||argc>3){std::cerr<<"Usage: gpu_p010_conversion <runtime-directory> [native]\n";return 2;}
 auto hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);if(FAILED(hr))return 3;
 int result=0;try{
  MfD3d11Bridge bridge;
  const bool native=argc==3&&std::wstring(argv[2])==L"native";
  moving_case(bridge,argv[1],640,360);
  moving_case(bridge,argv[1],3840,2160);
  for(bool queued:{false,true}){
   test_case(bridge,argv[1],1024,64,80,22,queued,native);
   test_case(bridge,argv[1],1920,1080,1152,22,queued,native);
  }
 }catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';result=1;}
 CoUninitialize();return result;
}
