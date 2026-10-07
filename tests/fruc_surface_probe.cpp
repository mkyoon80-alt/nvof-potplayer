#define wmain bridge_oracle_entry
#include "fruc_bridge_smoke.cpp"
#undef wmain
#include <cmath>
#include <iomanip>
#include <memory>

// Explicit GPU quality experiment, never registered as an automatic CTest.
namespace probe {
constexpr int width=640,height=360;
uint32_t hash(uint32_t v){v^=v>>16;v*=0x7feb352dU;v^=v>>15;v*=0x846ca68bU;return v^(v>>16);}
double texture(double x,double y,uint32_t seed){x=(x+4096)/8;y=(y+4096)/8;const int gx=int(std::floor(x)),gy=int(std::floor(y));const double a=x-gx,b=y-gy;const auto cell=[seed](int xx,int yy){return double(hash(uint32_t(xx)*733U+uint32_t(yy)*19349663U+seed)%191U);};return (1-a)*(1-b)*cell(gx,gy)+a*(1-b)*cell(gx+1,gy)+(1-a)*b*cell(gx,gy+1)+a*b*cell(gx+1,gy+1);}
uint8_t luma(double x,double y,double shift){return uint8_t(std::lround(32+.70*texture(x-shift,y,0)+.30*texture((x-shift)*4,y*4,73)));}
std::vector<uint8_t> base(double shift){std::vector<uint8_t> p(size_t(width)*height);for(int y=0;y<height;++y)for(int x=0;x<width;++x)p[size_t(y)*width+x]=luma(x,y,shift);return p;}
double sample(const std::vector<uint8_t>& p,double x,double y){const int ix=int(std::floor(x)),iy=int(std::floor(y));const double a=x-ix,b=y-iy;const auto at=[&](int xx,int yy){return p[size_t(std::clamp(yy,0,height-1))*width+std::clamp(xx,0,width-1)];};return (1-a)*(1-b)*at(ix,iy)+a*(1-b)*at(ix+1,iy)+(1-a)*b*at(ix,iy+1)+a*b*at(ix+1,iy+1);}
std::vector<uint8_t> encode(const std::vector<uint8_t>& p,int scale,bool nv12){const int w=width*scale,h=height*scale;std::vector<uint8_t> output(size_t(w)*h*(nv12?3:8)/2,128);for(int y=0;y<h;++y)for(int x=0;x<w;++x){const auto value=uint8_t(std::lround(sample(p,(x+.5)/scale-.5,(y+.5)/scale-.5)));if(nv12)output[size_t(y)*w+x]=value;else{const size_t k=(size_t(y)*w+x)*4;output[k]=output[k+1]=output[k+2]=value;output[k+3]=255;}}return output;}
std::vector<uint8_t> decode(const std::vector<uint8_t>& p,int scale,bool nv12){const int w=width*scale;std::vector<uint8_t> output(size_t(width)*height);for(int y=0;y<height;++y)for(int x=0;x<width;++x){unsigned sum=0;for(int oy=0;oy<scale;++oy)for(int ox=0;ox<scale;++ox)sum+=p[(size_t(y*scale+oy)*w+x*scale+ox)*(nv12?1:4)];output[size_t(y)*width+x]=uint8_t((sum+scale*scale/2)/(scale*scale));}return output;}
double mse(const std::vector<uint8_t>& a,const std::vector<uint8_t>& b){double sum=0;int count=0;for(int y=16;y<height-16;++y)for(int x=16;x<width-16;++x){const double d=int(a[size_t(y)*width+x])-int(b[size_t(y)*width+x]);sum+=d*d;++count;}return sum/count;}
double position(const std::vector<uint8_t>& p,double expected){double best=expected,error=std::numeric_limits<double>::infinity();for(int i=-16;i<=16;++i){const double shift=expected+i*.0625;double e=0;for(int y=32;y<height-32;y+=5)for(int x=32;x<width-32;x+=5){const double d=int(p[size_t(y)*width+x])-int(luma(x,y,shift));e+=d*d;}if(e<error){error=e;best=shift;}}return best;}
double sharpness(const std::vector<uint8_t>& p){double sum=0;int count=0;for(int y=16;y<height-16;++y)for(int x=16;x<width-16;++x){const size_t k=size_t(y)*width+x;const double d=4*int(p[k])-int(p[k-1])-int(p[k+1])-int(p[k-width])-int(p[k+width]);sum+=d*d;++count;}return std::sqrt(sum/count);}
}
int wmain(int argc,wchar_t** argv){try{
    if(argc<2)throw std::runtime_error("fruc_surface_probe <runtime> [pairs=8] [max-scale=2]");
    const int pairs=argc>2?std::stoi(argv[2]):8,max_scale=argc>3?std::stoi(argv[3]):2;
    require(pairs>=1&&pairs<=32&&max_scale>=1&&max_scale<=4,"Invalid probe bounds");
    const auto runtime=std::filesystem::absolute(argv[1]);Module nvidia(runtime/L"NvOFFRUC.dll"),bridge(runtime/L"NvofFrucBridge.dll");Context context;
    // Retain address identities across the whole experiment, avoiding SDK
    // registration-address reuse sensitivity after a session is destroyed.
    std::vector<std::unique_ptr<Resources>> lifetime;
    std::cout<<std::fixed<<std::setprecision(6);
    for(int scale=1;scale<=max_scale;scale*=2)for(bool nv12:{true,false})for(int phase:{20,40,50,60,80}){
        lifetime.push_back(std::make_unique<Resources>(size_t(probe::width*scale)*(probe::height*scale)*(nv12?3:8)/2));auto& resources=*lifetime.back();
        Adapter session(bridge.module);
        check(session.init(session.handle,probe::width*scale,probe::height*scale,nv12),"surface init");
        check(session.registration(session.handle,&resources.values[0],&resources.values[1],&resources.values[2]),"surface register");
        const auto run=[&](int index,int64_t input,int64_t output){NVEncNVOFFRUCParams p{&resources.values[index],input,&resources.values[2],output};NVEncNVOFFRUCResult result;check(session.process_ex(session.handle,&p,&result),"surface process");require((result.flags&1)!=0,"Missing repetition metadata");return (result.flags&2)!=0;};
        resources.upload(0,probe::encode(probe::base(0),scale,nv12));run(0,0,0);
        double total_error=0,total_position=0,total_sharpness=0,total_truth_sharpness=0,total_baseline=0;int repetitions=0;
        for(int pair=0;pair<pairs;++pair){const int index=(pair+1)%2;resources.upload(index,probe::encode(probe::base((pair+1)*4),scale,nv12));const bool repeated=run(index,int64_t(pair+1)*1000000,int64_t(pair)*1000000+phase*10000);const auto actual=probe::decode(resources.read(),scale,nv12);const double expected=(pair+phase/100.0)*4;const auto truth=probe::base(expected);const auto baseline=probe::decode(probe::encode(truth,scale,nv12),scale,nv12);const double error=probe::mse(actual,truth),fitted=probe::position(actual,expected);total_error+=error;total_position+=std::abs(fitted-expected);total_sharpness+=probe::sharpness(actual);total_truth_sharpness+=probe::sharpness(truth);total_baseline+=probe::mse(baseline,truth);repetitions+=repeated;
            std::cout<<"FRAME surface="<<(nv12?"NV12":"ARGB")<<" scale="<<scale<<" phase="<<phase<<" pair="<<pair<<" mse="<<error<<" expected="<<expected<<" fitted="<<fitted<<" repeated="<<repeated<<'\n';}
        std::cout<<"RESULT surface="<<(nv12?"NV12":"ARGB")<<" scale="<<scale<<" phase="<<phase<<" pairs="<<pairs<<" mean_mse="<<total_error/pairs<<" mean_position_error="<<total_position/pairs<<" sharpness="<<total_sharpness/pairs<<" truth_sharpness="<<total_truth_sharpness/pairs<<" spatial_baseline_mse="<<total_baseline/pairs<<" repeated="<<repetitions<<'\n';
    }
    return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
