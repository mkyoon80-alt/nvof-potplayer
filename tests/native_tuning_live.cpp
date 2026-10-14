// Interactive diagnostic host, driven by test-tuning.py. No installed filter/player.
#include "nvof/motion_synthesizer.hpp"
#include <wrl/client.h>
#include <dxgi1_2.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <cstdint>
#include <vector>
using Microsoft::WRL::ComPtr;
void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("D3D11 failure "+std::to_string(hr));}
int wmain(int argc,wchar_t** argv){try{
    constexpr unsigned w=640,h=360;
    ComPtr<IDXGIFactory1> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    for(unsigned i=0;;++i){ComPtr<IDXGIAdapter1> item;if(factory->EnumAdapters1(i,&item)==DXGI_ERROR_NOT_FOUND)break;
        DXGI_ADAPTER_DESC1 desc{};item->GetDesc1(&desc);if(desc.VendorId==0x10de){adapter=item;break;}}
    if(!adapter)throw std::runtime_error("NVIDIA adapter required");
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
    check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        levels,2,D3D11_SDK_VERSION,&device,nullptr,&context));
    nvof::MotionSynthesizer synth(device.Get(),context.Get());
    ComPtr<ID3D11Texture2D> inputs[2],output,staging;
    D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.MipLevels=desc.ArraySize=1;
    desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_NV12;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    for(unsigned frame=0;frame<2;++frame){
        std::vector<unsigned char> pixels(w*h*3/2,128);
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
            double qx=double(x)-frame*37.0,qy=y;
            double value=100+25*std::sin(x*.23)*std::cos(y*.19);
            if(qx>175&&qx<350&&qy>70&&qy<290)value=160+55*std::sin(qx*.51)*std::cos(qy*.43);
            pixels[y*w+x]=static_cast<unsigned char>(std::lround(value));
        }
        D3D11_SUBRESOURCE_DATA data{pixels.data(),w,static_cast<UINT>(pixels.size())};
        check(device->CreateTexture2D(&desc,&data,&inputs[frame]));
    }
    desc.BindFlags|=D3D11_BIND_RENDER_TARGET;check(device->CreateTexture2D(&desc,nullptr,&output));
    desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    check(device->CreateTexture2D(&desc,nullptr,&staging));
    ComPtr<ID3D11RenderTargetView> targets[2];
    for(unsigned plane=0;plane<2;++plane){D3D11_RENDER_TARGET_VIEW_DESC view{};
        view.Format=plane?DXGI_FORMAT_R8G8_UNORM:DXGI_FORMAT_R8_UNORM;view.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
        check(device->CreateRenderTargetView(output.Get(),&view,&targets[plane]));}
    std::filesystem::path directory=argc>1?argv[1]:L"";unsigned image=0;
    if(!directory.empty())std::filesystem::create_directories(directory);
    std::cout<<"READY "<<GetCurrentProcessId()<<std::endl;
    for(std::string command;std::cin>>command;){
        if(command=="quit")break;
        if(command=="prepare"){
            synth.prepare(inputs[0].Get(),inputs[1].Get(),w,h);
            std::cout<<"PREPARED "<<synth.reliable()<<std::endl;
        }else if(command=="reset"){
            synth.invalidate_history();std::cout<<"RESET"<<std::endl;
        }else if(command=="phase"){
            float phase;std::cin>>phase;synth.render_phase(targets[0].Get(),targets[1].Get(),phase);
            context->CopyResource(staging.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
            check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
            std::vector<unsigned char> pixels(w*h*3/2);
            uint64_t hash=14695981039346656037ull;
            for(unsigned y=0;y<h*3/2;++y)for(unsigned x=0;x<w;++x){
                auto value=static_cast<unsigned char*>(mapped.pData)[y*mapped.RowPitch+x];
                pixels[y*w+x]=value;hash=(hash^value)*1099511628211ull;}
            context->Unmap(staging.Get(),0);
            if(!directory.empty()){std::ofstream file(directory/(std::to_string(image++)+".nv12"),std::ios::binary);
                file.write(reinterpret_cast<const char*>(pixels.data()),pixels.size());if(!file)throw std::runtime_error("Cannot save pixels");}
            std::cout<<"PHASE "<<hash<<std::endl;
        }else throw std::runtime_error("Unknown host command");
    }
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<std::endl;return 1;}}
