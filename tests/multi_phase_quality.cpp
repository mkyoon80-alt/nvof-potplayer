#define wmain endpoint_diagnostic_entry
#include "phase_endpoint_smoke.cpp"
#undef wmain
#include <array>
#include <map>
#include <memory>

// Explicit hardware investigation, not an automatic CTest or throughput test.
// Shared = production FrucEngine's independent sessions sharing GPU allocations.
// Isolated = one separately allocated FrucEngine/context for each phase index.
namespace {
double detailed_texture(double x,double y,uint32_t seed) {
    return .70*texture(x,y,seed)+.30*texture(x*4,y*4,seed+73);
}
uint8_t detailed_luma(Scene scene,double x,double y,double shift) {
    if(scene==Scene::pan)return uint8_t(std::lround(32+detailed_texture(x-shift,y,0)));
    const double a=coverage(x,y,shift);
    return uint8_t(std::lround((24+.52*detailed_texture(x,y,9147))*(1-a)+(104+.60*detailed_texture(x-shift,y,731))*a));
}
nvof::Frame detailed_frame(Scene scene,double shift,int64_t pts) {
    auto f=frame(scene,shift,pts);
    for(int y=0;y<height;++y)for(int x=0;x<width;++x)f.pixels[size_t(y)*width+x]=detailed_luma(scene,x,y,shift);
    return f;
}
double detailed_position(const nvof::Frame& actual,Scene scene,double expected,double speed) {
    double best=expected,best_error=std::numeric_limits<double>::infinity();
    const int radius=int(std::ceil((speed+2)*8));
    for(int candidate=-radius;candidate<=radius;++candidate) {
        const double shift=std::round(expected*8)/8+candidate*.125;
        double error=0;int count=0,x0=32,x1=width-32,y0=32,y1=height-32;
        if(scene==Scene::foreground) {
            x0=std::max(16,int(left(expected))+12);x1=std::min(width-16,int(left(expected)+object_width())-12);
            y0=int(object_top())+12;y1=int(object_bottom())-12;
        }
        for(int y=y0;y<y1;y+=7)for(int x=x0;x<x1;x+=7) {
            const double delta=int(actual.pixels[size_t(y)*width+x])-int(detailed_luma(scene,x,y,shift));
            error+=delta*delta;++count;
        }
        if(count&&error/count<best_error){best_error=error/count;best=shift;}
    }
    return best;
}
uint64_t digest(const nvof::Frame& f) {
    uint64_t result=1469598103934665603ULL;
    for(auto byte:f.pixels){result^=byte;result*=1099511628211ULL;}return result;
}
double temporal_error(const nvof::Frame& a,const nvof::Frame& previous,
                      const nvof::Frame& truth,const nvof::Frame& previous_truth) {
    double sum=0;int count=0;
    for(int y=16;y<height-16;++y)for(int x=16;x<width-16;++x) {
        const auto k=size_t(y)*width+x;
        const int residual=int(a.pixels[k])-int(previous.pixels[k])-int(truth.pixels[k])+int(previous_truth.pixels[k]);
        sum+=residual*residual;++count;
    }
    return sum/count;
}
double detail_rms(const nvof::Frame& f) {
    double sum=0;int count=0;
    for(int y=16;y<height-16;++y)for(int x=16;x<width-16;++x) {
        const auto k=size_t(y)*width+x;
        const int d=4*int(f.pixels[k])-int(f.pixels[k-1])-int(f.pixels[k+1])-int(f.pixels[k-width])-int(f.pixels[k+width]);
        sum+=d*d;++count;
    }
    return std::sqrt(sum/count);
}
double uv_mse(const nvof::Frame& a,const nvof::Frame& b) {
    double sum=0;int count=0;
    for(int y=8;y<height/2-8;++y)for(int x=16;x<width-16;++x) {
        const auto k=size_t(width)*height+size_t(y)*width+x;
        const int d=int(a.pixels[k])-int(b.pixels[k]);sum+=d*d;++count;
    }
    return sum/count;
}
struct DiagnosticStats {
    int frames=0,motion=0,held=0,backwards=0,stalled_motion_steps=0,duplicates=0,position_errors=0;
    double sum_mse=0,max_mse=0,sum_temporal=0,max_temporal=0,max_background=0,max_position_error=0,max_step=0;
    nvof::Frame previous,previous_truth;
    double previous_position=0,previous_expected=0;
    void add(const std::string& label,Scene scene,double speed,const nvof::Frame& actual,
             double expected,double phase,const char* kind,bool hold,uint32_t repeated_mask) {
        const auto truth=detailed_frame(scene,expected,actual.pts);
        const double error=mse(actual,truth),position=detailed_position(actual,scene,expected,speed);
        const double background=scene==Scene::foreground?mse(actual,truth,true):0;
        const double step=frames?position-previous_position:0;
        const double step_mse=frames?mse(actual,previous):0;
        const double truth_step_mse=frames?mse(truth,previous_truth):0;
        const double delta_error=frames?temporal_error(actual,previous,truth,previous_truth):0;
        const bool duplicate=frames&&actual.pixels==previous.pixels;
        const bool backward=frames&&step<-.0625;
        ++frames;motion+=std::string(kind)=="motion";held+=hold;duplicates+=duplicate;backwards+=backward;
        position_errors+=std::abs(position-expected)>1;
        stalled_motion_steps+=frames>1&&expected-previous_expected>.1&&std::abs(step)<.0625;
        sum_mse+=error;max_mse=std::max(max_mse,error);sum_temporal+=delta_error;max_temporal=std::max(max_temporal,delta_error);
        max_background=std::max(max_background,background);max_position_error=std::max(max_position_error,std::abs(position-expected));
        max_step=std::max(max_step,std::abs(step));
        std::cout<<"FRAME case="<<label<<" n="<<(frames-1)<<" pts="<<actual.pts<<" kind="<<kind<<" phase="<<phase
          <<" expected_x="<<expected<<" inferred_x="<<position<<" step_px="<<step<<" mse="<<error
          <<" step_mse="<<step_mse<<" ideal_step_mse="<<truth_step_mse<<" temporal_error="<<delta_error
          <<" static_background_mse="<<background<<" detail_rms="<<detail_rms(actual)<<" ideal_detail_rms="<<detail_rms(truth)<<" backwards="<<backward<<" duplicate="<<duplicate
          <<" held="<<hold<<" repeated_mask="<<repeated_mask
          <<" cpu_storage="<<reinterpret_cast<uintptr_t>(actual.pixels.data())<<" nv12_digest="<<digest(actual)<<'\n';
        previous=actual;previous_truth=truth;previous_position=position;previous_expected=expected;
    }
    void report(const std::string& label)const {
        std::cout<<"RESULT case="<<label<<" frames="<<frames<<" motion_frames="<<motion<<" held_frames="<<held
          <<" backward_steps="<<backwards<<" stalled_motion_steps="<<stalled_motion_steps<<" duplicates="<<duplicates<<" position_errors_over_1px="<<position_errors
          <<" mean_mse="<<sum_mse/std::max(1,frames)<<" max_mse="<<max_mse
          <<" mean_temporal_error="<<sum_temporal/std::max(1,frames-1)<<" max_temporal_error="<<max_temporal
          <<" max_static_background_mse="<<max_background<<" max_position_error_px="<<max_position_error<<" max_step_px="<<max_step<<'\n';
    }
};
struct PhaseRecord {bool hold=false;uint32_t mask=0;};
void multi_phase_case(const std::filesystem::path& runtime,nvof::Rate rate,Scene scene,double speed,bool anime,int pairs) {
    const std::string label=std::to_string(rate.num)+"_"+name(scene)+"_"+std::to_string(int(speed))+"px_"+(anime?"AABB":"continuous");
    nvof::FrucEngine shared(runtime);
    std::vector<std::unique_ptr<nvof::FrucEngine>> isolated;
    std::map<int64_t,PhaseRecord> phases;
    DiagnosticStats output_stats,raw_stats,isolated_stats;
    int pair=0,output_index=0,originals=0,wrong_originals=0,shared_repeated=0,isolated_repeated=0,different_frames=0,near_endpoints=0;
    double sum_shared_isolated_mse=0,max_shared_isolated_mse=0;
    const auto shift=[=](int index){return (anime?index/2:index)*speed;};
    const auto exact_tick=[](int index){return int64_t(index)*units*source_den/source_num;};
    const auto target_tick=[=](int index){return int64_t(index)*units*rate.den/rate.num;};
    nvof::PhasePipeline pipeline(rate,[&](const auto& a,const auto& b,const auto& times){
        ++pair;
        auto result=shared.interpolate_pair(a,b,times);
        require(!result.quality.scene_cut,"Diagnostic motion was classified as scene cut");
        require(result.frames.size()==times.size(),"Production engine returned incomplete batch");
        require(times.empty()||result.quality.repetition_known,"Production engine omitted real repetition metadata");
        while(isolated.size()<times.size())isolated.push_back(std::make_unique<nvof::FrucEngine>(runtime));
        uint32_t independent_mask=0;
        for(size_t p=0;p<isolated.size();++p) {
            const auto independent=isolated[p]->interpolate_pair(a,b,p<times.size()?std::vector<int64_t>{times[p]}:std::vector<int64_t>{});
            if(p>=times.size())continue;
            require(!independent.quality.scene_cut&&independent.frames.size()==1&&independent.quality.repetition_known,"Independent engine returned incomplete phase or quality metadata");
            const double phase=double(times[p]-a.pts)/(b.pts-a.pts);
            const double expected=shift(pair-1)+(shift(pair)-shift(pair-1))*phase;
            const double difference=mse(result.frames[p],independent.frames[0]);
            near_endpoints+=phase<.025||phase>.975;
            different_frames+=result.frames[p].pixels!=independent.frames[0].pixels;
            sum_shared_isolated_mse+=difference;max_shared_isolated_mse=std::max(max_shared_isolated_mse,difference);
            if(independent.quality.repeated_mask)independent_mask|=uint32_t(1)<<p;
            raw_stats.add(label+"_raw",scene,speed,result.frames[p],expected,phase,"motion",false,result.quality.repeated_mask);
            isolated_stats.add(label+"_isolated",scene,speed,independent.frames[0],expected,phase,"motion",false,independent.quality.repeated_mask);
            std::cout<<"COMPARE case="<<label<<" pair="<<pair<<" index="<<p<<" phase="<<phase
              <<" shared_vs_isolated_mse="<<difference<<" shared_vs_isolated_uv_mse="<<uv_mse(result.frames[p],independent.frames[0])<<" shared_digest="<<digest(result.frames[p])
              <<" isolated_digest="<<digest(independent.frames[0])<<" shared_mask="<<result.quality.repeated_mask
              <<" isolated_mask="<<independent.quality.repeated_mask
              <<" input_a_storage="<<reinterpret_cast<uintptr_t>(a.pixels.data())
              <<" input_b_storage="<<reinterpret_cast<uintptr_t>(b.pixels.data())<<'\n';
        }
        shared_repeated+=result.quality.repeated_mask!=0;isolated_repeated+=independent_mask!=0;
        for(auto pts:times)phases[pts]={result.quality.repeated_mask!=0,result.quality.repeated_mask};
        return result;
    },{source_num,source_den});
    const auto emit=[&](const nvof::OutputFrame& output){
        require(output.frame.pts==target_tick(output_index),"Output timeline drifted");
        const double source_position=double(output_index)*source_num*rate.den/(double(source_den)*rate.num);
        const int index=std::min(pairs,int(std::floor(source_position+1e-8)));
        const double phase=index>=pairs?0:source_position-index;
        const double expected=shift(index)+(index<pairs?(shift(index+1)-shift(index))*phase:0);
        const auto found=phases.find(output.frame.pts);
        const bool motion=found!=phases.end(),terminal=output.frame.pts>exact_tick(pairs);
        if(!motion&&!terminal){
            ++originals;wrong_originals+=output.frame.pixels!=detailed_frame(scene,shift(index),output.frame.pts).pixels;
        }
        output_stats.add(label,scene,speed,output.frame,expected,phase,motion?"motion":terminal?"terminal":"source",
          motion&&found->second.hold,motion?found->second.mask:0);
        ++output_index;return true;
    };
    std::cout<<"CASE case="<<label<<" device="<<shared.device_name()<<" pairs="<<pairs<<" size="<<width<<'x'<<height<<'\n';
    for(int i=0;i<=pairs;++i)pipeline.push(detailed_frame(scene,shift(i),source_tick(i)),false,emit);
    pipeline.finish(exact_tick(1),emit);
    output_stats.report(label);raw_stats.report(label+"_raw");isolated_stats.report(label+"_isolated");
    std::cout<<"COMPARISON case="<<label<<" phases="<<raw_stats.frames<<" different_nv12_frames="<<different_frames
      <<" mean_shared_vs_isolated_mse="<<sum_shared_isolated_mse/std::max(1,raw_stats.frames)
      <<" max_shared_vs_isolated_mse="<<max_shared_isolated_mse<<" shared_repeated_pairs="<<shared_repeated
      <<" isolated_repeated_pairs="<<isolated_repeated<<" exact_originals="<<originals
      <<" wrong_originals="<<wrong_originals<<" near_endpoint_requests="<<near_endpoints<<'\n';
    const int expected_outputs=int((int64_t(pairs+1)*rate.num+source_num-1)/source_num);
    int expected_originals=0;
    for(int i=0;i<=pairs;++i)expected_originals+=(int64_t(i)*rate.num)%source_num==0;
    require(pair==pairs,"Production callback skipped a source pair");
    require(output_index==expected_outputs,"Continuous output frame count changed");
    require(originals==expected_originals,"An aligned source frame was omitted");
    require(wrong_originals==0,"A source-clock output changed source pixels");
    require(near_endpoints==0,"Normalized cadence still requested near-endpoint phases");
}
}
#ifndef NVOF_MULTI_PHASE_HELPERS_ONLY
int wmain(int argc,wchar_t** argv) {
    try {
        if(argc<2){std::cerr<<"multi_phase_quality <runtime> [pairs=12] [rate=0,48000,60000,120000] [scene=all,pan,foreground] [speed=0/all,4,10] [cadence=all,continuous,AABB]\n";return 2;}
        const int pairs=argc>2?std::stoi(argv[2]):12;
        const int chosen_rate=argc>3?std::stoi(argv[3]):0;
        const std::wstring chosen_scene=argc>4?argv[4]:L"all";
        const int chosen_speed=argc>5?std::stoi(argv[5]):0;
        const std::wstring chosen_cadence=argc>6?argv[6]:L"all";
        require(pairs>=4&&pairs<=36,"Use 4 to 36 pairs at 640x360");
        require(chosen_rate==0||chosen_rate==48000||chosen_rate==60000||chosen_rate==120000,"Unsupported diagnostic rate");
        require(chosen_scene==L"all"||chosen_scene==L"pan"||chosen_scene==L"foreground","Unsupported diagnostic scene");
        require(chosen_speed==0||chosen_speed==4||chosen_speed==10,"Unsupported diagnostic speed");
        require(chosen_cadence==L"all"||chosen_cadence==L"continuous"||chosen_cadence==L"AABB","Unsupported diagnostic cadence");
        std::cout<<std::fixed<<std::setprecision(6);
        for(int rate:{48000,60000,120000})for(Scene scene:{Scene::pan,Scene::foreground})for(int speed:{4,10})for(bool anime:{false,true}) {
            if(chosen_rate&&chosen_rate!=rate)continue;
            if(chosen_scene!=L"all"&&chosen_scene!=(scene==Scene::pan?L"pan":L"foreground"))continue;
            if(chosen_speed&&chosen_speed!=speed)continue;
            if(chosen_cadence!=L"all"&&chosen_cadence!=(anime?L"AABB":L"continuous"))continue;
            multi_phase_case(std::filesystem::path(argv[1]),{rate,1001},scene,speed,anime,pairs);
        }
        std::cout<<"PASS structural checks; RESULT/FRAME/COMPARE contain measured NVIDIA output (quality is diagnostic)\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
#endif
