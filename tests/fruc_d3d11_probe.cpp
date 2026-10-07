#define wmain old_gpu_test_entry
#include "gpu_engine_smoke.cpp"
#undef wmain
#include <d3d11_4.h>
#include <NvOFFRUC.h>
#include <cuda.h>

int wmain(int argc,wchar_t** argv){try{
 require(argc>=2,"fruc_d3d11_probe <runtime> [width height]");
 const int w=argc>2?_wtoi(argv[2]):640,h=argc>3?_wtoi(argv[3]):360;
 ComPtr<IDXGIFactory1> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI");ComPtr<IDXGIAdapter1> adapter;
 for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> c;if(factory->EnumAdapters1(i,&c)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 d{};c->GetDesc1(&d);if(d.VendorId==0x10de){adapter=c;break;}}
 require(bool(adapter),"NVIDIA missing");ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
 check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_VIDEO_SUPPORT|D3D11_CREATE_DEVICE_BGRA_SUPPORT,levels,2,D3D11_SDK_VERSION,&device,nullptr,&context),"D3D create");
 ComPtr<ID3D11Device5> dev5;ComPtr<ID3D11DeviceContext4> ctx4;check(device.As(&dev5),"device5");check(context.As(&ctx4),"context4");ComPtr<ID3D11Fence> fence;check(dev5->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&fence)),"fence");
 std::array<ComPtr<ID3D11Texture2D>,3> textures;
 for(auto& t:textures){D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=1;d.Format=DXGI_FORMAT_NV12;d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;d.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;check(device->CreateTexture2D(&d,nullptr,&t),"shared NV12 texture");}
 auto module=LoadLibraryExW((std::filesystem::absolute(argv[1])/L"NvOFFRUC.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);require(module!=nullptr,"NvOFFRUC load failed");
 auto create=reinterpret_cast<PtrToFuncNvOFFRUCCreate>(GetProcAddress(module,CreateProcName));auto reg=reinterpret_cast<PtrToFuncNvOFFRUCRegisterResource>(GetProcAddress(module,RegisterResourceProcName));auto process=reinterpret_cast<PtrToFuncNvOFFRUCProcess>(GetProcAddress(module,ProcessProcName));auto unreg=reinterpret_cast<PtrToFuncNvOFFRUCUnregisterResource>(GetProcAddress(module,UnregisterResourceProcName));auto destroy=reinterpret_cast<PtrToFuncNvOFFRUCDestroy>(GetProcAddress(module,DestroyProcName));
 require(create&&reg&&process&&unreg&&destroy,"Missing SDK exports");
 for(int generation=0;generation<3;++generation){
 CUcontext before=nullptr;cuCtxGetCurrent(&before);std::cout<<"GENERATION="<<generation<<" context="<<before<<std::endl;cuCtxSetCurrent(nullptr);
 if(generation){
  ComPtr<ID3D11Device> freshDevice;ComPtr<ID3D11DeviceContext> freshContext;
  check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_VIDEO_SUPPORT|D3D11_CREATE_DEVICE_BGRA_SUPPORT,levels,2,D3D11_SDK_VERSION,&freshDevice,nullptr,&freshContext),"fresh device");
  device=freshDevice;context=freshContext;check(device.As(&dev5),"fresh device5");check(context.As(&ctx4),"fresh context4");
 }
 ComPtr<ID3D11Fence> freshFence;check(dev5->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&freshFence)),"fresh fence");fence.Swap(freshFence);
 std::array<ComPtr<ID3D11Texture2D>,3> freshTextures;
 for(int j=0;j<3;++j){D3D11_TEXTURE2D_DESC d{};textures[j]->GetDesc(&d);check(device->CreateTexture2D(&d,nullptr,&freshTextures[j]),"fresh texture");}textures.swap(freshTextures);
 NvOFFRUC_CREATE_PARAM cp{};cp.uiWidth=w;cp.uiHeight=h;cp.pDevice=device.Get();cp.eResourceType=DirectX11Resource;cp.eSurfaceFormat=NV12Surface;cp.eCUDAResourceType=CudaResourceTypeUndefined;NvOFFRUCHandle handle=nullptr;
 auto rc=create(&cp,&handle);std::cout<<"DIRECT_D3D_CREATE="<<int(rc)<<std::endl;require(rc==NvOFFRUC_SUCCESS,"DirectX NV12 create failed");
 NvOFFRUC_REGISTER_RESOURCE_PARAM rp{};rp.uiCount=3;rp.pD3D11FenceObj=fence.Get();for(int i=0;i<3;++i)rp.pArrResource[i]=textures[i].Get();rc=reg(handle,&rp);std::cout<<"DIRECT_D3D_REGISTER="<<int(rc)<<std::endl;require(rc==NvOFFRUC_SUCCESS,"DirectX NV12 registration failed");
 HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);require(event!=nullptr,"event failed");uint64_t value=0;
 for(int i=0;i<12;++i){auto data=pixels(w,h,i*4);context->UpdateSubresource(textures[i%2].Get(),0,nullptr,data.data(),w,0);check(ctx4->Signal(fence.Get(),++value),"signal input");context->Flush();
  NvOFFRUC_PROCESS_IN_PARAMS in{};NvOFFRUC_PROCESS_OUT_PARAMS out{};bool repeated=false;in.stFrameDataInput.pFrame=textures[i%2].Get();in.stFrameDataInput.nTimeStamp=i*1000;in.uSyncWait.FenceWaitValue.uiFenceValueToWaitOn=value;out.stFrameDataOutput.pFrame=textures[2].Get();out.stFrameDataOutput.nTimeStamp=i?i*1000-500:0;out.stFrameDataOutput.bHasFrameRepetitionOccurred=&repeated;out.uSyncSignal.FenceSignalValue.uiFenceValueToSignalOn=++value;
  auto begin=std::chrono::steady_clock::now();rc=process(handle,&in,&out);double submit=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();require(rc==NvOFFRUC_SUCCESS,"DirectX NV12 process failed");
  check(fence->SetEventOnCompletion(value,event),"completion event");require(WaitForSingleObject(event,10000)==WAIT_OBJECT_0,"FRUC output fence did not signal");check(ctx4->Wait(fence.Get(),value),"GPU wait");
  auto image=readback(device.Get(),context.Get(),{textures[2],0,w,h,int64_t(i)*1000});auto truth=pixels(w,h,i?i*4-2:0);const auto error=mse(image,truth,w,h);std::cout<<"DIRECT_D3D_FRAME="<<i<<" submit_ms="<<submit<<" repeated="<<repeated<<" mse="<<error<<std::endl;require(i==0||repeated||error<10,"Direct NV12 midpoint corrupted");
 }
 NvOFFRUC_UNREGISTER_RESOURCE_PARAM up{};up.uiCount=3;for(int i=0;i<3;++i)up.pArrResource[i]=textures[i].Get();require(unreg(handle,&up)==NvOFFRUC_SUCCESS,"unregister failed");require(destroy(handle)==NvOFFRUC_SUCCESS,"destroy failed");CloseHandle(event);CUcontext after=nullptr;cuCtxGetCurrent(&after);std::cout<<"CLOSED context="<<after<<std::endl;}FreeLibrary(module);std::cout<<"RESEARCH ONLY: luma/fence probe completed; NOT color or repeated-seek acceptance\n";return 0;
}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
