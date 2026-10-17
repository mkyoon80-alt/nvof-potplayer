#pragma once
#include "hevc_color.hpp"

namespace nvof {
namespace hevc_scan_detail {
// Read only sequence-level scan evidence, never picture pixels. H.265
// 7.3.2.2.1/7.3.3/Annex E; the progressive constraints also appear in
// ETSI TS 126 265, 4.5.3. Unspecified, mixed, field-coded or conflicting
// headers must not override the decoder's interlace contract.
inline bool progressive_sps(const uint8_t* data,size_t size) {
    using hevc_detail::Invalid;
    if(!data || size<4 || size>65536 || (data[0]&0x80) || ((data[0]>>1)&63)!=33 ||
       (data[0]&1) || (data[1]>>3) || (data[1]&7)!=1)throw Invalid{};
    std::vector<uint8_t> rbsp;rbsp.reserve(size-2);
    unsigned zeros=0;
    for(size_t i=2;i<size;++i) {
        if(zeros==2 && data[i]==3) {
            if(i+1>=size || data[i+1]>3)throw Invalid{};
            zeros=0;continue;
        }
        rbsp.push_back(data[i]);zeros=data[i]==0?zeros+1:0;
    }
    hevc_detail::Bits b{rbsp};
    b.get(4);const unsigned sub=b.get(3);if(sub>6)throw Invalid{};b.get(1);
    b.skip(40);
    const bool progressive=b.get(1)!=0,interlaced=b.get(1)!=0;
    b.get(1);const bool frame_only=b.get(1)!=0;
    b.skip(52);unsigned profile[7]{},level[7]{};
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
        for(unsigned s=0;s<4;++s)for(unsigned m=0;m<6;m+=s==3?3:1) {
            if(!b.get(1))b.ue(6);
            else {if(s>1)b.ue(510);const unsigned count=(std::min)(64u,1u<<(4+2*s));for(unsigned j=0;j<count;++j)b.ue(510);}
        }
    }
    b.get(1);b.get(1);
    if(b.get(1)){b.skip(8);b.ue(16);b.ue(16);b.get(1);}
    const unsigned sets=b.ue(64);unsigned previous=0;
    for(unsigned i=0;i<sets;++i) {
        unsigned count=0;
        if(i && b.get(1)) {
            b.get(1);b.ue(32767);
            for(unsigned j=0;j<=previous;++j){const bool used=b.get(1)!=0;if(used || b.get(1))++count;}
        } else {
            const unsigned neg=b.ue(16),pos=b.ue(16);count=neg+pos;if(count>16)throw Invalid{};
            for(unsigned j=0;j<count;++j){b.ue(32767);b.get(1);}
        }
        if(count>16)throw Invalid{};previous=count;
    }
    if(b.get(1)){const unsigned count=b.ue(32);for(unsigned i=0;i<count;++i){b.get(poc);b.get(1);}}
    b.get(1);b.get(1);
    bool field_sequence=false;
    if(b.get(1)) { // vui_parameters_present_flag
        if(b.get(1) && b.get(8)==255)b.skip(32);
        if(b.get(1))b.get(1);
        if(b.get(1)){b.skip(4);if(b.get(1))b.skip(24);}
        if(b.get(1)){b.ue(5);b.ue(5);}
        b.get(1);field_sequence=b.get(1)!=0;
        b.get(1); // frame_field_info_present_flag (not a scan-type promise)
    }
    return progressive && !interlaced && frame_only && !field_sequence;
}
} // namespace hevc_scan_detail

inline bool hevc_progressive_only(const uint8_t* data,size_t size) noexcept {
    if(!data || size<4 || size>65536)return false;
    try {
        bool found=false,all=true;
        auto accept=[&](const uint8_t* nal,size_t n) {
            if(n<2)throw hevc_detail::Invalid{};
            if(((nal[0]>>1)&63)!=33)return;
            found=true;
            // Evaluate every SPS, including later malformed/conflicting ones.
            const bool next=hevc_scan_detail::progressive_sps(nal,n);all=all && next;
        };
        if(data[0]==1) {
            if(size<23)return false;
            size_t p=23;
            for(unsigned a=0;a<data[22];++a) {
                if(p+3>size)throw hevc_detail::Invalid{};
                const unsigned type=data[p++]&63,count=(unsigned(data[p])<<8)|data[p+1];p+=2;
                if(count>256)throw hevc_detail::Invalid{};
                for(unsigned j=0;j<count;++j) {
                    if(p+2>size)throw hevc_detail::Invalid{};
                    const size_t n=(size_t(data[p])<<8)|data[p+1];p+=2;
                    if(n<2 || n>size-p || ((data[p]>>1)&63)!=type)throw hevc_detail::Invalid{};
                    accept(data+p,n);p+=n;
                }
            }
            if(p!=size)throw hevc_detail::Invalid{};
        } else {
            auto start=[&](size_t p) {
                for(;p+3<=size;++p)if(!data[p] && !data[p+1] && data[p+2]==1)return p;
                return size;
            };
            size_t p=start(0);if(p>1)return false;
            while(p<size) {
                const size_t begin=p+3,next=start(begin);size_t end=next;
                while(end>begin && !data[end-1])--end;
                accept(data+begin,end-begin);p=next;
            }
        }
        return found && all;
    }catch(...){return false;}
}
} // namespace nvof
