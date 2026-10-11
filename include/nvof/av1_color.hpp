#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>

namespace nvof {
struct Av1Color { unsigned primaries=2, transfer=2, matrix=2; bool full_range=false; };
namespace av1_detail {
// Independent, bounded AV1 sequence-header reader. No picture decoding.
// AOM AV1 specification 5.5 and AV1-ISOBMFF 2.3 (av1C).
struct Invalid {};
struct Bits {
    const uint8_t* data; size_t size, pos=0;
    unsigned get(unsigned n) {
        if(n>32 || pos>size*8 || n>size*8-pos)throw Invalid{};
        unsigned v=0;while(n--){v=(v<<1)|((data[pos/8]>>(7-pos%8))&1);++pos;}return v;
    }
    void skip(unsigned n){while(n>32){get(32);n-=32;}get(n);}
    void uvlc(){unsigned n=0;while(!get(1)){if(++n==32)return;}get(n);}
};
inline Av1Color sequence(const uint8_t* data,size_t size) {
    Bits b{data,size};const unsigned profile=b.get(3);if(profile>2)throw Invalid{};
    const bool still=b.get(1)!=0,reduced=b.get(1)!=0;if(reduced && !still)throw Invalid{};
    if(reduced)b.get(5);
    else {
        bool model=false;unsigned delayBits=0;
        if(b.get(1)) {
            b.skip(64);if(b.get(1))b.uvlc();
            model=b.get(1)!=0;
            if(model){delayBits=b.get(5)+1;b.skip(42);}
        }
        const bool displayDelay=b.get(1)!=0;const unsigned ops=b.get(5)+1;
        for(unsigned i=0;i<ops;++i) {
            b.get(12);if(b.get(5)>7)b.get(1);
            if(model && b.get(1))b.skip(2*delayBits+1);
            if(displayDelay && b.get(1))b.get(4);
        }
    }
    const unsigned widthBits=b.get(4)+1,heightBits=b.get(4)+1;b.get(widthBits);b.get(heightBits);
    if(!reduced && b.get(1))b.skip(7);
    b.skip(3);
    if(!reduced) {
        b.skip(4);const bool order=b.get(1)!=0;if(order)b.skip(2);
        const unsigned screen=b.get(1)?2:b.get(1);
        if(screen && !b.get(1))b.get(1);
        if(order)b.get(3);
    }
    b.skip(3);
    const bool high=b.get(1)!=0;const unsigned depth=profile==2 && high?(b.get(1)?12:10):(high?10:8);
    const bool mono=profile==1?false:b.get(1)!=0;Av1Color c;
    if(b.get(1)){c.primaries=b.get(8);c.transfer=b.get(8);c.matrix=b.get(8);}
    if(mono)c.full_range=b.get(1)!=0;
    else {
        if(c.primaries==1 && c.transfer==13 && c.matrix==0)c.full_range=true;
        else {
            c.full_range=b.get(1)!=0;
            unsigned x=profile==0?1:0,y=x;
            if(profile==2){x=depth==12?b.get(1):1;y=depth==12 && x?b.get(1):0;}
            if(x && y)b.get(2);
        }
        b.get(1);
    }
    b.get(1); // film_grain_params_present
    if(!b.get(1))throw Invalid{}; // trailing_one_bit
    while(b.pos<size*8)if(b.get(1))throw Invalid{};
    return c;
}
}
inline std::optional<Av1Color> av1_color(const uint8_t* data,size_t size) noexcept {
    if(!data || !size || size>65536)return {};
    try {
        size_t p=0;
        if(data[0]&0x80) { // av1C is distinguished from OBU by the marker bit.
            if(size<4 || data[0]!=0x81 || (data[1]>>5)>2 || (data[3]&0xe0) || (!(data[3]&0x10) && (data[3]&15)))return {};
            p=4;
        }
        std::optional<Av1Color> result;unsigned count=0;
        while(p<size) {
            if(++count>256)throw av1_detail::Invalid{};
            const unsigned header=data[p++],type=(header>>3)&15;
            if((header&0x81) || !(header&2))throw av1_detail::Invalid{};
            if(header&4){if(p>=size || (data[p++]&7))throw av1_detail::Invalid{};}
            uint64_t length=0;unsigned shift=0;
            for(;;) {
                if(p>=size || shift>=56)throw av1_detail::Invalid{};
                const unsigned v=data[p++];length|=uint64_t(v&127)<<shift;shift+=7;
                if(!(v&128))break;
            }
            if(length>size-p)throw av1_detail::Invalid{};
            if(type==1) {
                auto c=av1_detail::sequence(data+p,static_cast<size_t>(length));
                if(result && (result->primaries!=c.primaries || result->transfer!=c.transfer || result->matrix!=c.matrix || result->full_range!=c.full_range))throw av1_detail::Invalid{};
                result=c;
            }
            p+=static_cast<size_t>(length);
        }
        return result;
    }catch(...){return {};}
}
}
