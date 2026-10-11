#include "nvof/av1_color.hpp"
#include "av1_color_fixture.hpp"
#include <iostream>
#include <stdexcept>
#include <fstream>
#include <iterator>
void require(bool v){if(!v)throw std::runtime_error("AV1 color test failed");}
int main(int argc,char** argv){try{
 for(unsigned t: {1u,16u,18u}) {
    const auto& bytes=t==1?av1_fixture::sdr:t==16?av1_fixture::pq:av1_fixture::hlg;
    for(unsigned offset:{0u,4u}){auto c=nvof::av1_color(bytes.data()+offset,bytes.size()-offset);require(c && c->transfer==t && c->primaries==(t==1?1:9) && c->matrix==(t==1?1:9) && !c->full_range);}
    for(size_t n=0;n<bytes.size();++n)require(!nvof::av1_color(bytes.data(),n));
 }
 auto bad=av1_fixture::pq;bad[4]|=0x80;require(!nvof::av1_color(bad.data(),bad.size()));
 bad=av1_fixture::pq;bad[5]=0xff;require(!nvof::av1_color(bad.data(),bad.size()));
 bad=av1_fixture::pq;bad.insert(bad.end(),av1_fixture::hlg.begin()+4,av1_fixture::hlg.end());require(!nvof::av1_color(bad.data(),bad.size()));
 for(unsigned n=0;n<65536;++n){uint8_t x[]={uint8_t(n>>8),uint8_t(n)};require(!nvof::av1_color(x,2));}
 if(argc>1){std::ifstream f(argv[1],std::ios::binary);std::vector<uint8_t> b{std::istreambuf_iterator<char>(f),{}};auto c=nvof::av1_color(b.data(),b.size());require(bool(c));std::cout<<"actual header: "<<c->primaries<<'/'<<c->transfer<<'/'<<c->matrix<<'\n';}
 std::cout<<"AV1 av1C/raw OBU PQ/HLG/SDR/truncated/malformed/conflicting OK\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
