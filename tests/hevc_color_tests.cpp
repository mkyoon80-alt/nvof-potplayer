#include "nvof/hevc_color.hpp"
#include "hevc_color_fixture.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
void require(bool p,const char* label){if(!p)throw std::runtime_error(label);}
int main(int argc,char**argv) {try{
    for(unsigned transfer:{1u,16u,18u})for(bool lists:{false,true})for(bool full:{false,true}) {
        auto data=hevc_fixture::hvcc(transfer,9,9,full,lists);auto c=nvof::hevc_color(data.data(),data.size());
        require(c && c->transfer==transfer && c->primaries==9 && c->matrix==9 && c->full_range==full,"hvcC VUI");
        for(size_t i=0;i<data.size();++i)require(!nvof::hevc_color(data.data(),i),"truncated hvcC accepted");
        auto nal=hevc_fixture::sps(transfer,9,9,full,lists);std::vector<uint8_t> annex{0,0,0,1};annex.insert(annex.end(),nal.begin(),nal.end());
        c=nvof::hevc_color(annex.data(),annex.size());require(c && c->transfer==transfer,"Annex B VUI");
        data[26]=0xff;data[27]=0xff;require(!nvof::hevc_color(data.data(),data.size()),"oversized NAL accepted");
    }
    auto a=hevc_fixture::hvcc();auto b=hevc_fixture::sps(18);a[25]=2;a.push_back(0);a.push_back(uint8_t(b.size()));a.insert(a.end(),b.begin(),b.end());
    require(!nvof::hevc_color(a.data(),a.size()),"conflicting SPS accepted");
    uint32_t random=7;for(unsigned n=0;n<4000;++n){auto v=hevc_fixture::hvcc();for(unsigned k=0;k<4;++k){random=random*1664525+1013904223;v[random%v.size()]=uint8_t(random>>24);}nvof::hevc_color(v.data(),v.size());}
    if(argc>1){std::ifstream f(argv[1],std::ios::binary);std::vector<uint8_t> v((std::istreambuf_iterator<char>(f)),{});
        require(v.size()>132,"actual media fixture missing");auto c=nvof::hevc_color(v.data()+132,v.size()-132);
        require(c && c->primaries==9 && c->matrix==9 && c->transfer==16 && !c->full_range,"actual HEVC HDR header");
        std::cout<<"Actual decoder input header: BT.2020 / PQ / limited verified\n";}
    std::cout<<"PASS HEVC PQ/HLG/SDR, hvcC/Annex B, scaling/RPS syntax, truncation/conflicts/bounds\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
