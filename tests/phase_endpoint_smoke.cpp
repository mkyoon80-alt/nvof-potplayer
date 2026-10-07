#include "nvof/engine.hpp"
#include "nvof/phase_pipeline.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Opt-in NVIDIA hardware diagnostic. --schedule-only never constructs CUDA.
// Link nvof_engine and nvof_phase_pipeline; do not register this with CTest.
namespace {
constexpr int64_t units=10000000;
constexpr int source_num=24000, source_den=1001;
constexpr double displacement=4;
int width=640, height=360;

void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
int64_t source_tick(int i) {
    // A common container/decoder clock: 23.976 fps rounded to milliseconds.
    return ((int64_t(i)*source_den*1000+source_num/2)/source_num)*10000;
}
int64_t output_tick(int i,int multiple=2) {return int64_t(i)*units*source_den/(source_num*multiple);}
uint32_t hash(uint32_t v) {
    v^=v>>16;v*=0x7feb352dU;v^=v>>15;v*=0x846ca68bU;return v^(v>>16);
}
double texture(double x,double y,uint32_t seed) {
    x=(x+4096)/8;y=(y+4096)/8;
    const int gx=int(std::floor(x)),gy=int(std::floor(y));
    const double fx=x-gx,fy=y-gy;
    const auto cell=[seed](int a,int b){return double(hash(uint32_t(a)*733U+uint32_t(b)*19349663U+seed)%191U);};
    return (1-fx)*(1-fy)*cell(gx,gy)+fx*(1-fy)*cell(gx+1,gy)+(1-fx)*fy*cell(gx,gy+1)+fx*fy*cell(gx+1,gy+1);
}
enum class Scene {pan,foreground};
const char* name(Scene scene){return scene==Scene::pan?"pan":"foreground";}
double left(double shift){return 32+shift;}
double object_width(){return width/4.0;}
double object_top(){return height/3.0;}
double object_bottom(){return height*2.0/3.0;}
double coverage(double x,double y,double shift) {
    const auto clamp=[](double v){return std::max(0.0,std::min(1.0,v));};
    // Analytic pixel edge coverage supplies a continuous subpixel oracle.
    return clamp(x-left(shift)+.5)*clamp(left(shift)+object_width()-x+.5)*
           clamp(y-object_top()+.5)*clamp(object_bottom()-y+.5);
}
uint8_t luma(Scene scene,double x,double y,double shift) {
    if(scene==Scene::pan)return uint8_t(std::lround(32+texture(x-shift,y,0)));
    const double a=coverage(x,y,shift);
    return uint8_t(std::lround((24+.52*texture(x,y,9147))*(1-a)+(104+.60*texture(x-shift,y,731))*a));
}
nvof::Frame frame(Scene scene,double shift,int64_t pts) {
    nvof::Frame result{width,height,pts,std::vector<uint8_t>(size_t(width)*height*3/2)};
    for(int y=0;y<height;++y)for(int x=0;x<width;++x)result.pixels[size_t(y)*width+x]=luma(scene,x,y,shift);
    for(int y=0;y<height;y+=2)for(int x=0;x<width;x+=2) {
        const size_t at=size_t(width)*height+size_t(y/2)*width+x;
        const double a=scene==Scene::pan?1.0:coverage(x+.5,y+.5,shift),moving=texture(x-shift,y,921);
        result.pixels[at]=uint8_t(std::lround(128+a*(moving/5-19)));
        result.pixels[at+1]=uint8_t(std::lround(128-a*(moving/5-19)));
    }
    return result;
}
double mse(const nvof::Frame& actual,const nvof::Frame& expected,bool background=false) {
    double sum=0;uint64_t count=0;
    for(int y=16;y<height-16;++y)for(int x=16;x<width-16;++x) {
        // Fixed strip never intersects moving foreground or occlusion edges.
        if(background&&y>=height/4)continue;
        const double delta=int(actual.pixels[size_t(y)*width+x])-int(expected.pixels[size_t(y)*width+x]);
        sum+=delta*delta;++count;
    }
    return count?sum/count:0;
}
double estimate_shift(const nvof::Frame& actual,Scene scene,double expected) {
    double best=expected,best_error=std::numeric_limits<double>::infinity();
    // Include preceding/following source positions to detect stale/reversed data.
    for(int candidate=-32;candidate<=32;++candidate) {
        const double shift=expected+candidate*.25;
        double error=0;int count=0,x0=32,x1=width-32,y0=32,y1=height-32;
        if(scene==Scene::foreground) {
            x0=std::max(16,int(left(expected))+12);x1=std::min(width-16,int(left(expected)+object_width())-12);
            y0=int(object_top())+12;y1=int(object_bottom())-12;
        }
        for(int y=y0;y<y1;y+=5)for(int x=x0;x<x1;x+=5) {
            const double delta=int(actual.pixels[size_t(y)*width+x])-int(luma(scene,x,y,shift));
            error+=delta*delta;++count;
        }
        if(count&&error/count<best_error){best_error=error/count;best=shift;}
    }
    return best;
}
struct Measurements {
    int frames=0,endpoints=0,repeated=0,backwards=0,position_errors=0,printed=0;
    double sum_mse=0,max_mse=0,max_endpoint_mse=0,max_background_mse=0;
    double max_position_error=0,max_step=0,previous_position=0;
    bool have_previous=false;
    void add(Scene scene,const nvof::Frame& actual,double expected,double phase,bool repeated_frame,
             const nvof::Frame& original,const char* path) {
        const auto truth=frame(scene,expected,actual.pts);
        const double error=mse(actual,truth),position=estimate_shift(actual,scene,expected);
        const double endpoint_error=mse(actual,original),position_error=position-expected;
        const bool endpoint=phase<.025||phase>.975;
        ++frames;endpoints+=endpoint;repeated+=repeated_frame;sum_mse+=error;max_mse=std::max(max_mse,error);
        if(endpoint)max_endpoint_mse=std::max(max_endpoint_mse,endpoint_error);
        if(scene==Scene::foreground)max_background_mse=std::max(max_background_mse,mse(actual,truth,true));
        max_position_error=std::max(max_position_error,std::abs(position_error));
        if(have_previous){backwards+=position<previous_position-.5;max_step=std::max(max_step,std::abs(position-previous_position));}
        have_previous=true;previous_position=position;position_errors+=std::abs(position_error)>1;
        // Measure every output, including late continuous pairs; bound verbosity.
        if((std::abs(position_error)>1||(endpoint&&endpoint_error>4))&&printed++<16)
            std::cout<<"SAMPLE path="<<path<<" scene="<<name(scene)<<" pts="<<actual.pts<<" phase="<<phase
                     <<" expected_x="<<expected<<" inferred_x="<<position<<" mse="<<error
                     <<" endpoint_mse="<<endpoint_error<<" repeated="<<repeated_frame<<'\n';
    }
    void report(const char* path,Scene scene)const {
        std::cout<<"RESULT path="<<path<<" scene="<<name(scene)<<" frames="<<frames<<" endpoints="<<endpoints
                 <<" repeated="<<repeated<<" backward_steps="<<backwards<<" position_errors_over_1px="<<position_errors
                 <<" mean_mse="<<(frames?sum_mse/frames:0)<<" max_mse="<<max_mse<<" max_endpoint_mse="<<max_endpoint_mse
                 <<" max_static_background_mse="<<max_background_mse<<" max_position_error_px="<<max_position_error
                 <<" max_step_px="<<max_step<<'\n';
    }
};
void scheduler_check(int pairs,int multiple) {
    std::vector<nvof::Frame> originals;
    for(int i=0;i<=pairs;++i) {
        nvof::Frame source{16,8,source_tick(i),std::vector<uint8_t>(16*8*3/2,64)};
        source.pixels[0]=uint8_t(i);source.pixels[1]=uint8_t(i>>8);originals.push_back(std::move(source));
    }
    int requests=0,endpoints=0,outputs=0,exact_originals=0,wrong_originals=0;
    nvof::PhasePipeline pipeline({source_num*multiple,source_den},[&](const auto& a,const auto& b,const auto& times){
        nvof::PhaseBatch<nvof::Frame> batch;batch.quality.repetition_known=true;
        for(auto time:times) {
            const double phase=double(time-a.pts)/(b.pts-a.pts);endpoints+=phase<.025||phase>.975;++requests;
            auto generated=a;generated.pts=time;generated.pixels[2]=255;batch.frames.push_back(std::move(generated));
        }
        return batch;
    },{source_num,source_den});
    const auto emit=[&](const nvof::OutputFrame& output) {
        require(output.frame.pts==output_tick(outputs,multiple),"Pipeline output clock changed");
        if(outputs<=pairs*multiple&&outputs%multiple==0) {
            if(output.frame.pixels==originals[size_t(outputs/multiple)].pixels)++exact_originals;else ++wrong_originals;
        }
        ++outputs;return true;
    };
    for(const auto& source:originals)pipeline.push(source,false,emit);
    pipeline.finish(output_tick(2),emit);
    std::cout<<"SCHEDULER multiple="<<multiple<<" pairs="<<pairs<<" outputs="<<outputs<<" motion_requests="<<requests
             <<" near_endpoint_requests="<<endpoints<<" exact_originals="<<exact_originals<<" wrong_originals="<<wrong_originals<<'\n';
    require(requests==pairs*(multiple-1),"Rounded timestamps requested an incorrect number of genuine motion phases");
    require(endpoints==0,"Rounded timestamps sent original-adjacent phases to FRUC");
    require(exact_originals==pairs+1&&wrong_originals==0,"An original-clock output was not byte-exact");
}
void raw_rounded_clock(nvof::FrucEngine& engine,Scene scene,int pairs) {
    engine.reset();Measurements measurements;
    auto previous=frame(scene,0,source_tick(0));int next_output=1;
    for(int i=1;i<=pairs;++i) {
        auto current=frame(scene,i*displacement,source_tick(i));std::vector<int64_t> times;
        while(output_tick(next_output)<current.pts)times.push_back(output_tick(next_output++));
        auto batch=engine.interpolate_pair(previous,current,times);
        require(!batch.quality.scene_cut,"Continuous diagnostic unexpectedly classified as scene cut");
        require(batch.frames.size()==times.size(),"Raw engine returned incomplete output batch");
        for(size_t p=0;p<times.size();++p) {
            const double phase=double(times[p]-previous.pts)/(current.pts-previous.pts);
            measurements.add(scene,batch.frames[p],(i-1+phase)*displacement,phase,
                             (batch.quality.repeated_mask&(uint32_t(1)<<p))!=0,phase<.5?previous:current,"raw-rounded");
        }
        if(output_tick(next_output)==current.pts)++next_output;
        previous=std::move(current);
    }
    measurements.report("raw-rounded",scene);
}
void actual_pipeline(nvof::FrucEngine& engine,Scene scene,int pairs) {
    engine.reset();Measurements measurements;
    int output_index=0,exact_originals=0,wrong_originals=0,endpoints=0,requests=0;
    nvof::PhasePipeline pipeline({48000,1001},[&](const auto& a,const auto& b,const auto& times){
        for(auto time:times){const double phase=double(time-a.pts)/(b.pts-a.pts);endpoints+=phase<.025||phase>.975;++requests;}
        return engine.interpolate_pair(a,b,times);
    },{source_num,source_den});
    const auto emit=[&](const nvof::OutputFrame& output) {
        require(output.frame.pts==output_tick(output_index),"Hardware pipeline output clock changed");
        if(output_index<=pairs*2) {
            const double expected=output_index*displacement/2;
            const int source_index=(output_index+1)/2;
            const auto original=frame(scene,source_index*displacement,source_tick(source_index));
            if((output_index&1)==0){if(output.frame.pixels==original.pixels)++exact_originals;else ++wrong_originals;}
            measurements.add(scene,output.frame,expected,(output_index&1)?.5:0,false,original,"pipeline");
        }
        ++output_index;return true;
    };
    for(int i=0;i<=pairs;++i)pipeline.push(frame(scene,i*displacement,source_tick(i)),false,emit);
    pipeline.finish(output_tick(2),emit);
    measurements.report("pipeline",scene);
    std::cout<<"PRESERVATION scene="<<name(scene)<<" originals="<<exact_originals<<" wrong_originals="<<wrong_originals
             <<" motion_requests="<<requests<<" near_endpoint_requests="<<endpoints<<'\n';
    require(requests==pairs&&endpoints==0,"Hardware pipeline generated endpoint phases at x2");
    require(exact_originals==pairs+1&&wrong_originals==0,"Hardware pipeline changed an original-clock frame");
    // Pan has a unique translation everywhere. Occlusion-scene quality remains
    // diagnostic rather than imposing an arbitrary image-error threshold.
    if(scene==Scene::pan) {
        require(measurements.backwards==0,"Continuous pan moved backward after endpoint normalization");
        require(measurements.max_position_error<=1,"Continuous pan phase is more than one pixel out of position");
    }
}
}
int wmain(int argc,wchar_t** argv) {
    try {
        if(argc<2) {
            std::cerr<<"phase_endpoint_smoke --schedule-only [pairs]\n"
                     <<"phase_endpoint_smoke <runtime> [pairs=48] [width=640 height=360]\n";return 2;
        }
        const int pairs=argc>2?std::stoi(argv[2]):48;
        require(pairs>=8&&pairs<=10000,"pairs must be between 8 and 10000");
        std::cout<<std::fixed<<std::setprecision(6);
        if(std::wstring(argv[1])==L"--schedule-only") {
            scheduler_check(pairs,2);scheduler_check(pairs,5);
            std::cout<<"PASS rounded 23.976-to-47.952/119.880 cadence and byte-exact originals (no GPU used)\n";return 0;
        }
        if(argc>3)width=std::stoi(argv[3]);if(argc>4)height=std::stoi(argv[4]);
        require(width>=320&&height>=180&&!(width&1)&&!(height&1),"Use even dimensions at least 320x180");
        require(left(pairs*displacement)+object_width()+16<width,"Too many pairs for continuous foreground motion; increase width");
        nvof::FrucEngine engine{std::filesystem::path(argv[1])};
        std::cout<<"DEVICE "<<engine.device_name()<<" pairs="<<pairs<<" size="<<width<<'x'<<height<<'\n';
        // Gather former raw-schedule evidence before asserting the corrected
        // pipeline, so a failing regression still leaves useful diagnostics.
        for(Scene scene:{Scene::pan,Scene::foreground})raw_rounded_clock(engine,scene,pairs);
        scheduler_check(pairs,2);scheduler_check(pairs,5);
        for(Scene scene:{Scene::pan,Scene::foreground})actual_pipeline(engine,scene,pairs);
        std::cout<<"PASS continuous rounded-clock endpoint regression; compare RESULT raw/pipeline quality\n";return 0;
    } catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
}
