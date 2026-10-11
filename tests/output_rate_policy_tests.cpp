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
        require(nvof::output_multiple(416600,120)==5,"tiny 24p deviation lost a phase");
        require(nvof::output_multiple(415000,120)==4,"material rate excess must not cross the cap");
        for(int limit : {60,120}) {
            const int expected=limit/30;
            require(nvof::output_multiple(333300,limit)==expected,"30.003 fps lost a multiplier");
            const auto preserved=nvof::capped_source_rate(333300,limit);
            require(preserved.num*333300LL==10000000LL*expected*preserved.den,"tolerant cap retimed the source");
            require(nvof::output_multiple(333001,limit)==expected,"in-tolerance boundary rejected");
            require(nvof::output_multiple(333000,limit)==expected-1,"above-tolerance boundary accepted");
            require(nvof::output_multiple(332226,limit)==expected-1,"30.1 fps excess treated as rounding");
        }
        require(nvof::output_multiple(166650,60)==1 && nvof::output_multiple(166650,120)==2,
            "60.006 fps must bypass at 60 and double at 120");
        // Published UI choices: caps retain source originals and direct modes
        // ignore the remembered cap. Invalid new keys preserve old defaults.
        const int caps[]={60,72,75,100,120,144,240};
        const int expected24[]={2,3,3,4,5,6,10},expected25[]={2,2,3,4,4,5,9};
        for(int i=0;i<7;++i) {
            require(nvof::output_multiple(416667,caps[i])==expected24[i],"24p expanded cap mismatch");
            require(nvof::output_multiple(400000,caps[i])==expected25[i],"25p expanded cap mismatch");
            require(nvof::output_multiple(10000000,caps[i])<=33,"Expanded cap exceeds phase pool");
        }
        for(int multiple=2;multiple<=5;++multiple)for(int64_t duration:{417083LL,416667LL,400000LL,333300LL,166667LL}) {
            const auto source=nvof::canonical_source_rate(duration);
            const auto rate=nvof::selected_output_rate(duration,1,60,multiple);
            require(rate.num*source.den==source.num*multiple*rate.den,"Direct multiple lost exact source cadence");
            require(nvof::selected_output_multiple(duration,1,60,multiple)==multiple,"Direct mode still capped");
        }
        require(nvof::selected_output_rate(0,1,120,3).num==0,"Direct mode invented unknown source");
        require(nvof::selected_output_rate(25000,1,240,3).num==0,"Direct mode exceeded 1000 fps timing bound");
        require(nvof::selected_output_multiple(416667,9,72,5)==3,"Invalid mode must preserve cap semantics");
        require(nvof::selected_output_multiple(416667,1,240,6)==2,"Invalid multiplier must default x2");
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
