// Exercise independent toggles through the production GPU engine, including reset.
#define NVOF_APPEARANCE_HELPERS_ONLY
#include "appearance_midpoint_quality.cpp"
#undef NVOF_APPEARANCE_HELPERS_ONLY
#include "nvof/gpu_engine.hpp"
int wmain(int argc,wchar_t** argv) { try {
 require(argc==2,"enhancement_options_gpu <runtime>"); TestGpu gpu;
 int cases=0;
 for(bool correction:{false,true})for(bool mouth:{false,true}) {
  nvof::GpuFrucEngine engine(std::filesystem::path(argv[1]),gpu.device.Get(),gpu.context.Get(),nullptr,nvof::GpuCompletionMode::blocking,true,correction,mouth);
  auto a=expression(0,false,48,0),b=expression(1,true,48,1000000);
  auto ga=gpu.texture(&a),gb=gpu.texture(&b);
  nvof::GpuFrame left{ga,0,width,height,0},right{gb,0,width,height,1000000};
  for(int seek=0;seek<2;++seek){
   engine.reset();auto result=engine.interpolate_pair(left,right,{500000});
   require(!result.quality.scene_cut&&!result.quality.repeated_mask&&result.frames.size()==1,"Option combination did not generate midpoint");
   require(result.quality.midpoint_stabilized_mask==(correction?1u:0u),"GPU correction switch ignored");
   require(result.quality.appearance_protected_mask==(mouth?1u:0u),"Mouth protection switch ignored");
   require(!result.quality.midpoint_stabilization_unavailable,"Requested optional path unavailable");
   auto actual=gpu.readback(result.frames[0].texture.Get(),500000);
   require(actual.pixels!=a.pixels&&actual.pixels!=b.pixels,"Motion collapsed to endpoint");
   require(gpu.readback(ga.Get(),0).pixels==a.pixels&&gpu.readback(gb.Get(),1000000).pixels==b.pixels,"Original texture altered");
   ++cases;
  }
  std::cout<<"OPTIONS correction="<<correction<<" mouth="<<mouth<<" reset=2 PASS\n";
 }
 std::cout<<"PASS independent GPU combinations="<<cases<<"\n";return 0;
 }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
