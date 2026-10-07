#include "nvof/engine.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
constexpr int W = 640, H = 360;
uint32_t mix(uint32_t v) { v ^= v >> 16; v *= 0x7feb352d; v ^= v >> 15; v *= 0x846ca68b; return v ^ (v >> 16); }
float texel(int x, int y) { return 32.f + float(mix(uint32_t(x) * 733U + uint32_t(y) * 19349663U) % 190U); }
uint8_t luminance(int x, int y) {
    // Coherent smooth textured scene, shifted identically at every time step.
    x += 1024; y += 1024;
    const int gx = x / 8, gy = y / 8;
    const float a = float(x % 8) / 8.f, b = float(y % 8) / 8.f;
    return uint8_t((1-a)*(1-b)*texel(gx,gy) + a*(1-b)*texel(gx+1,gy) + (1-a)*b*texel(gx,gy+1) + a*b*texel(gx+1,gy+1));
}
nvof::Frame scene(int shift, int64_t pts) {
    nvof::Frame f{W,H,pts,std::vector<uint8_t>(size_t(W)*H*3/2,128)};
    for(int y=0;y<H;++y) for(int x=0;x<W;++x) f.pixels[size_t(y)*W+x]=luminance(x-shift,y);
    return f;
}
double mse(const nvof::Frame& a, const nvof::Frame& b) {
    double error=0; size_t count=0;
    for(int y=32;y<H-32;++y) for(int x=64;x<W-64;++x) {
        const double d=double(a.pixels[size_t(y)*W+x])-b.pixels[size_t(y)*W+x]; error+=d*d; ++count;
    }
    return error / double(count);
}
void require(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
void save_pgm(const std::filesystem::path& path,const nvof::Frame& f) {
    std::ofstream output(path,std::ios::binary); output<<"P5\n"<<f.width<<' '<<f.height<<"\n255\n";
    output.write(reinterpret_cast<const char*>(f.pixels.data()),size_t(f.width)*f.height);
}
}
int wmain(int argc,wchar_t** argv) {
    try {
        if(argc<2) { std::cerr<<"Usage: engine_smoke <absolute runtime directory> [artifact directory]\n"; return 2; }
        nvof::FrucEngine engine{std::filesystem::path(argv[1])};
        std::cout<<"DEVICE "<<engine.device_name()<<'\n';
        auto previous=scene(0,0); nvof::Frame result;
        const auto start=std::chrono::steady_clock::now();
        for(int i=1;i<=12;++i) {
            auto current=scene(i*8,int64_t(i)*400000);
            result=engine.midpoint(previous,current);
            require(result.pts==current.pts-200000,"Midpoint timestamp is wrong");
            previous=std::move(current);
        }
        auto left=scene(88,4400000), right=scene(96,4800000), truth=scene(92,4600000), blend=left;
        for(size_t i=0;i<blend.pixels.size();++i) blend.pixels[i]=uint8_t((int(left.pixels[i])+right.pixels[i])/2);
        const double actual_error=mse(result,truth), blend_error=mse(blend,truth);
        std::cout<<"MOTION_MSE "<<actual_error<<" BLEND_MSE "<<blend_error<<" LEFT_DIFF "<<mse(result,left)<<" RIGHT_DIFF "<<mse(result,right)<<'\n';
        require(mse(result,left)>1.0 && mse(result,right)>1.0,"FRUC output repeated an input frame");
        require(actual_error < blend_error * 0.80,"Output did not outperform ordinary frame blending on known translation");
        if(argc>2) { const std::filesystem::path out(argv[2]); std::filesystem::create_directories(out); save_pgm(out/L"midpoint.pgm",result); save_pgm(out/L"expected.pgm",truth); save_pgm(out/L"blend.pgm",blend); }
        // Explicit reset, reverse seek to an earlier timestamp, and an implicit gap reset.
        for(int i=0;i<8;++i) {
            engine.reset();
            const auto a=scene(i*4,int64_t(i)*10000000), b=scene(i*4+8,int64_t(i)*10000000+400000);
            auto frame=engine.midpoint(a,b); require(frame.pixels.size()==a.pixels.size(),"Reset lost frame data");
            auto seek_a=scene(0,0), seek_b=scene(8,400000);
            frame=engine.midpoint(seek_a,seek_b); require(frame.pts==200000,"Backward seek reset failed");
        }
        bool rejected=false;
        try { engine.midpoint(scene(0,0),scene(8,0)); } catch(const std::invalid_argument&) { rejected=true; }
        require(rejected,"Non-increasing timestamps were accepted");
        engine.reset(); engine.reset();
        for(int i=0;i<3;++i) { nvof::FrucEngine reopened{std::filesystem::path(argv[1])}; reopened.midpoint(scene(0,0),scene(8,400000)); }
        const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
        std::cout<<"PASS true midpoint, sequential frames, 8 explicit resets, 8 backward seeks, double reset, 3 reopen/shutdown cycles; "<<ms<<" ms\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<"FAIL "<<error.what()<<'\n'; return 1; }
}
