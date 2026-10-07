#define NVOF_OCCLUSION_HELPERS_ONLY
#include "fractional_occlusion_quality.cpp"
#undef NVOF_OCCLUSION_HELPERS_ONLY
int wmain(int argc,wchar_t**argv){try{
 require(argc==2,"midpoint_stability_quality <runtime>");TestGpu gpu;std::vector<std::string> failures;
 auto evaluate=[&](const nvof::Frame&a,const nvof::Frame&b,const nvof::Frame&raw,nvof::FractionalRefiner&refiner){
 auto ga=gpu.texture(&a),gb=gpu.texture(&b),f=gpu.texture(&raw),out=gpu.texture();auto fy=gpu.srv(f.Get(),false),fu=gpu.srv(f.Get(),true);auto oy=gpu.rtv(out.Get(),false),ou=gpu.rtv(out.Get(),true);
 refiner.prepare(ga.Get(),gb.Get(),width,height);refiner.render_midpoint(oy.Get(),ou.Get(),fy.Get(),fu.Get());auto result=gpu.readback(out.Get(),raw.pts);
 auto evidence=refiner.diagnostic_motion_evidence();std::cout<<"MOTION_STATS";for(auto value:evidence)std::cout<<" "<<value;std::cout<<std::endl;
 require(gpu.readback(ga.Get(),a.pts).pixels==a.pixels&&gpu.readback(gb.Get(),b.pts).pixels==b.pixels,"Original modified");return result;
 };
 for(Scene scene:{Scene::pan,Scene::foreground})for(double speed:{-3.,-1.,.5,1.,2.,3.,8.,16.}){
 nvof::FrucEngine engine{std::filesystem::path(argv[1])};nvof::FractionalRefiner refiner(gpu.device.Get(),gpu.context.Get());double e0=0,e1=0,pos=0;
 auto a=detailed_frame(scene,0,0);
 for(int i=1;i<=4;++i){auto b=detailed_frame(scene,i*speed,i*1000000LL);auto batch=engine.interpolate_pair(a,b,{b.pts-500000});require(batch.frames.size()==1,"Missing baseline");auto out=evaluate(a,b,batch.frames[0],refiner);auto truth=detailed_frame(scene,(i-.5)*speed,b.pts-500000);e0+=mse(batch.frames[0],truth);e1+=mse(out,truth);pos=std::max(pos,std::abs(detailed_position(out,scene,(i-.5)*speed,std::abs(speed))-(i-.5)*speed));a=std::move(b);}
 std::cout<<"MIDPOINT scene="<<name(scene)<<" speed="<<speed<<" baseline_mse="<<e0/4<<" refined_mse="<<e1/4<<" max_position_error="<<pos<<std::endl;
 if(scene==Scene::pan&&std::abs(speed)==1&&e1>e0*.5)failures.push_back("Slow pan not improved by 50%");
 if(e1>e0*1.10+2)failures.push_back("Motion MSE regression");
 }
 for(Adversary scene:{Adversary::crossing,Adversary::pan_object,Adversary::wire,Adversary::fence,Adversary::crossing_bright,Adversary::wire_bright})for(double speed:{1.,3.,12.}){
 nvof::FrucEngine engine{std::filesystem::path(argv[1])};nvof::FractionalRefiner refiner(gpu.device.Get(),gpu.context.Get());double e0=0,e1=0,occ0=0,occ1=0;int missing0=0,missing1=0,extra0=0,extra1=0;
 auto a=adversarial_frame(scene,0,speed,0);
 for(int i=1;i<=3;++i){auto b=adversarial_frame(scene,i,speed,i*1000000LL);auto batch=engine.interpolate_pair(a,b,{b.pts-500000});require(batch.frames.size()==1,"Missing adversary baseline");auto out=evaluate(a,b,batch.frames[0],refiner);auto truth=adversarial_frame(scene,i-.5,speed,b.pts-500000);auto q=local_quality(batch.frames[0],out,truth,scene,i-.5,speed);if(scene==Adversary::fence&&speed==1&&i==2){save_pgm("build/slow-midpoint/fence-baseline.pgm",batch.frames[0]);save_pgm("build/slow-midpoint/fence-new.pgm",out);save_pgm("build/slow-midpoint/fence-truth.pgm",truth);save_pgm("build/slow-midpoint/fence-A.pgm",a);save_pgm("build/slow-midpoint/fence-B.pgm",b);}e0+=q.baseline;e1+=q.refined;occ0+=q.occlusion_baseline;occ1+=q.occlusion_refined;missing0+=q.missing_raw;missing1+=q.missing_new;extra0+=q.extra_raw;extra1+=q.extra_new;a=std::move(b);}
 std::cout<<"DELICATE scene="<<adversary_name(scene)<<" speed="<<speed<<" baseline_mse="<<e0/3<<" refined_mse="<<e1/3<<" occlusion0="<<occ0/3<<" occlusion1="<<occ1/3<<" missing0="<<missing0<<" missing1="<<missing1<<" extra0="<<extra0<<" extra1="<<extra1<<std::endl;
 if(e1>e0*1.10+1.5)failures.push_back(std::string(adversary_name(scene))+" MSE regression");
 if(occ1>occ0*1.10+6)failures.push_back(std::string(adversary_name(scene))+" occlusion regression");
 if(missing1>missing0*1.1+10||extra1>extra0*1.1+10)failures.push_back(std::string(adversary_name(scene))+" ink regression");
 }
 for(const auto& f:failures)std::cout<<"QUALITY_FAILURE "<<f<<std::endl;
 require(failures.empty(),"Midpoint stability regression");std::cout<<"PASS slow midpoint placement, fast motion, thin ink and occlusion protection, untouched originals\n";return 0;
}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
