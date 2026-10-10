#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace nvof {
struct HevcColor { unsigned primaries=0, transfer=0, matrix=0; bool full_range=false; };
namespace hevc_detail {
// Bounded syntax reader, independent implementation of H.265 SPS/VUI and hvcC.
// Only sequence-header color signalling is read; no picture data is decoded.
// Reference: ITU-T H.265, 7.3.2.2.1, 7.3.3, 7.3.4, 7.3.7 and Annex E.
struct Invalid {};
struct Bits {
    const std::vector<uint8_t>& bytes; size_t pos=0;
    unsigned get(unsigned n) {
        if(n>32 || pos>bytes.size()*8 || n>bytes.size()*8-pos)throw Invalid{};
        unsigned v=0;while(n--) {v=(v<<1)|((bytes[pos/8]>>(7-pos%8))&1);++pos;}return v;
    }
    void skip(unsigned n) {while(n>32){get(32);n-=32;}get(n);}
    unsigned ue(unsigned limit=65535) {
        unsigned zeros=0;while(!get(1)){if(++zeros>16)throw Invalid{};}
        const unsigned v=((1u<<zeros)-1)+get(zeros);if(v>limit)throw Invalid{};return v;
    }
};
inline std::optional<HevcColor> sps(const uint8_t* data,size_t size) {
    if(!data || size<4 || size>65536 || (data[0]&0x80) || ((data[0]>>1)&63)!=33 || (data[0]&1) || (data[1]>>3) || !(data[1]&7))return {};
    std::vector<uint8_t> rbsp;rbsp.reserve(size-2);
    unsigned zeros=0;
    for(size_t i=2;i<size;++i) {
        if(zeros==2 && data[i]==3) {
            if(i+1>=size || data[i+1]>3)throw Invalid{};
            zeros=0;continue;
        }
        rbsp.push_back(data[i]);zeros=data[i]==0?zeros+1:0;
    }
    Bits b{rbsp};b.get(4);const unsigned sub=b.get(3);if(sub>6)throw Invalid{};b.get(1);
    b.skip(96);unsigned profile[7]{},level[7]{};
    for(unsigned i=0;i<sub;++i){profile[i]=b.get(1);level[i]=b.get(1);}
    if(sub)for(unsigned i=sub;i<8;++i)b.get(2);
    for(unsigned i=0;i<sub;++i){if(profile[i])b.skip(88);if(level[i])b.skip(8);}
    b.ue(15);const unsigned chroma=b.ue(3);if(chroma==3)b.get(1);
    if(!b.ue(32768) || !b.ue(32768))throw Invalid{};
    if(b.get(1))for(int i=0;i<4;++i)b.ue(32768);
    b.ue(8);b.ue(8);const unsigned poc=b.ue(12)+4;
    const unsigned ordering=b.get(1);
    for(unsigned i=ordering?0:sub;i<=sub;++i){b.ue(16);b.ue(16);b.ue();}
    for(int i=0;i<6;++i)b.ue(16);
    if(b.get(1) && b.get(1)) {
        for(unsigned sizeId=0;sizeId<4;++sizeId)for(unsigned matrixId=0;matrixId<6;matrixId+=sizeId==3?3:1) {
            if(!b.get(1))b.ue(6);
            else {if(sizeId>1)b.ue(510);const unsigned count=(std::min)(64u,1u<<(4+2*sizeId));for(unsigned i=0;i<count;++i)b.ue(510);}
        }
    }
    b.get(1);b.get(1);
    if(b.get(1)){b.skip(8);b.ue(16);b.ue(16);b.get(1);}
    const unsigned sets=b.ue(64);unsigned previous=0;
    for(unsigned i=0;i<sets;++i) {
        unsigned count=0;
        if(i && b.get(1)) {
            b.get(1);b.ue(32767);
            for(unsigned j=0;j<=previous;++j){const bool used=b.get(1)!=0;const bool delta=used || b.get(1)!=0;if(delta)++count;}
        } else {
            const unsigned neg=b.ue(16),pos=b.ue(16);count=neg+pos;
            if(count>16)throw Invalid{};
            for(unsigned j=0;j<count;++j){b.ue(32767);b.get(1);}
        }
        if(count>16)throw Invalid{};previous=count;
    }
    if(b.get(1)){const unsigned count=b.ue(32);for(unsigned i=0;i<count;++i){b.get(poc);b.get(1);}}
    b.get(1);b.get(1);
    if(!b.get(1))return {}; // vui_parameters_present_flag
    if(b.get(1)){if(b.get(8)==255)b.skip(32);}
    if(b.get(1))b.get(1);
    if(!b.get(1))return {}; // video_signal_type_present_flag
    b.get(3);HevcColor color;color.full_range=b.get(1)!=0;
    if(!b.get(1))return {}; // colour_description_present_flag
    color.primaries=b.get(8);color.transfer=b.get(8);color.matrix=b.get(8);return color;
}
}
inline std::optional<HevcColor> hevc_color(const uint8_t* data,size_t size) noexcept {
    if(!data || size<4 || size>65536)return {};
    try {
        std::optional<HevcColor> result;
        auto accept=[&](const uint8_t* nal,size_t n) {
            auto next=hevc_detail::sps(nal,n);if(!next)return;
            if(result && (result->primaries!=next->primaries || result->transfer!=next->transfer || result->matrix!=next->matrix || result->full_range!=next->full_range))throw hevc_detail::Invalid{};
            result=next;
        };
        if(data[0]==1) { // HEVCDecoderConfigurationRecord (hvcC)
            if(size<23)return {};
            size_t p=23;
            for(unsigned a=0;a<data[22];++a) {
                if(p+3>size)throw hevc_detail::Invalid{};
                const unsigned type=data[p++]&63;
                const unsigned count=(unsigned(data[p])<<8)|data[p+1];p+=2;
                if(count>256)throw hevc_detail::Invalid{};
                for(unsigned j=0;j<count;++j) {
                    if(p+2>size)throw hevc_detail::Invalid{};
                    const size_t n=(size_t(data[p])<<8)|data[p+1];p+=2;
                    if(n<2 || n>size-p)throw hevc_detail::Invalid{};
                    if(((data[p]>>1)&63)!=type)throw hevc_detail::Invalid{};
                    if(type==33)accept(data+p,n);p+=n;
                }
            }
            if(p!=size)throw hevc_detail::Invalid{};
        } else { // Annex B sequence header
            auto start=[&](size_t p)->size_t {
                for(;p+3<=size;++p)if(data[p]==0 && data[p+1]==0 && data[p+2]==1)return p;
                return size;
            };
            size_t p=start(0);if(p>1)return {};
            while(p<size) {
                const size_t begin=p+3;const size_t next=start(begin);size_t end=next;
                while(end>begin && data[end-1]==0)--end;
                accept(data+begin,end-begin);p=next;
            }
        }
        return result;
    }catch(...){return {};}
}
}
