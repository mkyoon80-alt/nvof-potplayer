#include "nvof/output_buffer_policy.hpp"
#include <iostream>
#include <stdexcept>
int main() {
    try {
        auto check=[](bool value){if(!value)throw std::runtime_error("output headroom policy regression");};
        constexpr uint64_t p010_8k=7680ULL*4320*3;
        check(nvof::gpu_ready_output_count(4,{240,1},p010_8k)==10); // >=40 ms, <1 GiB.
        check(nvof::gpu_ready_output_count(4,{240000,1001},p010_8k)==10);
        check(nvof::gpu_ready_output_count(10,{240,1},3840ULL*1604*3)==12); // Existing batch floor.
        check(nvof::gpu_ready_output_count(2,{120,1},p010_8k)==0);
        check(nvof::gpu_ready_output_count(5,{120000,1001},1920ULL*1080*3)==0);
        check(nvof::gpu_ready_output_count(4,{144,1},p010_8k)==6);
        check(nvof::gpu_ready_output_count(4,{480,1},p010_8k)==10); // Memory wins over time.
        check(nvof::gpu_ready_output_count(33,{990,1},1920ULL*1080*3)==32);
        check(nvof::gpu_ready_output_count(INT_MAX,{240,1},p010_8k)==10);
        check(nvof::gpu_ready_output_count(4,{0,1},p010_8k)==0);
        check(nvof::gpu_ready_output_count(4,{240,0},p010_8k)==0);
        check(nvof::gpu_ready_output_count(4,{240,1},0)==0);
        check(nvof::gpu_ready_output_count(4,{240,1},UINT64_MAX)==0);
        check(nvof::gpu_output_buffer_count(10,4,{240,1},p010_8k)==16);
        check(nvof::gpu_output_buffer_count(10,4,{240000,1001},p010_8k)==16);
        check(nvof::gpu_output_buffer_count(10,2,{120,1},p010_8k)==10);
        check(nvof::gpu_output_buffer_count(4,10,{240,1},3840ULL*1604*3)==20);
        check(nvof::gpu_output_buffer_count(4,5,{120000,1001},1920ULL*1080*3)==10);
        check(nvof::gpu_output_buffer_count(24,4,{240,1},p010_8k)==24); // Host contract.
        check(nvof::gpu_output_buffer_count(4,5,{300,1},p010_8k)==16); // Bounded growth.
        check(nvof::gpu_output_buffer_count(4,33,{990,1},p010_8k)==32); // Legacy floor.
        check(nvof::gpu_output_buffer_count(4,4,{0,1},p010_8k)==8);
        check(nvof::gpu_output_buffer_count(4,4,{240,0},p010_8k)==8);
        check(nvof::gpu_output_buffer_count(4,4,{240,1},0)==8);
        check(nvof::gpu_output_buffer_count(4,4,{240,1},UINT64_MAX)==8);
        std::cout<<"time-based capacity, rational cadence, host floor and bounded memory=OK\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
