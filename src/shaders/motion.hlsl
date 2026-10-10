
Texture2D<float> ay:register(t0);
Texture2D<float> by:register(t1);
Texture2D<float2> auv:register(t2);
Texture2D<float2> buv:register(t3);
Texture2D<int2> fw:register(t4);
Texture2D<int2> bw:register(t5);
Texture2D<float4> inverseOffsets:register(t6);
Texture2D<float4> inverseWeights:register(t7);
Texture2D<float> layerMask:register(t8);
Texture2D<uint> forwardCost:register(t9);
Texture2D<uint> backwardCost:register(t10);
Texture2D<int2> rawForward:register(t11);
Texture2D<int2> rawBackward:register(t12);
RWStructuredBuffer<uint> riskStatistics:register(u0);
SamplerState linearClamp:register(s0);
cbuffer Parameters:register(b0) {
    float4 size; // original width/height, analysis width/height
    float4 mode; // flow grid, UV plane, downsample source index, interpolation fraction
};
float4 vs(uint v:SV_VertexID):SV_Position {
    return float4(v==0?float2(-1,-1):(v==1?float2(-1,3):float2(3,-1)),0,1);
}
float yAt(float2 p,bool next) {
    return next?by.SampleLevel(linearClamp,p/size.xy,0):ay.SampleLevel(linearClamp,p/size.xy,0);
}
float2 uvAt(float2 p,bool next) {
    return next?buv.SampleLevel(linearClamp,p/size.xy,0):auv.SampleLevel(linearClamp,p/size.xy,0);
}
float2 color(float2 p,bool next) {return mode.y!=0?uvAt(p,next):float2(yAt(p,next),0);}
bool inside(float2 p) {return all(p>=0.5)&&all(p<=size.xy-0.5);}
float2 flowGradient(float2 p,bool next,out float2 dx,out float2 dy) {
    float2 q=p*size.zw/size.xy/mode.x-0.5;
    int2 i=int2(floor(q)),hi=int2(ceil(size.zw/mode.x))-1;
    float2 t=frac(q);
    int2 a=clamp(i,0,hi),b=clamp(i+int2(1,0),0,hi);
    int2 c=clamp(i+int2(0,1),0,hi),d=clamp(i+1,0,hi);
    float2 fa,fb,fc,fd;
    if(next){fa=bw.Load(int3(a,0));fb=bw.Load(int3(b,0));fc=bw.Load(int3(c,0));fd=bw.Load(int3(d,0));}
    else {fa=fw.Load(int3(a,0));fb=fw.Load(int3(b,0));fc=fw.Load(int3(c,0));fd=fw.Load(int3(d,0));}
    float2 factor=(size.xy/size.zw)/32.0;
    dx=lerp(fb-fa,fd-fc,t.y)*factor*(size.z/size.x/mode.x);
    dy=lerp(fc-fa,fd-fb,t.x)*factor*(size.w/size.y/mode.x);
    return lerp(lerp(fa,fb,t.x),lerp(fc,fd,t.x),t.y)*factor;
}
float2 flow(float2 p,bool next) {float2 dx,dy;return flowGradient(p,next,dx,dy);}
// Repair an ambiguous match only when a competing motion has lower local
// image error and agrees with the independently estimated reverse direction.
float matchCost(float2 p,float2 v,bool next) {
    float photo=0;
    float scale=max(size.x/size.z,size.y/size.w);
    [unroll]for(int y=-1;y<=1;++y)[unroll]for(int x=-1;x<=1;++x) {
        float2 q=p+float2(x,y)*4.0*scale;
        photo+=abs(yAt(q,next)-yAt(q+v,!next))*255.0;
    }
    return photo/9.0;
}
// Recheck repeated structure in the source itself. A flat occlusion may
// match several destinations too, but is not evidence of a periodic pattern.
bool repeatedPatch(float2 p,float2 period,bool next) {
    float scale=max(size.x/size.z,size.y/size.w);
    float error=0,lo=255,hi=0;
    [unroll]for(int y=-1;y<=1;++y)[unroll]for(int x=-1;x<=1;++x) {
        float2 q=p+float2(x,y)*32.0*scale;
        if(!inside(q)||!inside(q+period))return false;
        float a=yAt(q,next)*255.0,b=yAt(q+period,next)*255.0;
        error+=abs(a-b);lo=min(lo,a);hi=max(hi,a);
    }
    return hi-lo>32.0 && error<9.0*12.0;
}
int2 repairMotion(float4 screen:SV_Position):SV_Target {
    bool next=mode.z!=0;
    float scale=max(size.x/size.z,size.y/size.w);
    float2 p=screen.xy*mode.x*size.xy/size.zw;
    float2 original=flow(p,next),best=original;
    float photo=matchCost(p,original,next);
    float fb=length(original+flow(p+original,!next))/scale;
    float bestCost=photo+2.0*min(fb,8.0);
    // mode.w is a pass selector only for this shader: positive = initial
    // repair, zero = propagation. Small displacements are deliberately excluded;
    // broad repair of low-confidence occlusions regresses faces and hands.
    // Propagation additionally needs a large alias and repeated source detail.
    if(length(original)/scale>(mode.w>0.0?32.0:64.0)) {
        [loop]for(int k=0;k<33;++k) {
            int d=(k-1)%4+1;
            float2 delta=d==1?float2(64,0):(d==2?float2(-64,0):(d==3?float2(0,64):float2(0,-64)));
            delta*=exp2(float(((k-1)%16)/4)-1.0);
            float2 anchor=p+delta*scale;
            if(k>0&&!inside(anchor))continue;
            float2 v=k==0?-flow(p,!next):(k>16?-flow(anchor,!next):flow(anchor,next));
            if(k==0){v=-flow(p+v,!next);v=-flow(p+v,!next);}
            float cf=length(v+flow(p+v,!next))/scale;
            float support=k>16?length(-v+flow(anchor-v,next))/scale:length(v+flow(anchor+v,!next))/scale;
            if(inside(p+v)&&(cf<1.5 || (fb>8.0&&support<1.5&&length(v)+16.0*scale<length(original)))) {
                float cp=matchCost(p,v,next),cost=cp+2.0*min(cf,8.0);
                if(cf>=1.5){if(cp>=10.0)continue;cost=cp+2.0+support;}
                if(cp<12.0&&length(v)+16.0*scale<length(original)&&(cost<bestCost*0.6 || (cost<bestCost+2.0&&length(v)+16.0*scale<length(best)))) {
                    if(mode.w==0.0&&!repeatedPatch(p,original-v,next))continue;
                    best=v;bestCost=cost;
                }
            }
        }
    }
    return int2(round(best*size.zw/size.xy*32.0));
}
// Reject frame-wide correspondence failure, not isolated occlusion boundaries.
// Sampling and aggregation remain on the GPU; only one count is read by CPU.
[numthreads(8,8,1)] void assessMotion(uint3 id:SV_DispatchThreadID) {
    if(id.x>=128||id.y>=72)return;
    float2 p=(float2(id.xy)+0.5)*size.xy/float2(128,72);
    float scale=max(size.x/size.z,size.y/size.w);
    uint bad=0;
    [unroll]for(uint n=0;n<2;++n) {
        bool next=n!=0;float2 v=flow(p,next),q=p+v;
        if(inside(q)) {
            float inconsistency=length(v+flow(q,!next));
            float photo=abs(yAt(p,next)-yAt(q,!next))*255.0;
            if(inconsistency>4.0*scale&&photo>8.0)++bad;
        }
    }
    if(bad)InterlockedAdd(riskStatistics[0],bad);
}
// Analysis alone is reduced. Every synthesized sample uses original-resolution Y/UV.
float4 downsample(float4 screen:SV_Position):SV_Target {
    float2 p=screen.xy*(mode.y!=0?2.0:1.0)*size.xy/size.zw;
    float2 footprint=size.xy/size.zw*(mode.y!=0?2.0:1.0);
    float2 d=0.25*footprint;
    bool next=mode.z!=0;
    return float4(0.25*(color(p-d,next)+color(p+d,next)+
        color(p+float2(d.x,-d.y),next)+color(p+float2(-d.x,d.y),next)),0,1);
}
float evidence(float2 q,float2 p,bool next) {
    float2 f=flow(q,next),end=q+f;
    if(!inside(q)||!inside(end))return 0;
    float scale=max(size.x/size.z,size.y/size.w);
    float fb=length(f+flow(end,!next));
    float residual=length(q+(next?1.0-mode.w:mode.w)*f-p);
    float photo=abs(yAt(q,next)-yAt(end,!next))*255.0;
    float2 chroma=abs(uvAt(q,next)-uvAt(end,!next))*255.0;
    photo=max(photo,max(chroma.x,chroma.y));
    return (1-smoothstep(0.4*scale,2.0*scale,fb))*
        (1-smoothstep(0.25*scale,1.0*scale,residual))*
        (1-smoothstep(8.0,28.0,photo));
}
// Fixed-point inversion oscillates where adjacent motion layers have steep
// gradients. Use a bounded Newton step with backtracking; never accept a step
// that increases the forward-projection residual.
float2 solveInverse(float2 p,bool next,float fraction,float2 seed) {
    float2 q=seed;
    [loop]for(int i=0;i<6;++i) {
        float2 dx,dy;
        float2 v=flowGradient(q,next,dx,dy),r=q+fraction*v-p;
        float error=dot(r,r);
        if(error<0.0001)break;
        float2 jx=float2(1,0)+fraction*dx,jy=float2(0,1)+fraction*dy;
        float det=jx.x*jy.y-jy.x*jx.y;
        float2 step=abs(det)>0.1?float2(jy.y*r.x-jy.x*r.y,-jx.y*r.x+jx.x*r.y)/det:
            r/(1.0+fraction*(length(dx)+length(dy)));
        float maxStep=32.0*max(size.x/size.z,size.y/size.w);
        step*=min(1.0,maxStep/max(length(step),0.001));
        float2 candidate=q-step;
        float2 cr=candidate+fraction*flow(candidate,next)-p;
        [unroll]for(int k=0;k<3;++k) {
            if(dot(cr,cr)<=error)break;
            step*=0.5;candidate=q-step;
            cr=candidate+fraction*flow(candidate,next)-p;
        }
        if(dot(cr,cr)>=error)break;
        q=candidate;
    }
    return q;
}
float warpReliability(float2 q,float2 p,bool next,float fraction) {
    float2 dx,dy,v=flowGradient(q,next,dx,dy);
    float2 residual=q+fraction*v-p;
    float scale=max(size.x/size.z,size.y/size.w);
    float2 jx=float2(1,0)+fraction*dx,jy=float2(0,1)+fraction*dy;
    float determinant=jx.x*jy.y-jy.x*jx.y;
    // Folded mappings and unsolved inverse positions must not gain the same
    // prior as a valid correspondence just because both photometric scores fail.
    // A folded field can still provide the only photometrically valid source.
    // Conversely, a positive determinant does not rescue a mismatched patch.
    // Keep a bounded geometric prior, attenuated by the local image mismatch.
    float photo=matchCost(q,v,next);
    float patch=1.0-smoothstep(3.0,12.0,photo);
    float geometric=max(0.01+0.99*smoothstep(0.0,0.25,determinant),0.25*patch);
    float bad=photo/12.0;
    return geometric/(1.0+bad*bad*bad*bad)/
        (1.0+dot(residual,residual)/(0.25*scale*scale));
}

// A single Newton seed can converge to the background branch at an occlusion.
// Retry only folded or unsolved mappings, retaining the original solution
// unless an in-bounds alternative has clearly better bidirectional evidence.
// The bounded five-seed search runs at analysis resolution, shared by Y/UV.
float2 inversePosition(float2 p,bool next,float fraction) {
    float2 best=solveInverse(p,next,fraction,p-fraction*flow(p,next));
    float quality=(0.05+evidence(best,p,next))*warpReliability(best,p,next,fraction);
    float scale=max(size.x/size.z,size.y/size.w);
    float2 dx,dy,v=flowGradient(best,next,dx,dy);
    float2 residual=best+fraction*v-p;
    float2 jx=float2(1,0)+fraction*dx,jy=float2(0,1)+fraction*dy;
    float determinant=jx.x*jy.y-jy.x*jx.y;
    if(quality<0.4&&(determinant<0.25||dot(residual,residual)>0.25*scale*scale)) {
        [loop]for(int k=0;k<5;++k) {
            float2 direction=k==1?float2(1,0):k==2?float2(-1,0):k==3?float2(0,1):float2(0,-1);
            float2 seed=k==0?p+fraction*flow(p,!next):p-fraction*flow(p+direction*32.0*scale,next);
            float2 q=solveInverse(p,next,fraction,seed);
            if(!inside(q))continue;
            float score=(0.05+evidence(q,p,next))*warpReliability(q,p,next,fraction);
            if(score>quality*1.2+0.001){best=q;quality=score;}
        }
    }
    return best;
}
// Cost belongs to the hardware vector at the same source-grid coordinate.
// A repaired vector has no hardware cost; leave its existing evidence intact.
// An unbound cost SRV reads zero, preserving baseline weights on unsupported GPUs.
float hardwareWeightAt(int2 cell,bool next) {
    int2 original=next?rawBackward.Load(int3(cell,0)):rawForward.Load(int3(cell,0));
    int2 repaired=next?bw.Load(int3(cell,0)):fw.Load(int3(cell,0));
    if(any(original!=repaired))return 1.0;
    float cost=next?backwardCost.Load(int3(cell,0)):forwardCost.Load(int3(cell,0));
    // Bounded influence: cost cannot select a source with zero geometric evidence,
    // stop motion, or turn a pair into a repeated frame.
    return max(0.25,1.0/(1.0+cost/64.0));
}
float hardwareWeight(float2 p,bool next) {
    float2 q=p*size.zw/size.xy/mode.x-0.5;
    int2 i=int2(floor(q)),hi=int2(ceil(size.zw/mode.x))-1;
    float2 t=frac(q);
    return lerp(lerp(hardwareWeightAt(clamp(i,0,hi),next),hardwareWeightAt(clamp(i+int2(1,0),0,hi),next),t.x),
                lerp(hardwareWeightAt(clamp(i+int2(0,1),0,hi),next),hardwareWeightAt(clamp(i+1,0,hi),next),t.x),t.y);
}
// Compute one continuous inverse field and its visibility weights per output
// time. Both full-resolution planes sample it, avoiding independent per-pixel
// branch choices and Y/UV disagreement. Only the map is at analysis resolution.
struct InverseOutput { float4 offsets:SV_Target0;float4 weights:SV_Target1; };
InverseOutput inverseMap(float4 screen:SV_Position) {
    float2 p=screen.xy*size.xy/size.zw;
    float2 a=inversePosition(p,false,mode.w),b=inversePosition(p,true,1.0-mode.w);
    float wa=evidence(a,p,false),wb=evidence(b,p,true);
    InverseOutput result;
    result.offsets=float4(a-p,b-p);
    result.weights=float4(
        inside(a)?(1.0-mode.w)*(0.05+wa)*warpReliability(a,p,false,mode.w):0.0,
        inside(b)?mode.w*(0.05+wb)*warpReliability(b,p,true,1.0-mode.w):0.0,
        hardwareWeight(a,false),hardwareWeight(b,true));
    return result;
}
// Seed a stationary layer from matching high-contrast source detail. Expand
// its mask at full resolution before sampling displaced source positions, so
// antialiased edges do not leak outside a stationary glyph or curved outline.
float stationaryLayer(float2 p) {
    float centerA=yAt(p,false),centerB=yAt(p,true);
    float error=0,weight=0;
    [unroll]for(int y=-1;y<=1;++y)[unroll]for(int x=-1;x<=1;++x) {
        float2 q=p+float2(x,y);
        float av=yAt(q,false),bv=yAt(q,true);
        float edge=max(abs(av-centerA),abs(bv-centerB))*255.0;
        float local=1.0/(1.0+edge*edge/16.0);
        float delta=(av-bv)*255.0;
        error+=local*delta*delta;weight+=local;
    }
    if(error/weight>=4.0)return 0.0;
    float detail=0;
    [unroll]for(int k=0;k<4;++k) {
        float2 d=k==0?float2(8,0):(k==1?float2(-8,0):(k==2?float2(0,8):float2(0,-8)));
        float da=(yAt(p+d,false)-centerA)*255.0,db=(yAt(p+d,true)-centerB)*255.0;
        float sameEdge=da*db>0?min(abs(da),abs(db)):0;
        if(sameEdge>24.0) {
            // Agreement of the endpoint colors alone cannot locate an edge:
            // a moving dark hair strand can bracket the same bright interior.
            // Compare normalized profiles, allowing the background luminance
            // behind a genuinely fixed title to change between frames.
            float positionError=0;
            [unroll]for(int step=1;step<8;++step) {
                float2 q=p+d*(float(step)/8.0);
                float na=(yAt(q,false)-centerA)*255.0/da;
                float nb=(yAt(q,true)-centerB)*255.0/db;
                positionError=max(positionError,abs(na-nb));
            }
            float consistent=1.0-smoothstep(0.06,0.16,positionError);
            detail=max(detail,consistent*sameEdge*(1.0-smoothstep(0.35,0.75,abs(da-db)/sameEdge)));
        }
    }
    // Flat, similarly colored areas are not evidence of a stationary object.
    float still=(1.0-smoothstep(1.0,4.0,error/weight))*smoothstep(24.0,64.0,detail);
    // Stationary evidence must also beat the moving correspondence across
    // the neighborhood; a flat interior alone can hide displaced fine lines.
    if(still>0.0) {
        float2 vf=flow(p,false),vb=flow(p,true);
        float fixedError=0,motionError=0;
        [unroll]for(int y=-2;y<=2;++y)[unroll]for(int x=-2;x<=2;++x) {
            float2 q=p+float2(x,y)*2.0;
            float av=yAt(q,false),bv=yAt(q,true);
            fixedError+=abs(av-bv)*255.0;
            motionError+=0.5*(abs(av-yAt(q+vf,true))+abs(bv-yAt(q+vb,false)))*255.0;
        }
        still*=smoothstep(0.5,2.0,(motionError-fixedError)/25.0);
    }
    return still;
}
float4 stationaryMask(float4 screen:SV_Position):SV_Target {
    return float4(stationaryLayer(screen.xy),0,0,1);
}
float4 expandStationaryMask(float4 screen:SV_Position):SV_Target {
    float mask=0;
    [unroll]for(int offset=-4;offset<=4;++offset) {
        int2 d=mode.z==0?int2(offset,0):int2(0,offset);
        mask=max(mask,layerMask.Load(int3(clamp(int2(screen.xy)+d,0,int2(size.xy)-1),0)));
    }
    return float4(mask,0,0,1);
}
float4 midpoint(float4 screen:SV_Position):SV_Target {
    float2 p=screen.xy*(mode.y!=0?2.0:1.0);
    float4 offsets=inverseOffsets.SampleLevel(linearClamp,p/size.xy,0);
    float4 weights=inverseWeights.SampleLevel(linearClamp,p/size.xy,0);
    float2 a=p+offsets.xy,b=p+offsets.zw;
    // Retain baseline evidence in xy for stationary-background protection.
    // Hardware cost in zw changes only the moving-source blend.
    float ta=weights.x*weights.z,tb=weights.y*weights.w;
    float2 ca=color(a,false),cb=color(b,true);
    float2 warped=ta+tb>0.0?(ca*ta+cb*tb)/(ta+tb):lerp(ca,cb,mode.w);
    float still=layerMask.SampleLevel(linearClamp,p/size.xy,0);
    float sa=layerMask.SampleLevel(linearClamp,a/size.xy,0)*smoothstep(1.0,3.0,length(a-p));
    float sb=layerMask.SampleLevel(linearClamp,b/size.xy,0)*smoothstep(1.0,3.0,length(b-p));
    float2 fixed=lerp(color(p,false),color(p,true),mode.w);
    float sourceStatic=max(sa,sb);
    // The dilated mask also covers a narrow moving-background margin. Only
    // replace the conservative fallback with the other warped direction when
    // that direction has positive matching evidence, not merely a tiny prior.
    float validA=smoothstep(0.08,0.30,weights.x/max(1.0-mode.w,0.0001));
    float validB=smoothstep(0.08,0.30,weights.y/max(mode.w,0.0001));
    float useA=sb*(1.0-sa)*validA,useB=sa*(1.0-sb)*validB;
    float2 protectedBackground=fixed*(1.0-useA-useB)+ca*useA+cb*useB;
    float2 result=lerp(warped,protectedBackground,sourceStatic);
    return float4(lerp(result,fixed,still),0,1);
}
