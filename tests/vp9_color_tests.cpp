#include "nvof/vp9_color.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>

void require(bool condition,const char* text){if(!condition)throw std::runtime_error(text);}
int main() try {
    // vpcC payload: profile 2, level 5.1, 10-bit 4:2:0 limited BT.2020 PQ.
    const std::vector<uint8_t> record={2,51,0xa2,9,16,9,0,0};
    std::vector<std::vector<uint8_t>> forms={record};
    auto full=record;full.insert(full.begin(),{1,0,0,0});forms.push_back(full);
    auto dshow=full;dshow.insert(dshow.begin(),{'v','p','c','C'});forms.push_back(dshow);
    auto box=dshow;box.insert(box.begin(),{0,0,0,20});forms.push_back(box);
    for(const auto& bytes:forms) {
        auto color=nvof::vp9_color(bytes.data(),bytes.size());
        require(color && color->primaries==9 && color->transfer==16 && color->matrix==9 && !color->full_range,"PQ header");
        for(size_t n=0;n<bytes.size();++n)require(!nvof::vp9_color(bytes.data(),n),"truncation accepted");
        auto excess=bytes;excess.push_back(0);require(!nvof::vp9_color(excess.data(),excess.size()),"trailing data accepted");
    }
    auto hlg=record;hlg[4]=18;require(nvof::vp9_color(hlg.data(),hlg.size())->transfer==18,"HLG header");
    auto sdr=record;sdr[3]=sdr[4]=sdr[5]=1;require(nvof::vp9_color(sdr.data(),sdr.size())->transfer==1,"10-bit SDR changed");
    auto unspecified=record;unspecified[4]=2;require(nvof::vp9_color(unspecified.data(),8)->transfer==2,"BT.2020 inferred as HDR");
    auto range=record;range[2]|=1;require(nvof::vp9_color(range.data(),8)->full_range,"full-range bit lost");
    for(auto index:{0,1,2,6,7}) {auto bad=record;bad[index]=255;require(!nvof::vp9_color(bad.data(),bad.size()),"invalid record accepted");}
    for(auto index:{4,5,6,7}) {auto bad=dshow;bad[index]=2;require(!nvof::vp9_color(bad.data(),bad.size()),"invalid version/flags accepted");}
    auto wrong_size=box;wrong_size[3]=19;require(!nvof::vp9_color(wrong_size.data(),wrong_size.size()),"bad box length accepted");
    auto depth=record;depth[2]=0x82;require(!nvof::vp9_color(depth.data(),8),"profile/depth mismatch accepted");
    auto subsampling=record;subsampling[2]=0xa4;require(!nvof::vp9_color(subsampling.data(),8),"profile/chroma mismatch accepted");
    require(!nvof::vp9_color(nullptr,20),"null accepted");
    require(!nvof::vp9_color(record.data(),65537),"unbounded header accepted");
    std::cout<<"PASS VP9 vpcC PQ/HLG/SDR, wrappers, truncation, bounds and no HDR inference\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
