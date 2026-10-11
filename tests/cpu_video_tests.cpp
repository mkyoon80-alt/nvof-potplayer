#include "nvof/cpu_video.hpp"
#include "nvof/avc_depth.hpp"
#include "avc_samples.hpp"
#include <iostream>
void require(bool v){if(!v)throw std::runtime_error("CPU video test failed");}
int main() {
    using namespace nvof;
    for(auto f:{CpuVideoFormat::p010,CpuVideoFormat::p016,CpuVideoFormat::p210,CpuVideoFormat::y210}) {
        CpuVideoLayout l{1024,4,1024*(f==CpuVideoFormat::y210?4:2)+32,f};
        std::vector<uint8_t> bytes(cpu_video_bytes(l),0xcc);
        auto put=[&](size_t i,int v){uint16_t w=uint16_t(v<<6);memcpy(bytes.data()+i,&w,2);};
        for(int y=0;y<4;++y)for(int x=0;x<1024;x+=2) {
            if(f==CpuVideoFormat::y210) {
                size_t i=size_t(y)*l.pitch+x*4;put(i,x);put(i+2,101);put(i+4,x+1);put(i+6,901);
            } else {
                put(size_t(y)*l.pitch+x*2,x);put(size_t(y)*l.pitch+x*2+2,x+1);
            }
        }
        if(f!=CpuVideoFormat::y210)for(int y=0;y<(cpu_422(f)?4:2);++y)for(int x=0;x<1024;x+=2) {
            size_t i=size_t(l.pitch)*4+size_t(y)*l.pitch+x*2;put(i,101);put(i+2,901);
        }
        auto p=unpack_cpu_video(bytes.data(),bytes.size(),l);auto out=pack_cpu_p010(p);
        for(int y=0;y<4;++y)for(int x=0;x<1024;++x)require(out[y*1024+x]==x<<6);
        for(size_t i=4096;i<out.size();i+=2){require(out[i]==101<<6);require(out[i+1]==901<<6);}
        bool rejected=false;try{unpack_cpu_video(bytes.data(),bytes.size()-1,l);}catch(...){rejected=true;}require(rejected);
    }
    // Opposite-time fields must not mix luma OR chroma. Test both field orders.
    CpuVideoPlanes p{8,8,8,std::vector<uint16_t>(64),std::vector<uint16_t>(32),std::vector<uint16_t>(32)};
    for(int y=0;y<8;++y){for(int x=0;x<8;++x)p.y[y*8+x]=(y&1)?701:101;for(int x=0;x<4;++x){p.u[y*4+x]=(y&1)?801:201;p.v[y*4+x]=(y&1)?901:301;}}
    for(int parity:{0,1}) {
        auto out=pack_cpu_p010(p,parity);
        for(int i=0;i<64;++i)require(out[i]==((parity?701:101)<<6));
        for(int i=64;i<96;i+=2){require(out[i]==((parity?801:201)<<6));require(out[i+1]==((parity?901:301)<<6));}
    }
    auto stable=pack_cpu_p010(p,0,&p);require(stable[8]==701<<6);
    // High 10 SPS prefix: chroma420, bit depths 10/10; AVC1 and Annex B.
    const uint8_t annex[]={0,0,0,1,0x67,110,0,31,0xa6,0xc0};
    require(avc_depth(annex,sizeof annex)==10);
    const uint8_t avcc[]={1,110,0,31,255,225,0,6,0x67,110,0,31,0xa6,0xc0,0};
    require(avc_depth(avcc,sizeof avcc)==10);
    require(!avc_format(avcc,sizeof avcc).frame_mbs_only.has_value());
    for(bool progressive:{true,false}) {
        const auto* data=progressive?avc_progressive10:avc_interlaced10;
        const size_t size=progressive?sizeof avc_progressive10:sizeof avc_interlaced10;
        const size_t sps_size=(size_t(data[6])<<8)|data[7];
        std::vector<uint8_t> annexb={0,0,0,1},lengths={data[6],data[7]};
        annexb.insert(annexb.end(),data+8,data+8+sps_size);lengths.insert(lengths.end(),data+8,data+8+sps_size);
        for(auto header:{std::vector<uint8_t>(data,data+size),annexb,lengths}) {
            auto f=avc_format(header.data(),header.size());
            require(f.depth==10 && f.chroma==(progressive?1:2) && f.frame_mbs_only==progressive && f.mbaff==!progressive);
        }
        for(size_t n=0;n<8+sps_size;++n)require(!avc_format(data,n).frame_mbs_only.has_value());
    }
    for(size_t n=0;n<10;++n)require(avc_depth(annex,n)==0);
    std::cout<<"PASS all 1024 levels, padded P010/P016/P210/Y210, chroma field separation, AVC progressive/MBAFF and truncation\n";
}
