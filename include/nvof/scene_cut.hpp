#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <initializer_list>
namespace nvof {
inline constexpr unsigned scene_grid_width=64,scene_grid_height=36;
// Only aggregate statistics cross the GPU/CPU boundary, never video planes.
struct SceneStats {
    uint32_t samples=0, absolute_sum=0;
    int32_t signed_sum=0;
    uint32_t squared_sum=0, changed=0;
    uint32_t histogram_a[32]{},histogram_b[32]{};
};
static_assert(sizeof(SceneStats)==69*sizeof(uint32_t));
struct SceneDecision {bool cut=false;double difference=0,coverage=0,histogram_distance=0,variation=0;};
inline SceneDecision classify_scene(const SceneStats& s) {
    SceneDecision d{};if(!s.samples)return d;
    d.difference=double(s.absolute_sum)/s.samples;d.coverage=double(s.changed)/s.samples;
    const double mean=double(s.signed_sum)/s.samples;
    d.variation=std::sqrt((std::max)(0.0,double(s.squared_sum)/s.samples-mean*mean));
    for(unsigned i=0;i<32;++i)d.histogram_distance+=std::abs(double(s.histogram_a[i])-s.histogram_b[i]);
    d.histogram_distance/=2.0*s.samples;
    // Conservative hard-cut decision: require widespread change, a changed
    // distribution and non-uniform residual. Uniform flashes are not cuts.
    d.cut=d.difference>=32.0&&d.coverage>=0.80&&d.histogram_distance>=0.30&&d.variation>=18.0;
    return d;
}
inline void add_scene_sample(SceneStats& s,unsigned a,unsigned b) {
    const int delta=int(b)-int(a);const unsigned absolute=unsigned(std::abs(delta));
    ++s.samples;s.absolute_sum+=absolute;s.signed_sum+=delta;s.squared_sum+=unsigned(delta*delta);
    s.changed+=absolute>=32;++s.histogram_a[a>>3];++s.histogram_b[b>>3];
}
inline SceneStats scene_stats_cpu(const uint8_t* a,const uint8_t* b,int width,int height) {
    SceneStats s{};
    for(unsigned gy=0;gy<scene_grid_height;++gy)for(unsigned gx=0;gx<scene_grid_width;++gx){
        unsigned va=0,vb=0;
        for(unsigned oy:{1u,3u})for(unsigned ox:{1u,3u}){
            const unsigned x=(gx*4+ox)*unsigned(width)/(scene_grid_width*4);
            const unsigned y=(gy*4+oy)*unsigned(height)/(scene_grid_height*4);
            va+=a[size_t(y)*width+x];vb+=b[size_t(y)*width+x];
        }
        add_scene_sample(s,(va+2)/4,(vb+2)/4);
    }
    return s;
}
}
