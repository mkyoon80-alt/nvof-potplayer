#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <stdexcept>
#include <vector>

namespace nvof {
enum class CpuVideoFormat { p010, p016, p210, y210 };
struct CpuVideoLayout {
    int width=0,height=0,pitch=0;
    CpuVideoFormat format=CpuVideoFormat::p010;
};
inline bool cpu_422(CpuVideoFormat f) {return f==CpuVideoFormat::p210 || f==CpuVideoFormat::y210;}
inline size_t cpu_video_bytes(const CpuVideoLayout& l) {
    if(l.width<2 || l.height<2 || l.width>8192 || l.height>8192 || (l.width&1) || (l.height&1) ||
       l.pitch<l.width*(l.format==CpuVideoFormat::y210?4:2) || l.pitch>65536 || (l.pitch&1))
        throw std::invalid_argument("Invalid CPU 10-bit video layout");
    return size_t(l.pitch)*l.height*(l.format==CpuVideoFormat::y210?2:cpu_422(l.format)?4:3)/2;
}
// Unpack to 10-bit code values. Full-height chroma is retained until after
// deinterlacing: averaging opposite-time 4:2:2 fields would produce colour ghosts.
struct CpuVideoPlanes {int width=0,height=0,chroma_height=0;std::vector<uint16_t> y,u,v;};
inline CpuVideoPlanes unpack_cpu_video(const uint8_t* data,size_t bytes,const CpuVideoLayout& l) {
    if(!data || bytes<cpu_video_bytes(l))throw std::invalid_argument("Truncated CPU 10-bit video buffer");
    CpuVideoPlanes p;p.width=l.width;p.height=l.height;p.chroma_height=cpu_422(l.format)?l.height:l.height/2;
    p.y.resize(size_t(l.width)*l.height);p.u.resize(size_t(l.width/2)*p.chroma_height);p.v.resize(p.u.size());
    auto word=[&](size_t offset){uint16_t v;std::memcpy(&v,data+offset,2);return uint16_t(v>>6);};
    if(l.format==CpuVideoFormat::y210) {
        for(int y=0;y<l.height;++y)for(int x=0;x<l.width;x+=2) {
            const size_t s=size_t(y)*l.pitch+x*4,d=size_t(y)*l.width+x,c=d/2;
            p.y[d]=word(s);p.u[c]=word(s+2);p.y[d+1]=word(s+4);p.v[c]=word(s+6);
        }
    } else {
        for(int y=0;y<l.height;++y)for(int x=0;x<l.width;++x)p.y[size_t(y)*l.width+x]=word(size_t(y)*l.pitch+x*2);
        const size_t base=size_t(l.pitch)*l.height;
        for(int y=0;y<p.chroma_height;++y)for(int x=0;x<l.width/2;++x) {
            const size_t c=size_t(y)*(l.width/2)+x,s=base+size_t(y)*l.pitch+x*4;
            p.u[c]=word(s);p.v[c]=word(s+2);
        }
    }
    return p;
}
// Motion-adaptive, edge-directed field reconstruction. Stable areas may weave
// only with evidence from the preceding frame; moving areas use this field
// alone. Never interpolate between fields captured at different times.
inline std::vector<uint16_t> reconstruct_field(const std::vector<uint16_t>& a,const std::vector<uint16_t>* previous,
                                              int w,int h,int parity) {
    if(w<1 || h<2 || (h&1) || (parity!=0 && parity!=1) || a.size()!=size_t(w)*h)
        throw std::invalid_argument("Invalid field plane");
    std::vector<uint16_t> out(a.size());
    auto at=[&](const std::vector<uint16_t>& v,int x,int y){return int(v[size_t(std::clamp(y,0,h-1))*w+std::clamp(x,0,w-1)]);};
    for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
        const size_t pos=size_t(y)*w+x;
        if((y&1)==parity){out[pos]=a[pos];continue;}
        int top=y-1,bottom=y+1;
        if(top<0)top=bottom;if(bottom>=h)bottom=top;
        bool still=previous && previous->size()==a.size();
        if(still)for(int dy=-1;dy<=1 && still;++dy)for(int dx=-1;dx<=1;++dx)
            if(std::abs(at(a,x+dx,y+dy)-at(*previous,x+dx,y+dy))>2){still=false;break;}
        if(still){out[pos]=a[pos];continue;}
        int best=std::abs(at(a,x,top)-at(a,x,bottom)),value=(at(a,x,top)+at(a,x,bottom)+1)/2;
        for(int d:{-1,1,-2,2}) {
            // Prefer the vertical estimate unless a diagonal is clearly better.
            int score=std::abs(at(a,x-d,top)-at(a,x+d,bottom))+std::abs(d)*2;
            if(score<best){best=score;value=(at(a,x-d,top)+at(a,x+d,bottom)+1)/2;}
        }
        out[pos]=uint16_t(value);
    }
    return out;
}
inline std::vector<uint16_t> pack_cpu_p010(const CpuVideoPlanes& p,int parity=-1,const CpuVideoPlanes* previous=nullptr) {
    if(p.width<2 || p.height<2 || (p.width&1) || (p.height&1) ||
       (p.chroma_height!=p.height && p.chroma_height!=p.height/2) ||
       p.y.size()!=size_t(p.width)*p.height || p.u.size()!=size_t(p.width/2)*p.chroma_height || p.v.size()!=p.u.size())
        throw std::invalid_argument("Invalid video planes");
    const bool matching=previous && previous->width==p.width && previous->height==p.height && previous->chroma_height==p.chroma_height;
    std::vector<uint16_t> fy,fu,fv;
    const auto* y=&p.y;const auto* u=&p.u;const auto* v=&p.v;
    if(parity>=0) {
        fy=reconstruct_field(p.y,matching?&previous->y:nullptr,p.width,p.height,parity);
        fu=reconstruct_field(p.u,matching?&previous->u:nullptr,p.width/2,p.chroma_height,parity);
        fv=reconstruct_field(p.v,matching?&previous->v:nullptr,p.width/2,p.chroma_height,parity);
        y=&fy;u=&fu;v=&fv;
    }
    std::vector<uint16_t> out(size_t(p.width)*p.height*3/2);
    for(size_t i=0;i<y->size();++i)out[i]=uint16_t((*y)[i]<<6);
    const size_t base=size_t(p.width)*p.height;
    for(int row=0;row<p.height/2;++row)for(int x=0;x<p.width/2;++x) {
        const bool full=p.chroma_height==p.height;
        const size_t c=size_t(row*(full?2:1))*(p.width/2)+x;
        const unsigned uc=full?((*u)[c]+(*u)[c+p.width/2]+1)/2:(*u)[c];
        const unsigned vc=full?((*v)[c]+(*v)[c+p.width/2]+1)/2:(*v)[c];
        const size_t d=base+size_t(row)*p.width+x*2;
        out[d]=uint16_t(uc<<6);out[d+1]=uint16_t(vc<<6);
    }
    return out;
}
}
