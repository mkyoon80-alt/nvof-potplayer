#include "nvof/pipeline.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace nvof;
void require(bool condition, const char* what) { if(!condition) throw std::runtime_error(what); }
Frame make(int64_t pts,uint8_t level) { return Frame{8,4,pts,std::vector<uint8_t>(48,level)}; }
Frame midpoint(const Frame& a,const Frame& b) { auto m=make(a.pts+(b.pts-a.pts)/2,128); return m; }
void cadence(int target) {
    std::vector<OutputFrame> output;
    HybridPipeline p({target*1000LL,1001},midpoint);
    auto emit=[&](const OutputFrame& f){output.push_back(f);return true;};
    // Ten seconds at 24000/1001: 240 source frames, exact matching target clock.
    for(int i=0;i<240;++i) p.push(make(int64_t(i)*10000000*1001/24000,uint8_t(i%200)),false,emit);
    p.finish(10000000LL*1001/24000,emit);
    require(output.size()==size_t(target*10),"wrong frame count");
    for(size_t i=0;i<output.size();++i) {
        require(output[i].frame.pts==int64_t(i)*10000000*1001/(target*1000),"timestamp drift");
        require(output[i].stop>output[i].frame.pts,"nonpositive sample duration");
        if(i) require(output[i-1].stop==output[i].frame.pts,"timestamp gap/overlap");
    }
}
int main() { try {
    cadence(60);cadence(120);
    int calls=0; std::vector<OutputFrame> output;
    HybridPipeline p({60,1},[&](const Frame&a,const Frame&b){++calls;return midpoint(a,b);});
    auto emit=[&](const OutputFrame& f){output.push_back(f);return true;};
    p.push(make(1000000,10),false,emit);
    require(output.size()==1 && calls==0,"first seek frame must be immediate");
    p.push(make(1416667,20),false,emit);
    require(calls==1,"one native 2x midpoint per input pair");
    size_t before=output.size();
    p.push(make(0,200),true,emit);
    require(output.size()==before+1 && output.back().frame.pixels[0]==200 && output.back().discontinuity,"seek leaked old frame");
    for(int i=0;i<1000;++i) { p.reset();p.push(make(i%2?90000000:0,uint8_t(i%250)),false,emit); }
    require(calls==1,"reset/first frame invoked GPU unnecessarily");
    p.reset();p.push(make(0,1),false,[](const auto&){return false;});
    before=output.size();p.push(make(20000000,90),false,emit);
    require(output.size()==before+1 && output.back().discontinuity,"cancel retained pending history");
    HybridPipeline precise({23999999,100000},midpoint);
    int64_t output_index=0;
    auto precise_emit=[&](const OutputFrame& f) {
        const int64_t expected=int64_t((static_cast<long double>(output_index)*10000000*100000)/23999999);
        require(f.frame.pts==expected,"high precision clock overflow/drift");
        ++output_index;return true;
    };
    for(int i=0;i<3000;++i) precise.push(make(int64_t(i)*10000000/24,10),false,precise_emit);
    require(output_index>29000,"long timing regression too short");
    bool bad=false; try { p.push(Frame{3,4,0,{}},false,emit); } catch(const std::invalid_argument&){bad=true;}
    require(bad,"invalid NV12 not rejected");
    HybridPipeline failure({60,1},[](const Frame&,const Frame&)->Frame {throw std::runtime_error("GPU failed");});
    failure.push(make(0,0),false,emit);bad=false;
    try {failure.push(make(416667,1),false,emit);}catch(const std::runtime_error&){bad=true;}
    require(bad,"GPU error silently hidden");
    std::cout << "PASS: NTSC 60/120 cadence, monotonic timestamps, immediate seek preview, 1000 resets, cancellation, format validation, visible GPU failures\n";
    return 0;
} catch(const std::exception&e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;} }

