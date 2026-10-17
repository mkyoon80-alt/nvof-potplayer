#include "nvof/hevc_scan.hpp"
#include "hevc_scan_fixture.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
bool progressive(const std::vector<uint8_t>& bytes){return nvof::hevc_progressive_only(bytes.data(),bytes.size());}
int main(int argc,char** argv) {try {
    using namespace hevc_scan_fixture;
    unsigned cases=0;
    for(unsigned mask=0;mask<16;++mask)for(bool vui:{false,true})for(unsigned sub:{0u,2u,6u}) {
        Scan scan{bool(mask&1),bool(mask&2),bool(mask&4),bool(mask&8),vui,sub};
        const bool expected=scan.progressive && !scan.interlaced && scan.frame_only && (!vui || !scan.fields);
        auto nal=sps(scan);auto data=hvcc({nal});
        require(progressive(data)==expected,"hvcC scan constraints");
        require(progressive(annex({nal}))==expected,"Annex B scan constraints");
        for(size_t n=0;n<data.size();++n)require(!nvof::hevc_progressive_only(data.data(),n),"truncated hvcC authorized correction");
        cases+=2;
    }
    const auto p=sps();const auto interlaced=sps({false,true,false,true});
    for(const auto& ns:std::vector<std::vector<std::vector<uint8_t>>>{{p,interlaced},{interlaced,p},{p,sps({true,true,true,false})},{p,sps({false,false,true,false})}}) {
        require(!progressive(hvcc(ns)) && !progressive(annex(ns)),"mixed/conflicting/unknown SPS authorized correction");
    }
    require(progressive(hvcc({p,p})) && progressive(annex({p,p})),"consistent multiple SPS");
    auto damaged=p;damaged[1]|=8;require(!progressive(hvcc({p,damaged})),"multilayer SPS authorized correction");
    damaged=p;damaged[0]|=0x80;require(!progressive(hvcc({damaged})),"forbidden bit");
    damaged=p;damaged[1]=0;require(!progressive(hvcc({damaged})),"invalid temporal id");
    require(!progressive(hvcc({})) && !nvof::hevc_progressive_only(nullptr,0),"missing SPS");
    auto data=hvcc({p});data.push_back(0);require(!progressive(data),"unframed hvcC trailing byte");
    data=hvcc({p});data[26]=255;data[27]=255;require(!progressive(data),"oversized NAL");
    require(!progressive(hevc_fixture::hvcc()),"color-only/unspecified header changed cadence");
    for(unsigned transfer:{1u,16u,18u}) {
        data=hvcc({sps({},transfer)});const auto color=nvof::hevc_color(data.data(),data.size());
        require(progressive(data) && color && color->transfer==transfer,"SDR/PQ/HLG color preserved");
    }
    uint32_t random=19;for(unsigned i=0;i<4000;++i){data=hvcc({p});for(unsigned j=0;j<4;++j){random=random*1664525+1013904223;data[random%data.size()]=uint8_t(random>>24);}progressive(data);}
    if(argc>1){std::ifstream f(argv[1],std::ios::binary);data.assign(std::istreambuf_iterator<char>(f),{});require(progressive(data),"actual HEVC progressive header");}
    std::cout<<"PASS HEVC scan cases="<<cases<<"; true/false/unknown/mixed/field sequence, sublayers, framing, bounds, color, mutations\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
