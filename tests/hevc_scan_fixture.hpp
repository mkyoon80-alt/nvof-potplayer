#pragma once
#include "hevc_color_fixture.hpp"
namespace hevc_scan_fixture {
struct Scan { bool progressive=true,interlaced=false,frame_only=true,fields=false,vui=true;unsigned sub=0; };
inline std::vector<uint8_t> sps(Scan scan={},unsigned transfer=1) {
    hevc_fixture::Writer b;
    b.put(0,4);b.put(scan.sub,3);b.put(1);
    b.put(2,8);b.put(1u<<29,32);b.put(scan.progressive);b.put(scan.interlaced);b.put(1);b.put(scan.frame_only);b.zeros(44);b.put(120,8);
    for(unsigned i=0;i<scan.sub;++i){b.put(0);b.put(1);}
    if(scan.sub)for(unsigned i=scan.sub;i<8;++i)b.zeros(2);
    for(unsigned i=0;i<scan.sub;++i)b.put(90,8);
    b.ue(0);b.ue(1);b.ue(640);b.ue(360);b.put(0);b.ue(2);b.ue(2);b.ue(4);
    b.put(0);b.ue(4);b.ue(0);b.ue(0);for(int i=0;i<6;++i)b.ue(0);
    b.put(0);b.put(1);b.put(1);b.put(0);b.ue(0);b.put(0);b.put(1);b.put(1);b.put(scan.vui);
    if(scan.vui) {
        b.put(0);b.put(0);b.put(1);b.put(5,3);b.put(0);b.put(1);b.put(transfer==1?1:9,8);b.put(transfer,8);b.put(transfer==1?1:9,8);
        b.put(0);b.put(0);b.put(scan.fields);b.put(1);b.put(0);b.put(0);b.put(0);
    }
    b.put(0);b.put(1);while(b.pos%8)b.put(0);
    std::vector<uint8_t> nal{0x42,1};unsigned zeros=0;
    for(auto value:b.v){if(zeros==2 && value<=3){nal.push_back(3);zeros=0;}nal.push_back(value);zeros=value==0?zeros+1:0;}
    return nal;
}
inline std::vector<uint8_t> hvcc(const std::vector<std::vector<uint8_t>>& spses) {
    std::vector<uint8_t> result(23);result[0]=1;result[21]=3;result[22]=1;
    result.push_back(0xa1);result.push_back(uint8_t(spses.size()>>8));result.push_back(uint8_t(spses.size()));
    for(const auto& nal:spses){result.push_back(uint8_t(nal.size()>>8));result.push_back(uint8_t(nal.size()));result.insert(result.end(),nal.begin(),nal.end());}
    return result;
}
inline std::vector<uint8_t> annex(const std::vector<std::vector<uint8_t>>& spses) {
    std::vector<uint8_t> result;for(const auto& nal:spses){result.insert(result.end(),{0,0,0,1});result.insert(result.end(),nal.begin(),nal.end());}return result;
}
}
