#pragma once
#include <vector>
#include <cstdint>
namespace hevc_fixture {
struct Writer {
    std::vector<uint8_t> v;unsigned pos=0;
    void put(unsigned x,unsigned n=1){for(unsigned i=n;i;--i){if(!(pos%8))v.push_back(0);v.back()|=((x>>(i-1))&1)<<(7-pos%8);++pos;}}
    void zeros(unsigned n){while(n--)put(0);}
    void ue(unsigned x){++x;unsigned bits=0;for(unsigned t=x;t;t>>=1)++bits;zeros(bits-1);put(x,bits);}
};
inline std::vector<uint8_t> sps(unsigned transfer=16,unsigned primaries=9,unsigned matrix=9,bool full=false,bool lists=false) {
    Writer b;b.put(0,4);b.put(0,3);b.put(1);b.zeros(96);b.ue(0);b.ue(1);b.ue(640);b.ue(360);b.put(0);b.ue(2);b.ue(2);b.ue(4);
    b.put(0);b.ue(4);b.ue(0);b.ue(0);for(int i=0;i<6;++i)b.ue(0);
    b.put(lists);if(lists){b.put(1);for(unsigned s=0;s<4;++s)for(unsigned m=0;m<6;m+=s==3?3:1){b.put(0);b.ue(0);}}
    b.put(0);b.put(1);b.put(0);b.ue(2); // RPS with one explicit and one predicted set.
    b.ue(1);b.ue(0);b.ue(0);b.put(1);b.put(1);b.put(0);b.ue(0);b.put(1);b.put(0);b.put(0);
    b.put(1);b.ue(1);b.put(0,8);b.put(1); // Long term reference.
    b.put(1);b.put(1);b.put(1); // temporal MVP, smoothing, VUI.
    b.put(1);b.put(255,8);b.put(1,16);b.put(1,16);b.put(1);b.put(0); // aspect and overscan.
    b.put(1);b.put(5,3);b.put(full);b.put(1);b.put(primaries,8);b.put(transfer,8);b.put(matrix,8);b.put(1);
    std::vector<uint8_t> nal{0x42,1};unsigned z=0;
    for(auto x:b.v){if(z==2 && x<=3){nal.push_back(3);z=0;}nal.push_back(x);z=x==0?z+1:0;}return nal;
}
inline std::vector<uint8_t> hvcc(unsigned transfer=16,unsigned primaries=9,unsigned matrix=9,bool full=false,bool lists=false) {
    auto nal=sps(transfer,primaries,matrix,full,lists);std::vector<uint8_t> h(23);h[0]=1;h[21]=3;h[22]=1;
    h.push_back(0xa1);h.push_back(0);h.push_back(1);h.push_back(uint8_t(nal.size()>>8));h.push_back(uint8_t(nal.size()));h.insert(h.end(),nal.begin(),nal.end());return h;
}
}
