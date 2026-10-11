#pragma once
#include "nvof/hevc_color.hpp"
namespace nvof {
struct AvcFormat {
    unsigned depth=0,chroma=1;
    std::optional<bool> frame_mbs_only;
    bool mbaff=false;
};
// Read codec facts from SPS. In particular, a decoder may advertise an
// interlace-capable raw output even when every coded picture is progressive.
inline AvcFormat avc_format(const uint8_t* data,size_t size) noexcept {
    if(!data || size<4 || size>65536)return {};
    try {
        AvcFormat result;
        auto accept=[&](const uint8_t* p,size_t n) {
            if(n<4 || (p[0]&0x80) || (p[0]&31)!=7)return;
            std::vector<uint8_t> rbsp;unsigned zeros=0;
            for(size_t i=1;i<n;++i) {
                if(zeros==2 && p[i]==3){if(i+1>=n || p[i+1]>3)throw hevc_detail::Invalid{};zeros=0;continue;}
                rbsp.push_back(p[i]);zeros=p[i]==0?zeros+1:0;
            }
            hevc_detail::Bits b{rbsp};unsigned profile=b.get(8);b.skip(16);b.ue(31);
            AvcFormat value;value.depth=8;
            if(profile==100 || profile==110 || profile==122 || profile==244 || profile==44 || profile==83 || profile==86 ||
               profile==118 || profile==128 || profile==138 || profile==139 || profile==134 || profile==135) {
                value.chroma=b.ue(3);if(value.chroma==3)b.get(1);
                value.depth=8+b.ue(6);unsigned chroma=8+b.ue(6);value.depth=(std::max)(value.depth,chroma);
            }
            // Keep the depth prefix usable if an older decoder exposes only
            // that prefix; truncated field syntax never proves progressive.
            try {
                auto se=[&](unsigned limit=65535) {unsigned v=b.ue(limit);return (v&1)?int((v+1)/2):-int(v/2);};
                if(profile==100 || profile==110 || profile==122 || profile==244 || profile==44 || profile==83 || profile==86 ||
                   profile==118 || profile==128 || profile==138 || profile==139 || profile==134 || profile==135) {
                    b.get(1); // qpprime_y_zero_transform_bypass_flag
                    if(b.get(1))for(unsigned i=0;i<(value.chroma==3?12u:8u);++i)if(b.get(1)) {
                        int last=8,next=8;
                        for(unsigned j=0;j<(i<6?16u:64u);++j) {
                            if(next)next=(last+se(255)+256)%256;
                            if(next)last=next;
                        }
                    }
                }
                b.ue(12);const unsigned poc=b.ue(2);
                if(poc==0)b.ue(12);
                else if(poc==1) {b.get(1);se();se();const unsigned count=b.ue(255);for(unsigned i=0;i<count;++i)se();}
                b.ue(32);b.get(1);b.ue(65535);b.ue(65535);
                const bool frames=b.get(1)!=0;
                const bool mbaff=!frames && b.get(1)!=0;
                b.get(1); // direct_8x8_inference_flag
                value.frame_mbs_only=frames;value.mbaff=mbaff;
            }catch(const hevc_detail::Invalid&){}
            if(result.depth) {
                if(result.depth!=value.depth || result.chroma!=value.chroma)throw hevc_detail::Invalid{};
                if(result.frame_mbs_only!=value.frame_mbs_only || result.mbaff!=value.mbaff) {
                    result.frame_mbs_only.reset();result.mbaff=false;
                }
            }else result=value;
        };
        if(data[0]==1) { // AVCDecoderConfigurationRecord
            if(size<7)return {};size_t p=6;unsigned count=data[5]&31;
            for(unsigned i=0;i<count;++i) {
                if(p+2>size)throw hevc_detail::Invalid{};
                size_t n=(size_t(data[p])<<8)|data[p+1];p+=2;
                if(n<4 || n>size-p)throw hevc_detail::Invalid{};
                accept(data+p,n);p+=n;
            }
        } else if(data[0]==0 && data[1]==0 && (data[2]==1 || (size>3 && data[2]==0 && data[3]==1))) {
            auto start=[&](size_t p){for(;p+3<=size;++p)if(!data[p] && !data[p+1] && data[p+2]==1)return p;return size;};
            for(size_t p=start(0);p<size;) {size_t begin=p+3,next=start(begin);accept(data+begin,next-begin);p=next;}
        } else { // DirectShow MPEG2VIDEOINFO: 16-bit length-prefixed SPS/PPS
            size_t p=0;
            while(p<size) {
                if(p+2>size)throw hevc_detail::Invalid{};
                size_t n=(size_t(data[p])<<8)|data[p+1];p+=2;
                if(!n || n>size-p)throw hevc_detail::Invalid{};
                accept(data+p,n);p+=n;
            }
        }
        return result;
    }catch(...){return {};}
}
inline unsigned avc_depth(const uint8_t* data,size_t size) noexcept {return avc_format(data,size).depth;}
}
