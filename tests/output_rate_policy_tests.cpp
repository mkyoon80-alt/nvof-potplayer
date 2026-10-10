#include "nvof/output_rate_policy.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>

void require(bool condition,const char* text) { if(!condition)throw std::runtime_error(text); }
void expect(int64_t duration,nvof::Rate expected) {
    const auto actual=nvof::double_source_rate(duration);
    require(actual.num==expected.num && actual.den==expected.den,"incorrect x2 output rate");
}
int main() {
    try {
        expect(417083,{48000,1001}); expect(417084,{48000,1001});
        const auto canonical=nvof::canonical_source_rate(417083);
        require(canonical.num==24000 && canonical.den==1001,"rounded NTSC source cadence must be canonical");
        const auto unusual=nvof::canonical_source_rate(416999);
        require(unusual.num==10000000 && unusual.den==416999,"unusual source cadence must retain exact duration");
        require(nvof::canonical_source_rate(0).num==0,"unknown cadence must stay unknown");
        expect(416667,{48,1}); expect(400000,{50,1});
        expect(333667,{60000,1001}); expect(333333,{60,1});
        expect(208333,{96,1}); expect(200000,{100,1});
        expect(166833,{120000,1001}); expect(166667,{120,1});
        expect(83333,{240,1}); expect(69444,{288,1}); expect(60606,{330,1});
        expect(20000,{1000,1}); expect(19999,{0,1});
        // Narrow canonical matching must not turn unusual source FPS into24p.
        expect(416999,{20000000,416999}); expect(417085,{4000000,83417});
        expect(0,{0,1}); expect(-1,{0,1});
        expect((std::numeric_limits<int64_t>::max)(),{0,1});
        require(nvof::supported_output_rate({20000000,416999}),"exact unusual x2 rational must fit");
        require(nvof::supported_output_rate({1000000000,10000000}),"safe numeric boundary rejected");
        require(!nvof::supported_output_rate({1000000001,10000000}),"unsafe numerator accepted");
        require(!nvof::supported_output_rate({1,10000001}),"unsafe denominator accepted");
        require(!nvof::supported_output_rate({1001,1}),"over-limit FPS accepted");
        struct Case { int64_t duration; nvof::Rate low,high; int lowMultiple,highMultiple; };
        for(const auto c : {Case{417083,{48000,1001},{120000,1001},2,5},
            Case{416667,{48,1},{120,1},2,5},Case{400000,{50,1},{100,1},2,4},
            Case{333667,{60000,1001},{120000,1001},2,4},Case{333333,{60,1},{120,1},2,4},
            Case{200000,{50,1},{100,1},1,2},Case{166833,{60000,1001},{120000,1001},1,2},
            Case{166667,{60,1},{120,1},1,2},Case{83333,{120,1},{120,1},1,1},
            Case{69444,{144,1},{144,1},1,1}}) {
            for(int limit : {60,120}) {
                const auto actual=nvof::capped_source_rate(c.duration,limit),expected=limit==60?c.low:c.high;
                require(actual.num*expected.den==expected.num*actual.den,"integer limit cadence differs from table");
                require(nvof::output_multiple(c.duration,limit)==(limit==60?c.lowMultiple:c.highMultiple),"wrong multiplier");
            }
        }
        require(nvof::output_multiple(10000000,120)==33,"low-rate phase bound missing");
        require(nvof::capped_source_rate(0,120).num==0,"unknown input invented a cadence");
        require(nvof::capped_source_rate(-1,60).num==0,"negative input invented a cadence");
        require(nvof::output_multiple(417083,119)==2,"invalid limit must default to 60");
        const auto odd=nvof::capped_source_rate(416999,120);
        require(odd.num==50000000 && odd.den==416999,"nonstandard rate was rounded up across the cap");
        require(nvof::output_multiple(416600,120)==4,"nonstandard rate above 24 crossed the 120 cap");
        // Constructor must use the same bounds as the target policy.
        nvof::HybridPipeline precise({20000000,416999},[](const nvof::Frame& a,const nvof::Frame& b) {
            auto middle=a;middle.pts=(a.pts+b.pts)/2;return middle;
        });
        int count=0;
        auto emit=[&](const nvof::OutputFrame& f) {
            require(f.frame.pts==int64_t(count)*416999/2,"x2 clock drift");
            require(f.stop>f.frame.pts,"empty x2 output duration");++count;return true;
        };
        for(int i=0;i<100;++i) precise.push({2,2,int64_t(i)*416999,std::vector<uint8_t>(6,128)},false,emit);
        precise.finish(416999,emit);
        require(count==200,"x2 must emit exactly double for a CFR clip");
        std::cout<<"PASS x2 rate policy: standard/NTSC, unusual exact rational, high FPS, safe bounds and 2x clock\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
