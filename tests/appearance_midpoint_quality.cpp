// Independent discrete-drawing oracle: preserve a source expression while
// moving its rigid face halfway. FRUC must not invent torn open/closed shapes.
#define NVOF_OCCLUSION_HELPERS_ONLY
#include "fractional_occlusion_quality.cpp"
#undef NVOF_OCCLUSION_HELPERS_ONLY
namespace {
nvof::Frame expression(double shift,bool open,double radius,int64_t pts) {
    nvof::Frame f{width,height,pts,std::vector<uint8_t>(size_t(width)*height*3/2,128)};
    for(int y=0;y<height;++y)for(int x=0;x<width;++x){double value=0;
        for(int sy=0;sy<4;++sy)for(int sx=0;sx<4;++sx){
            double px=x+(sx+.5)/4-shift,py=y+(sy+.5)/4;
            double v=45+.35*detailed_texture(px,py,19);
            if(px>175&&px<465&&py>35&&py<330){v=195;
                if(px<177||px>463||py<37||py>328)v=28;
                if(line_distance(px,py,220,110,267,107)<1||line_distance(px,py,373,107,420,110)<1)v=30;
                double xx=(px-320)/radius,yy=(py-231)/(radius*.72);
                if(open&&xx*xx+yy*yy<1)v=py>238?115:60;
                if(!open&&line_distance(px,py,320-radius,215,320+radius,217)<.8)v=35;
            }value+=v;
        }f.pixels[size_t(y)*width+x]=uint8_t(std::lround(value/16));
    }return f;
}
double mouth_error(const nvof::Frame& f,const nvof::Frame& expected,double shift,double radius) {
 double e=0;int n=0;
 for(int y=int(231-radius*.72)-5;y<int(231+radius*.72)+5;++y)for(int x=int(320+shift-radius)-5;x<int(320+shift+radius)+5;++x){int d=int(f.pixels[y*width+x])-expected.pixels[y*width+x];e+=d*d;++n;}
 return e/n;
}
}
int wmain(int argc,wchar_t**argv){try{
 require(argc==2,"appearance_midpoint_quality <runtime>");TestGpu gpu;int cases=0;std::vector<std::string> failures;
 for(double radius:{48.,24.,12.})for(double speed:{0.,1.,3.})for(bool closing:{false,true}){
  nvof::FrucEngine engine{std::filesystem::path(argv[1])};
  nvof::FractionalRefiner old(gpu.device.Get(),gpu.context.Get(),false),fix(gpu.device.Get(),gpu.context.Get(),true);
  auto a=expression(0,closing,radius,0),b=expression(speed,!closing,radius,1000000),truth=expression(speed*.5,closing,radius,500000);
  auto batch=engine.interpolate_pair(a,b,{500000});require(batch.frames.size()==1,"Missing midpoint");
  auto evaluate=[&](nvof::FractionalRefiner& refiner){auto ga=gpu.texture(&a),gb=gpu.texture(&b),f=gpu.texture(&batch.frames[0]),out=gpu.texture();auto fy=gpu.srv(f.Get(),false),fu=gpu.srv(f.Get(),true);auto oy=gpu.rtv(out.Get(),false),ou=gpu.rtv(out.Get(),true);refiner.prepare(ga.Get(),gb.Get(),width,height);refiner.render_midpoint(oy.Get(),ou.Get(),fy.Get(),fu.Get());auto result=gpu.readback(out.Get(),500000);require(gpu.readback(ga.Get(),0).pixels==a.pixels&&gpu.readback(gb.Get(),1000000).pixels==b.pixels,"Source changed");return result;};
  auto before=evaluate(old),after=evaluate(fix);double e0=mouth_error(before,truth,speed*.5,radius),e1=mouth_error(after,truth,speed*.5,radius);
  std::cout<<"EXPRESSION radius="<<radius<<" speed="<<speed<<" closing="<<closing<<" before="<<e0<<" after="<<e1<<std::endl;
  if(speed==0&&e1>e0*.35+3)failures.push_back("Still-view discrete expression not sufficiently protected");
  if(speed>0&&after.pixels!=before.pixels)failures.push_back("Appearance protection changed a camera pan");
  ++cases;
 }
 for(auto& f:failures)std::cout<<"QUALITY_FAILURE "<<f<<std::endl;
 require(failures.empty(),"Appearance protection regression");std::cout<<"PASS appearance cases="<<cases<<" sources unchanged\n";return 0;
}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
