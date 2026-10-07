// Direct SDK oracle for user-provided local raw frames; no product pipeline.
#define wmain old_bridge_test_entry
#include "fruc_bridge_smoke.cpp"
#undef wmain
#include <fstream>
int wmain(int argc,wchar_t**argv){try{
 require(argc>=4,"video_fruc_reference <runtime> <raw NV12> <output raw> [argb]");
 constexpr int w=1920,h=1080;const bool nv12=argc<5;const size_t count=size_t(w)*h*(nv12?3:8)/2;
 Module nvidia(std::filesystem::absolute(argv[1])/L"NvOFFRUC.dll");Context context;Resources memory(count);Reference sdk(nvidia.module);sdk.start(w,h,memory,nv12);
 std::ifstream input(std::filesystem::path(argv[2]),std::ios::binary);std::ofstream output(std::filesystem::path(argv[3]),std::ios::binary);require(bool(input)&&bool(output),"Open data files");
 std::vector<uint8_t> previous(count),current(count);int frames=0,repeats=0;
 while(input.read(reinterpret_cast<char*>(current.data()),count)){
  const int index=frames%2;memory.upload(index,current);
  const int64_t t=int64_t(frames)*10000000*1001/24000,m=frames?int64_t(frames*2-1)*10000000*1001/48000:0;
  const bool repeated=sdk.run(index,double(t),double(m));
  if(frames){auto interpolated=repeated?previous:memory.read();output.write(reinterpret_cast<const char*>(interpolated.data()),count);repeats+=repeated;}
  output.write(reinterpret_cast<const char*>(current.data()),count);previous=current;++frames;
 }
 require(frames>0,"No input frames");output.write(reinterpret_cast<const char*>(previous.data()),count);require(bool(output),"Output write");
 std::cout<<"PASS direct NVIDIA input="<<frames<<" nv12="<<nv12<<" repeats="<<repeats<<std::endl;return 0;
}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
