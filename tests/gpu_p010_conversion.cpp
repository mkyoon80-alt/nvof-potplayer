#include "nvof/gpu_engine.hpp"
#include "nvof/mf_bridge.hpp"
#include <iostream>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <cstring>
#include <d3d10_1.h>
using Microsoft::WRL::ComPtr;
using namespace nvof;
namespace {
void check(HRESULT hr,const char* action){if(FAILED(hr))throw std::runtime_error(std::string(action)+" hr="+std::to_string(unsigned(hr)));}
void require(bool ok,const char* what){if(!ok)throw std::runtime_error(what);}
std::vector<uint8_t> download(ID3D11Device* d,ID3D11DeviceContext* c,const GpuFrame& frame){
 D3D11_TEXTURE2D_DESC desc{};frame.texture->GetDesc(&desc);
 require(desc.Format==DXGI_FORMAT_NV12&&desc.Width==UINT(frame.width)&&desc.Height==UINT(frame.height),"Output is not visible-size NV12");
 desc.BindFlags=0;desc.MiscFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
 ComPtr<ID3D11Texture2D> staging;check(d->CreateTexture2D(&desc,nullptr,&staging),"Create test readback");
 c->CopyResource(staging.Get(),frame.texture.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
 check(c->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Map test result");
 std::vector<uint8_t> result(size_t(frame.width)*frame.height*3/2);
 for(int row=0;row<frame.height*3/2;++row)memcpy(result.data()+size_t(row)*frame.width,static_cast<uint8_t*>(mapped.pData)+size_t(row)*mapped.RowPitch,frame.width);
 c->Unmap(staging.Get(),0);return result;
}
void test_case(MfD3d11Bridge& bridge,const std::filesystem::path& runtime,int w,int h,int padded,int arrays,bool queued,bool native){
 GpuFrucEngine engine(runtime,bridge.device(),bridge.context(),bridge.mutex(),queued?GpuCompletionMode::context_ordered:GpuCompletionMode::blocking,true,!native,!native,native?GpuInterpolationBackend::native_experimental:GpuInterpolationBackend::fruc);
 D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=padded;desc.ArraySize=arrays;desc.MipLevels=1;
 desc.Format=DXGI_FORMAT_P010;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_DECODER;
 ComPtr<ID3D11Texture2D> source;check(bridge.device()->CreateTexture2D(&desc,nullptr,&source),"Create P010 decoder array");
 std::vector<uint16_t> input(size_t(w)*padded*3/2,65535);
 std::vector<uint8_t> expected(size_t(w)*h*3/2);
 const auto code=[](int x,int row,int offset){return unsigned((x+row*29+offset)&1023);};
 const auto fill=[&](int offset){
  for(int row=0;row<h*3/2;++row)for(int x=0;x<w;++x){
   unsigned value=code(x,row,offset);size_t out=size_t(row)*w+x;
   size_t in=row<h?out:size_t(padded+row-h)*w+x;
   // Exercise ignored low-six storage bits, rounding boundaries and every
   // ten-bit code value. Padding and nonselected array slices must not leak.
   input[in]=uint16_t((value<<6)|unsigned((x+row)&63));
   expected[out]=uint8_t((std::min)(255u,(value+2)/4));
  }
  bridge.context()->UpdateSubresource(source.Get(),arrays-1,nullptr,input.data(),w*2,0);
 };
 fill(0);auto firstExpected=expected;
 auto first=engine.copy({source,UINT(arrays-1),w,h,100});
 require(!engine.queued_completion(),"P010 must complete GPU work before sharing output");
 require(download(bridge.device(),bridge.context(),first)==firstExpected,"P010 quantization/crop/UV mismatch");
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
 auto bypass=engine.copy(second);require(download(bridge.device(),bridge.context(),bypass)==expected,"NV12 copy regression after P010");
 engine.reset();auto afterSeek=engine.copy({source,UINT(arrays-1),w,h,0});
 require(download(bridge.device(),bridge.context(),afterSeek)==expected,"P010 conversion failed after seek reset");
 std::cout<<"PASS P010 "<<w<<'x'<<h<<" codedHeight="<<padded<<" array="<<arrays<<" queued="<<queued<<" all-1024-codes/crop/UV/decoder-reuse/state/FRUC/seek=OK\n";
}
}
int wmain(int argc,wchar_t** argv){
 if(argc<2||argc>3){std::cerr<<"Usage: gpu_p010_conversion <runtime-directory> [native]\n";return 2;}
 auto hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);if(FAILED(hr))return 3;
 int result=0;try{
  MfD3d11Bridge bridge;
  const bool native=argc==3&&std::wstring(argv[2])==L"native";
  for(bool queued:{false,true}){
   test_case(bridge,argv[1],1024,64,80,22,queued,native);
   test_case(bridge,argv[1],1920,1080,1152,22,queued,native);
  }
 }catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';result=1;}
 CoUninitialize();return result;
}
