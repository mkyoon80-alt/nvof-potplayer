#define NVOF_REFINER_HELPERS_ONLY
#include "fractional_refiner_quality.cpp"
#undef NVOF_REFINER_HELPERS_ONLY
#include <fstream>

namespace {
enum class Adversary { crossing, pan_object, wire, fence, crossing_bright, wire_bright };
const char* adversary_name(Adversary s){switch(s){case Adversary::crossing:return "crossing";case Adversary::pan_object:return "pan_object";case Adversary::wire:return "wire";case Adversary::fence:return "fence";case Adversary::crossing_bright:return "crossing_bright";default:return "wire_bright";}}
struct Surface {double y;int layer;};
double line_distance(double x,double y,double ax,double ay,double bx,double by){const double dx=bx-ax,dy=by-ay;const double t=std::max(0.,std::min(1.,((x-ax)*dx+(y-ay)*dy)/(dx*dx+dy*dy)));return std::hypot(x-ax-t*dx,y-ay-t*dy);}
Surface cartoon(double x,double y,double ox,double oy,int id,bool fence) {
    const double px=x-ox,py=y-oy;const double w=128,h=156;
    if(px<0||px>=w||py<0||py>=h)return {-1,-1};
    double value=id==1?192:180;
    if(px<1.2||py<1.2||px>w-1.2||py>h-1.2)value=20;
    // Thin diagonal hair, eyes and mouth on otherwise flat, similar colors.
    if(line_distance(px,py,6,28,66,8)<.7||line_distance(px,py,56,8,118,34)<.7||
       line_distance(px,py,24,66,52,65)<.6||line_distance(px,py,78,66,106,65)<.6||
       line_distance(px,py,36,114,96,116)<.65)value=20;
    if(fence&&std::fmod(px,8.)<1.)value=20;
    return {value,id};
}
Surface surface(Adversary scene,double x,double y,double time,double speed) {
    const bool bright=scene==Adversary::crossing_bright||scene==Adversary::wire_bright;
    const double illumination=bright?time*8:0;
    Surface p{70,0};
    if(scene==Adversary::pan_object)p.y=40+.6*detailed_texture(x+time*3,y,274);
    if(scene==Adversary::wire||scene==Adversary::wire_bright) {
        p.y=190;
        // A one-pixel wire moves far enough that a middle pixel is unchanged in A/B.
        const double wx=96+time*speed;
        if(y>36&&y<height-36&&std::abs(x-wx)<.5)p={20,1};
        // A sparse, finer diagonal remains a different motion component.
        if(line_distance(x,y,430-time*speed*.6,44,540-time*speed*.6,304)<.45)p={36,2};
    } else {
        auto a=cartoon(x,y,132+time*speed,84,1,scene==Adversary::fence);
        auto b=cartoon(x,y,316-time*speed*.8,118,2,scene==Adversary::fence);
        if(a.layer>=0)p=a;if(b.layer>=0)p=b;
        if(scene==Adversary::pan_object) {
            auto c=cartoon(x,y,448-time*speed*.3,28,1,false);if(c.layer>=0)p=c;
        }
    }
    p.y=std::max(16.,std::min(235.,p.y+illumination));return p;
}
bool delicate_pixel(Adversary scene,double x,double y,double time,double speed) {
    if(scene==Adversary::wire||scene==Adversary::wire_bright)
        return std::abs(x-(96+time*speed))<1.6||line_distance(x,y,430-time*speed*.6,44,540-time*speed*.6,304)<1.5;
    const auto box=[&](double ox,double oy){double px=x-ox,py=y-oy;if(px< -2||px>130||py< -2||py>158)return false;
        if(std::min(std::abs(px),std::abs(px-128))<2.3||std::min(std::abs(py),std::abs(py-156))<2.3)return true;
        if(line_distance(px,py,6,28,66,8)<1.8||line_distance(px,py,56,8,118,34)<1.8||line_distance(px,py,24,66,52,65)<1.7||line_distance(px,py,78,66,106,65)<1.7||line_distance(px,py,36,114,96,116)<1.75)return true;
        if(scene==Adversary::fence){double m=std::fmod(px+1024,8.);if(m<2.2||m>6.8)return true;}
        return false;};
    return box(132+time*speed,84)||box(316-time*speed*.8,118)||(scene==Adversary::pan_object&&box(448-time*speed*.3,28));
}
nvof::Frame adversarial_frame(Adversary scene,double time,double speed,int64_t pts) {
    nvof::Frame f{width,height,pts,std::vector<uint8_t>(size_t(width)*height*3/2,128)};
    for(int y=0;y<height;++y)for(int x=0;x<width;++x){double v=0;const int samples=delicate_pixel(scene,x+.5,y+.5,time,speed)?16:2;for(int sy=0;sy<samples;++sy)for(int sx=0;sx<samples;++sx)v+=surface(scene,x+(sx+.5)/samples,y+(sy+.5)/samples,time,speed).y;f.pixels[size_t(y)*width+x]=uint8_t(std::lround(v/(samples*samples)));}
    return f;
}
void save_pgm(const std::filesystem::path& file,const nvof::Frame& f){std::ofstream s(file,std::ios::binary);s<<"P5\n"<<width<<' '<<height<<"\n255\n";s.write(reinterpret_cast<const char*>(f.pixels.data()),size_t(width)*height);}
struct LocalQuality {double baseline=0,refined=0,occlusion_baseline=0,occlusion_refined=0,worst_added=0;int worst_x=0,worst_y=0,regressed_tiles=0,missing_raw=0,missing_new=0,extra_raw=0,extra_new=0;double worst_raw=0,worst_new=0;};
LocalQuality local_quality(const nvof::Frame& baseline,const nvof::Frame& refined,const nvof::Frame& truth,Adversary scene,double time,double speed) {
    LocalQuality q;q.baseline=mse(baseline,truth);q.refined=mse(refined,truth);
    double sum0=0,sum1=0;int n=0;
    const double ta=std::floor(time),tb=ta+1;
    for(int y=16;y<height-16;++y)for(int x=16;x<width-16;++x){const auto k=size_t(y)*width+x;const auto l=surface(scene,x+.5,y+.5,time,speed).layer;
        const bool occ=l!=surface(scene,x+.5,y+.5,ta,speed).layer||l!=surface(scene,x+.5,y+.5,tb,speed).layer;
        if(occ){double a=int(baseline.pixels[k])-int(truth.pixels[k]),b=int(refined.pixels[k])-int(truth.pixels[k]);sum0+=a*a;sum1+=b*b;++n;}
        // Dark-outline errors: reference dark ink missing, or extra ink outside it.
        const bool ink=truth.pixels[k]<80;
        q.missing_raw+=ink&&baseline.pixels[k]>truth.pixels[k]+35;q.missing_new+=ink&&refined.pixels[k]>truth.pixels[k]+35;
        q.extra_raw+=!ink&&baseline.pixels[k]<truth.pixels[k]-35;q.extra_new+=!ink&&refined.pixels[k]<truth.pixels[k]-35;
    }
    q.occlusion_baseline=n?sum0/n:0;q.occlusion_refined=n?sum1/n:0;
    for(int y0=16;y0<height-32;y0+=16)for(int x0=16;x0<width-32;x0+=16){double a=0,b=0;for(int y=y0;y<y0+16;++y)for(int x=x0;x<x0+16;++x){const auto k=size_t(y)*width+x;const int da=int(baseline.pixels[k])-truth.pixels[k],db=int(refined.pixels[k])-truth.pixels[k];a+=da*da;b+=db*db;}a/=256;b/=256;
        q.regressed_tiles+=b>a*1.5+10;
        if(b-a>q.worst_added){q.worst_added=b-a;q.worst_raw=a;q.worst_new=b;q.worst_x=x0;q.worst_y=y0;}
    }return q;
}
void adversarial_case(TestGpu& gpu,const std::filesystem::path& runtime,Adversary scene,double speed,int pairs,const std::filesystem::path& captures) {
    nvof::FrucEngine engine(runtime);nvof::FractionalRefiner refiner(gpu.device.Get(),gpu.context.Get());
    auto previous=adversarial_frame(scene,0,speed,0);auto ga=gpu.texture(&previous);
    double raw_total=0,new_total=0,worst_added=0;int count=0,tiles=0,missing0=0,missing1=0,extra0=0,extra1=0;
    const std::string label=std::string(adversary_name(scene))+"_"+std::to_string(int(speed))+"px";
    for(int pair=1;pair<=pairs;++pair){auto current=adversarial_frame(scene,pair,speed,int64_t(pair)*1000000);auto gb=gpu.texture(&current);
        const std::vector<int64_t> times={previous.pts+200000,previous.pts+400000,previous.pts+500000,previous.pts+600000,previous.pts+800000};auto baseline=engine.interpolate_pair(previous,current,times);
        require(!baseline.quality.scene_cut&&baseline.frames.size()==5,"Missing actual FRUC adversarial baseline");refiner.prepare(ga.Get(),gb.Get(),width,height);
        for(size_t p=0;p<times.size();++p){double phase=double(times[p]-previous.pts)/1000000;auto fb=gpu.texture(&baseline.frames[p]);auto fy=gpu.srv(fb.Get(),false),fuv=gpu.srv(fb.Get(),true);auto out=gpu.texture();auto oy=gpu.rtv(out.Get(),false),ouv=gpu.rtv(out.Get(),true);refiner.render(float(phase),oy.Get(),ouv.Get(),fy.Get(),fuv.Get());auto actual=gpu.readback(out.Get(),times[p]);auto truth=adversarial_frame(scene,pair-1+phase,speed,times[p]);auto q=local_quality(baseline.frames[p],actual,truth,scene,pair-1+phase,speed);
            ++count;raw_total+=q.baseline;new_total+=q.refined;tiles+=q.regressed_tiles;missing0+=q.missing_raw;missing1+=q.missing_new;extra0+=q.extra_raw;extra1+=q.extra_new;
            std::cout<<"ADVERSARIAL case="<<label<<" pair="<<pair<<" phase="<<phase<<" repeated_mask="<<baseline.quality.repeated_mask<<" baseline_mse="<<q.baseline<<" refined_mse="<<q.refined<<" baseline_occlusion_mse="<<q.occlusion_baseline<<" refined_occlusion_mse="<<q.occlusion_refined<<" regressed_tiles="<<q.regressed_tiles<<" worst_added_mse="<<q.worst_added<<" worst_raw="<<q.worst_raw<<" worst_refined="<<q.worst_new<<" tile_x="<<q.worst_x<<" tile_y="<<q.worst_y<<" missing_ink_raw="<<q.missing_raw<<" missing_ink_refined="<<q.missing_new<<" extra_ink_raw="<<q.extra_raw<<" extra_ink_refined="<<q.extra_new<<'\n';
            if(q.worst_added>worst_added){worst_added=q.worst_added;if(!captures.empty()){std::filesystem::create_directories(captures);save_pgm(captures/(label+"-baseline.pgm"),baseline.frames[p]);save_pgm(captures/(label+"-refined.pgm"),actual);save_pgm(captures/(label+"-truth.pgm"),truth);save_pgm(captures/(label+"-a.pgm"),previous);save_pgm(captures/(label+"-b.pgm"),current);std::ofstream note(captures/(label+"-capture.txt"));note<<"pair="<<pair<<" phase="<<phase<<" x="<<q.worst_x<<" y="<<q.worst_y<<" raw="<<q.worst_raw<<" refined="<<q.worst_new<<'\n';}}
        }previous=std::move(current);ga=gb;
    }
    std::cout<<"ADVERSARIAL_RESULT case="<<label<<" frames="<<count<<" baseline_mean_mse="<<raw_total/count<<" refined_mean_mse="<<new_total/count<<" regressed_tiles="<<tiles<<" worst_tile_added_mse="<<worst_added<<" missing_ink_raw="<<missing0<<" missing_ink_refined="<<missing1<<" extra_ink_raw="<<extra0<<" extra_ink_refined="<<extra1<<'\n';
}
}
#ifndef NVOF_OCCLUSION_HELPERS_ONLY
int wmain(int argc,wchar_t** argv){try{if(argc<2){std::cerr<<"fractional_occlusion_quality <runtime> [pairs=6] [scene=all,crossing,pan_object,wire,fence,crossing_bright,wire_bright] [speed=12] [capture-directory]\n";return 2;}const int pairs=argc>2?std::stoi(argv[2]):6;const std::string wanted=argc>3?std::filesystem::path(argv[3]).string():"all";const double speed=argc>4?std::stod(argv[4]):12;const std::filesystem::path captures=argc>5?argv[5]:L"";require(pairs>=2&&pairs<=12,"pairs must be2..12");require(speed>=1&&speed<=32,"speed must be1..32");TestGpu gpu;std::cout<<std::fixed<<std::setprecision(6);int cases=0;for(auto scene:{Adversary::crossing,Adversary::pan_object,Adversary::wire,Adversary::fence,Adversary::crossing_bright,Adversary::wire_bright})if(wanted=="all"||wanted==adversary_name(scene)){adversarial_case(gpu,std::filesystem::path(argv[1]),scene,speed,pairs,captures);++cases;}require(cases>0,"Unknown scene");std::cout<<"DONE diagnostic only; positive per-tile regression is not suppressed by global averages\n";return 0;}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}


#endif
