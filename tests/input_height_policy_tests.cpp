#include "nvof/input_height_policy.hpp"
#include <cassert>
#include <iostream>
int main(){
using namespace nvof;
for(unsigned limit: {0u,1080u,1440u})for(int height: {720,1080,1082,1440,1442,1604,2160,3840})
 if(input_height_selected(height,limit)!=(limit==0 || height<=int(limit)))return 1;
if(input_height_selected(0,0) || input_height_selected(-1080,1080) || normalize_height_limit(2160)!=0)return 2;
std::cout<<"Vertical-only height boundary and invalid setting checks OK\n";
}
