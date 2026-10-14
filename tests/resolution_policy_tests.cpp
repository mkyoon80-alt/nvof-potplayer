#include "nvof/resolution_policy.hpp"
#include <iostream>
#include <stdexcept>
using namespace nvof;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main() {
    try {
        const auto rules=parse_resolution_rules("0,240,144,120,-3,-2,-1");
        require(rules==ResolutionRules{0,240,144,120,-3,-2,-1},"valid settings lost");
        for(const char* invalid:{"","0,120","0,0,0,0,0,0,0,0","0,0,0,0,0,0,","0,0,0,0,0,0,-6",
            "0,0,0,0,0,0,119","0,0,0,0,0,0,120junk","0,0,0,0,0,0,99999999999999999"})
            require(parse_resolution_rules(invalid)==ResolutionRules{},"malformed settings accepted");
        require(parse_resolution_rules(" 0 , 240,144 ,120,-3,-2,-1 ")==rules,"whitespace rejected");
        for(int height=1;height<=8192;++height) {
            size_t expected=height<480?0:height<720?1:height<1080?2:height<1440?3:height<2160?4:height<4320?5:6;
            require(resolution_band(height)==expected,"resolution boundary overlap or gap");
            const auto selected=resolve_resolution_output(height,true,rules,0,75,5);
            require(selected.rule==rules[expected],"wrong resolution rule");
            const auto common=resolve_resolution_output(height,false,rules,1,75,5);
            require(common.mode==1 && common.multiplier==5 && common.limit==75 && !common.disabled && common.rule==0,"off changed common setting");
        }
        require(resolve_resolution_output(800,true,rules,0,60,2).limit==144,"1920x800 must use height not width");
        const auto hd=resolve_resolution_output(1080,true,rules,1,60,2);
        const auto uhd=resolve_resolution_output(2160,true,rules,0,240,5);
        require(selected_output_multiple(417083,hd.mode,hd.limit,hd.multiplier)==5,"1080p cap mismatch");
        require(selected_output_multiple(417083,uhd.mode,uhd.limit,uhd.multiplier)==2,"2160p multiplier mismatch");
        require(resolve_resolution_output(4320,true,rules,0,120,2).disabled,"8K disable lost");
        const auto inherited=resolve_resolution_output(360,true,rules,1,120,4);
        require(inherited.mode==1 && inherited.multiplier==4,"inherit lost common mode");
        require(!resolve_resolution_output(0,true,rules,0,60,2).disabled,"unknown height must inherit");
        std::cout<<"PASS resolution ranges 1..8192, settings parsing, inheritance, cap/multiplier/off\n";
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
