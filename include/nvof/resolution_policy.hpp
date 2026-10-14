#pragma once
#include "output_rate_policy.hpp"
#include <array>
#include <charconv>
#include <string_view>

namespace nvof {
constexpr std::array<int,7> resolution_heights{0,480,720,1080,1440,2160,4320};
using ResolutionRules=std::array<int,7>;
// 0 inherits the common setting, -1 preserves originals, -2..-5 selects a
// direct multiplier; positive values select an existing integer-cadence cap.
constexpr bool valid_resolution_rule(int rule) noexcept {
    return (rule>=-5 && rule<=0) || rule==60 || rule==72 || rule==75 ||
        rule==100 || rule==120 || rule==144 || rule==240;
}
inline ResolutionRules parse_resolution_rules(std::string_view text) noexcept {
    ResolutionRules result{};
    for(size_t i=0;i<result.size();++i) {
        const size_t comma=text.find(',');
        auto token=text.substr(0,comma);
        while(!token.empty() && (token.front()==' ' || token.front()=='\t'))token.remove_prefix(1);
        while(!token.empty() && (token.back()==' ' || token.back()=='\t'))token.remove_suffix(1);
        if(token.empty())return {};
        int value=0;const auto parsed=std::from_chars(token.data(),token.data()+token.size(),value);
        if(parsed.ec!=std::errc{} || parsed.ptr!=token.data()+token.size() || !valid_resolution_rule(value))return {};
        result[i]=value;
        if(i+1==result.size())return comma==std::string_view::npos?result:ResolutionRules{};
        if(comma==std::string_view::npos)return {};
        text.remove_prefix(comma+1);
    }
    return {};
}
constexpr size_t resolution_band(int height) noexcept {
    size_t band=0;
    for(size_t i=1;i<resolution_heights.size();++i)if(height>=resolution_heights[i])band=i;
    return band;
}
struct ResolutionOutput {
    int mode=0,limit=60,multiplier=2,rule=0;
    bool disabled=false;
};
inline ResolutionOutput resolve_resolution_output(int height,bool enabled,const ResolutionRules& rules,
    int mode,int limit,int multiplier) noexcept {
    ResolutionOutput output{normalize_output_mode(mode),normalize_output_limit(limit),normalize_output_multiplier(multiplier)};
    if(!enabled || height<=0)return output;
    const int rule=rules[resolution_band(height)];
    if(!valid_resolution_rule(rule))return output;
    output.rule=rule;
    if(rule==-1)output.disabled=true;
    else if(rule<=-2){output.mode=1;output.multiplier=-rule;}
    else if(rule>0){output.mode=0;output.limit=rule;}
    return output;
}
}
