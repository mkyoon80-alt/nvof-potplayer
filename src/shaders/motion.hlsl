
Texture2D<float> ay:register(t0);
Texture2D<float> by:register(t1);
Texture2D<float2> auv:register(t2);
Texture2D<float2> buv:register(t3);
Texture2D<int2> fw:register(t4);
Texture2D<int2> bw:register(t5);
Texture2D<float4> inverseOffsets:register(t6);
Texture2D<float4> inverseWeights:register(t7);
Texture2D<float2> layerMask:register(t8);
Texture2D<uint> forwardCost:register(t9);
Texture2D<uint> backwardCost:register(t10);
Texture2D<int2> rawForward:register(t11);
Texture2D<int2> rawBackward:register(t12);
Texture2D<float4> glyphMotion:register(t13);
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
// A compact white-on-black glyph patch provides a stronger match than the
// sparse nine-tap motion cost when neighboring credit rows look alike.
float glyphPatchCost(float2 p,float2 v,bool next,out bool glyph) {
    float scale=max(size.x/size.z,size.y/size.w);
    float error=0,ink=0,dark=0,bright=0,neutral=0,black=0;
    [unroll]for(int y=-3;y<=3;++y)[unroll]for(int x=-4;x<=4;++x) {
        float2 q=p+float2(x,y)*2.0*scale;
        if(!inside(q)||!inside(q+v)){glyph=false;return 255.0;}
        float a=yAt(q,next)*255.0,b=yAt(q+v,!next)*255.0;
        float active=max(a,b)>40.0?1.0:0.0;
        error+=abs(a-b)*active;ink+=active;
        dark+=a<28.0?1.0:0.0;bright+=a>120.0?1.0:0.0;
        // Limited-range video black, allowing small compression variation.
        black+=abs(a-16.0)<3.0?1.0:0.0;
        float2 uv=abs(uvAt(q,next)*255.0-128.0);neutral=max(neutral,max(uv.x,uv.y));
    }
    glyph=dark>=32.0&&black>=dark*0.7&&bright>=3.0&&neutral<3.0;
    return error/max(ink,1.0);
}
float2 repairGlyphAlias(float2 p,float2 original,bool next) {
    float scale=max(size.x/size.z,size.y/size.w);
    // Preserve small motions and large-displacement repair. This path addresses
    // one-row aliases below that repair's old 32-analysis-pixel threshold.
    if(length(original)<=8.0*scale||length(original)>32.0*scale)return original;
    bool glyph;float oldCost=glyphPatchCost(p,original,next,glyph);
    if(!glyph||oldCost<12.0)return original;
    float2 best=original;float bestCost=oldCost;
    [loop]for(int k=0;k<16;++k) {
        int d=k%4;float radius=16.0*exp2(float(k/4))*scale;
        float2 delta=d==0?float2(radius,0):d==1?float2(-radius,0):d==2?float2(0,radius):float2(0,-radius);
        float2 anchor=p+delta;
        if(!inside(anchor))continue;
        float2 v=flow(anchor,next);
        // Fixed graphics retain the established stationary-layer path.
        if(length(v)<1.0*scale||length(v)>=8.0*scale||length(original-v)<8.0*scale||!inside(p+v))continue;
        float support=length(v+flow(anchor+v,!next))/scale;
        if(support>1.0)continue;
        // Validate both the neighboring trajectory and the actual glyph. A
        // smaller flow is not preferred merely because it is smaller.
        bool ignored;float cost=glyphPatchCost(p,v,next,ignored);
        if(cost<10.0&&cost<oldCost*0.4&&cost<bestCost) {best=v;bestCost=cost;}
    }
    return best;
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
    if(mode.w>0.0)best=repairGlyphAlias(p,best,next);
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

// Apply hardware uncertainty only where image/geometry evidence is ambiguous.
// Strong local agreement should not be damaged by an uncalibrated hardware cost;
// almost-invalid candidates must not be promoted merely for having low cost.
// Neutral/absent costs and repaired vectors leave the local score unchanged.
float fusedHardwareWeight(float2 q,bool next,float localQuality) {
    float confidence=saturate(localQuality/1.05);
    float ambiguous=4.0*confidence*(1.0-confidence);
    float hw=hardwareWeight(q,next);
    // Re-express the legacy cost/64 weight at four times the sensitivity.
    // This is a bounded heuristic, not a calibrated error probability; apply
    // it after spatial interpolation so repaired/unknown cells stay neutral.
    return lerp(1.0,max(0.10,hw/(4.0-3.0*hw)),ambiguous);
}
float candidateQuality(float2 q,float2 p,bool next,float fraction,int policy) {
    float quality=(0.05+evidence(q,p,next))*warpReliability(q,p,next,fraction);
    return quality*(policy>=2?fusedHardwareWeight(q,next,quality):1.0);
}
// A single Newton seed can converge to the background branch at an occlusion.
// Retry only folded or unsolved mappings, retaining the original solution
// unless an in-bounds alternative has clearly better bidirectional evidence.
// The bounded five-seed search runs at analysis resolution, shared by Y/UV.
float2 inversePosition(float2 p,bool next,float fraction,int policy) {
    float2 best=solveInverse(p,next,fraction,p-fraction*flow(p,next));
    float quality=candidateQuality(best,p,next,fraction,policy);
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
            float score=candidateQuality(q,p,next,fraction,policy);
            if(score>quality*1.2+0.001){best=q;quality=score;}
        }
    }
    return best;
}
// Compute inverse branches and visibility at analysis resolution. Both full-
// resolution planes share these decisions; discontinuities are resolved when
// sampling colors, rather than by averaging incompatible source coordinates.
struct InverseOutput { float4 offsets:SV_Target0;float4 weights:SV_Target1; };
// A small patch plus its worst luma sample prevents a thin mismatched stroke
// from disappearing in an otherwise flat patch. Chroma also constrains matches.
float alignedPatchError(float2 a,float2 b) {
    float scale=max(size.x/size.z,size.y/size.w);
    float error=0,peak=0;
    [unroll]for(int y=-1;y<=1;++y)[unroll]for(int x=-1;x<=1;++x) {
        float2 d=float2(x,y)*3.0*scale;
        float delta=abs(yAt(a+d,false)-yAt(b+d,true))*255.0;
        error+=delta;peak=max(peak,delta);
    }
    float2 chroma=abs(uvAt(a,false)-uvAt(b,true))*255.0;
    return max(error/9.0,peak/2.0)+max(chroma.x,chroma.y);
}
// Only recover a pair of weak, disagreeing inverse candidates. A nearby flow
// proposes one shared trajectory through both originals; its donor must have
// reverse consistency and at least one proposed endpoint must support it too.
// Stationary overlays and candidates without a clear patch improvement stay on
// the established Newton path. This never changes a pair into a frame hold.
bool coherentTrajectory(float2 p,inout float2 a,inout float2 b,inout float qa,inout float qb) {
    if(max(qa,qb)>=0.35||abs(yAt(a,false)-yAt(b,true))*255.0<8.0||layerMask.SampleLevel(linearClamp,p/size.xy,0).x>0.1)return false;
    float error=alignedPatchError(a,b);
    if(error<6.0)return false;
    float scale=max(size.x/size.z,size.y/size.w),bestError=error;
    float2 bestA=a,bestB=b;
    [loop]for(int k=0;k<18;++k) {
        bool next=k>=9;int index=k%9;
        int d=(index-1)%4;
        float2 direction=d==0?float2(1,0):d==1?float2(-1,0):d==2?float2(0,1):float2(0,-1);
        float2 anchor=index==0?(next?b:a):p+direction*(index<=4?8.0:32.0)*scale;
        float2 local=flow(anchor,next),end=anchor+local;
        if(!inside(anchor)||!inside(end)||length(local+flow(end,!next))>1.5*scale)continue;
        float2 v=next?-local:local;
        float2 ca=p-mode.w*v,cb=p+(1.0-mode.w)*v;
        if(!inside(ca)||!inside(cb))continue;
        if(max(layerMask.SampleLevel(linearClamp,ca/size.xy,0).x,layerMask.SampleLevel(linearClamp,cb/size.xy,0).x)>0.1)continue;
        float candidate=alignedPatchError(ca,cb);
        if(candidate<6.0&&candidate<bestError*0.75&&candidate<error*0.5) {
            // At least one endpoint must directly support this trajectory;
            // a similar-colored donor alone is not correspondence evidence.
            float support=min(length(v-flow(ca,false)),length(v+flow(cb,true)));
            if(support>max(1.5,0.5*mode.x+1.0)*scale)continue;
            bestError=candidate;bestA=ca;bestB=cb;
        }
    }
    if(bestError>=error*0.5)return false;
    a=bestA;b=bestB;
    // Keep recovered evidence below the stationary-background acceptance gate.
    // Matching colors alone do not establish strong visibility in an occlusion.
    qa=qb=0.05*(1.0-bestError/12.0);
    return true;
}
InverseOutput inverseMapPolicy(float4 screen,int policy) {
    float2 p=screen.xy*size.xy/size.zw;
    bool fusion=policy>=2;
    float2 a=inversePosition(p,false,mode.w,policy),b=inversePosition(p,true,1.0-mode.w,policy);
    float qa=(0.05+evidence(a,p,false))*warpReliability(a,p,false,mode.w);
    float qb=(0.05+evidence(b,p,true))*warpReliability(b,p,true,1.0-mode.w);
    bool recovered=coherentTrajectory(p,a,b,qa,qb);
    float ca=policy==0||recovered?1.0:(fusion?fusedHardwareWeight(a,false,qa):hardwareWeight(a,false));
    float cb=policy==0||recovered?1.0:(fusion?fusedHardwareWeight(b,true,qb):hardwareWeight(b,true));
    InverseOutput result;
    result.offsets=float4(a-p,b-p);
    result.weights=float4(inside(a)?(1.0-mode.w)*qa:0.0,inside(b)?mode.w*qb:0.0,ca,cb);
    return result;
}
InverseOutput inverseMap(float4 screen:SV_Position) {return inverseMapPolicy(screen,2);}
InverseOutput inverseMapBlend(float4 screen:SV_Position) {return inverseMapPolicy(screen,1);}
InverseOutput inverseMapOff(float4 screen:SV_Position) {return inverseMapPolicy(screen,0);}
// Seed a stationary layer from matching high-contrast source detail. Expand
// its mask at full resolution before sampling displaced source positions, so
// antialiased edges do not leak outside a stationary glyph or curved outline.
// Require the same closed opaque span in both originals along both axes.
// A moving bright rectangle can have a fixed-looking top edge; its side bounds
// still move. An unbounded/ambiguous span is not accepted as a stationary plate.
bool fixedOpaqueSpan(float2 p,float2 direction) {
    [loop]for(int k=1;k<=64;++k) {
        float2 q=p+direction*float(k*4);
        if(!inside(q))return false;
        float a=yAt(q,false)*255.0,b=yAt(q,true)*255.0;
        if(min(a,b)<208.0) {
            float worst=0;
            [unroll]for(int j=-4;j<=1;++j) {
                float2 at=q+direction*float(j);
                float aa=yAt(at,false)*255.0,bb=yAt(at,true)*255.0;
                // Compare the opaque coverage rather than the moving background.
                worst=max(worst,abs(saturate((aa-208.0)/12.0)-saturate((bb-208.0)/12.0)));
            }
            return worst<0.08;
        }
    }
    return false;
}
float2 stationaryLayer(float2 p) {
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
    float detail=0,plateDetail=0;
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
            // A changing background can change contrast without moving the edge.
            // The normalized profile above checks its position independently.
            detail=max(detail,consistent*sameEdge);
            plateDetail=max(plateDetail,(1.0-smoothstep(0.015,0.04,positionError))*sameEdge);
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
    // A neutral opaque graphic has invariant source color and edge position.
    // Hardware flow matching the background is not evidence against that layer.
    float2 ca=abs(uvAt(p,false)*255.0-128.0),cb=abs(uvAt(p,true)*255.0-128.0);
    float neutral=max(max(ca.x,ca.y),max(cb.x,cb.y));
    float plate=smoothstep(208.0,220.0,min(centerA,centerB)*255.0)*
        (1.0-smoothstep(1.0,3.0,neutral))*(1.0-smoothstep(0.5,1.5,abs(centerA-centerB)*255.0))*
        smoothstep(48.0,96.0,plateDetail);
    [branch]if(plate>still) {
        // A fixed overlay has near-zero local flow; inspect its neighboring
        // background before skipping work, otherwise leaking source pixels
        // inside the zero-motion plate would never receive protection.
        float movement=max(length(flow(p,false)),length(flow(p,true)));
        [unroll]for(int d=0;d<4;++d) {
            float2 delta=d==0?float2(16,0):d==1?float2(-16,0):d==2?float2(0,16):float2(0,-16);
            movement=max(movement,max(length(flow(p+delta,false)),length(flow(p+delta,true))));
        }
        if(movement<2.0)return float2(still,0);
        if(!fixedOpaqueSpan(p,float2(1,0))||!fixedOpaqueSpan(p,float2(-1,0))||
           !fixedOpaqueSpan(p,float2(0,1))||!fixedOpaqueSpan(p,float2(0,-1)))plate=0;
    }
    return float2(still,plate);
}
float4 stationaryMask(float4 screen:SV_Position):SV_Target {
    return float4(stationaryLayer(screen.xy),0,1);
}
float4 expandStationaryMask(float4 screen:SV_Position):SV_Target {
    float2 mask=0;float2 p=screen.xy;
    float centerA=yAt(p,false),centerB=yAt(p,true);
    bool opaque=min(centerA,centerB)*255.0>208.0&&abs(centerA-centerB)*255.0<1.5;
    [unroll]for(int offset=-4;offset<=4;++offset) {
        int2 d=mode.z==0?int2(offset,0):int2(0,offset);
        float2 candidate=layerMask.Load(int3(clamp(int2(p)+d,0,int2(size.xy)-1),0));
        mask.x=max(mask.x,candidate.x);
        // The extra plate layer never expands into the moving background.
        if(opaque&&abs(yAt(p+d,false)-centerA)*255.0<2.0&&abs(yAt(p+d,true)-centerB)*255.0<2.0)
            mask.y=max(mask.y,candidate.y);
    }
    return float4(mask,0,1);
}
float2 mappedColor(float2 p,float4 offsets,float4 weights) {
    float2 a=p+offsets.xy,b=p+offsets.zw;
    // Retain baseline evidence in xy for stationary-background protection.
    // Hardware cost in zw changes only the moving-source blend.
    float ta=weights.x*weights.z,tb=weights.y*weights.w;
    float2 ca=color(a,false),cb=color(b,true);
    float2 warped=ta+tb>0.0?(ca*ta+cb*tb)/(ta+tb):lerp(ca,cb,mode.w);
    float still=layerMask.SampleLevel(linearClamp,p/size.xy,0).x;
    float sa=layerMask.SampleLevel(linearClamp,a/size.xy,0).x*smoothstep(1.0,3.0,length(a-p));
    float sb=layerMask.SampleLevel(linearClamp,b/size.xy,0).x*smoothstep(1.0,3.0,length(b-p));
    float2 fixed=lerp(color(p,false),color(p,true),mode.w);
    // Separate provenance for opaque fixed graphics; do not feed this stronger
    // evidence into Newton recovery or widen the existing background margin.
    float plate=layerMask.SampleLevel(linearClamp,p/size.xy,0).y;
    sa=max(sa,layerMask.SampleLevel(linearClamp,a/size.xy,0).y*smoothstep(1.0,3.0,length(a-p)));
    sb=max(sb,layerMask.SampleLevel(linearClamp,b/size.xy,0).y*smoothstep(1.0,3.0,length(b-p)));
    still=max(still,plate);
    float sourceStatic=max(sa,sb);
    // The dilated mask also covers a narrow moving-background margin. Only
    // replace the conservative fallback with the other warped direction when
    // that direction has positive matching evidence, not merely a tiny prior.
    float validA=smoothstep(0.08,0.30,weights.x/max(1.0-mode.w,0.0001));
    float validB=smoothstep(0.08,0.30,weights.y/max(mode.w,0.0001));
    float useA=sb*(1.0-sa)*validA,useB=sa*(1.0-sb)*validB;
    float2 protectedBackground=fixed*(1.0-useA-useB)+ca*useA+cb*useB;
    float2 result=lerp(warped,protectedBackground,sourceStatic);
    return lerp(result,fixed,still);
}

// Fit a rigid translation to a whole monochrome glyph group. This is a
// separate, image-verified layer: unreliable per-letter flow must not bend
// letters that demonstrably translate together. No screen location is assumed.
// One workgroup per overlapping patch. Cache source samples once and score
// candidate translations in parallel; no readback or per-frame allocation.
RWTexture2D<float4> glyphOutput:register(u1);
groupshared float glyphSource[289];
groupshared float4 glyphStats[32];
groupshared float2 glyphErrors[32];
groupshared float4 glyphFits[32];
groupshared float4 glyphBest;
groupshared uint glyphAccepted;
float cachedGroupCost(float2 p,float2 v) {
    float scale=max(size.x/size.z,size.y/size.w);
    if(!inside(p+v-24.0*scale)||!inside(p+v+24.0*scale))return 255.0;
    float error=0,ink=0;
    [loop]for(int i=0;i<289;++i) {
        float2 q=p+float2(i%17-8,i/17-8)*3.0*scale;
        float a=glyphSource[i],b=yAt(q+v,true)*255.0;
        float active=max(a,b)>40.0?1.0:0.0;
        error+=abs(a-b)*active;ink+=active;
    }
    return error/max(ink,1.0);
}
[numthreads(32,1,1)]void glyphGroup(uint3 group:SV_GroupID,uint lane:SV_GroupIndex) {
    float scale=max(size.x/size.z,size.y/size.w);
    float2 p=(float2(group.xy)+0.5)*32.0*scale;
    if(lane==0)glyphOutput[group.xy]=0;
    if(!inside(p-24.0*scale)||!inside(p+24.0*scale))return;
    float4 stats=0;float2 errors=0;
    [loop]for(int i=int(lane);i<289;i+=32) {
        float2 q=p+float2(i%17-8,i/17-8)*3.0*scale;
        float a=yAt(q,false)*255.0,b=yAt(q,true)*255.0;glyphSource[i]=a;
        float active=max(a,b)>40.0?1.0:0.0;errors+=float2(abs(a-b),1)*active;
        stats.x+=a<28.0?1.0:0.0;stats.y+=abs(a-16.0)<3.0?1.0:0.0;stats.z+=a>160.0?1.0:0.0;
        float2 uv=max(abs(uvAt(q,false)*255.0-128.0),abs(uvAt(q,true)*255.0-128.0));
        stats.w=max(stats.w,max(uv.x,uv.y));
    }
    glyphStats[lane]=stats;glyphErrors[lane]=errors;
    GroupMemoryBarrierWithGroupSync();
    [unroll]for(uint step=16;step>0;step/=2) {
        if(lane<step){glyphStats[lane].xyz+=glyphStats[lane+step].xyz;glyphStats[lane].w=max(glyphStats[lane].w,glyphStats[lane+step].w);glyphErrors[lane]+=glyphErrors[lane+step];}
        GroupMemoryBarrierWithGroupSync();
    }
    if(lane==0) {
        float4 st=glyphStats[0];float zero=glyphErrors[0].x/max(glyphErrors[0].y,1.0);
        glyphAccepted=st.x>=160.0&&st.y>=st.x*0.85&&st.z>=8.0&&st.w<3.0&&zero>=12.0;
        glyphBest=float4(0,0,zero,zero);
    }
    GroupMemoryBarrierWithGroupSync();
    if(!glyphAccepted)return;
    float2 v=0;float cost=255.0;
    if(lane<17) {
        float2 anchor=p;
        if(lane>0) {
            int k=int(lane)-1,d=k%4;float radius=16.0*exp2(float(k/4))*scale;
            anchor+=d==0?float2(radius,0):d==1?float2(-radius,0):d==2?float2(0,radius):float2(0,-radius);
        }
        if(inside(anchor)) {
            v=flow(anchor,false);
            if(length(v)>=scale&&length(v)<=16.0*scale)cost=cachedGroupCost(p,v);
        }
    }
    glyphFits[lane]=float4(v,cost,0);
    GroupMemoryBarrierWithGroupSync();
    if(lane==0) {
        [unroll]for(int i=0;i<17;++i)if(glyphFits[i].z<glyphBest.z)glyphBest.xyz=glyphFits[i].xyz;
        glyphAccepted=glyphBest.z<glyphBest.w*0.65;
    }
    GroupMemoryBarrierWithGroupSync();
    if(!glyphAccepted)return;
    [loop]for(int level=0;level<4;++level) {
        float step=scale*exp2(-float(level));v=glyphBest.xy;cost=255.0;
        if(lane<9){v+=float2(int(lane)%3-1,int(lane)/3-1)*step;cost=cachedGroupCost(p,v);}
        glyphFits[lane]=float4(v,cost,0);
        GroupMemoryBarrierWithGroupSync();
        if(lane==0)[unroll]for(int i=0;i<9;++i)if(glyphFits[i].z<glyphBest.z)glyphBest.xyz=glyphFits[i].xyz;
        GroupMemoryBarrierWithGroupSync();
    }
    float neutral=0;
    [loop]for(int i=int(lane);i<289;i+=32) {
        float2 q=p+float2(i%17-8,i/17-8)*3.0*scale+glyphBest.xy;
        float2 uv=abs(uvAt(q,true)*255.0-128.0);neutral=max(neutral,max(uv.x,uv.y));
    }
    glyphStats[lane].w=neutral;
    GroupMemoryBarrierWithGroupSync();
    [unroll]for(uint step=16;step>0;step/=2) {
        if(lane<step)glyphStats[lane].w=max(glyphStats[lane].w,glyphStats[lane+step].w);
        GroupMemoryBarrierWithGroupSync();
    }
    if(lane==0&&glyphStats[0].w<3.0&&glyphBest.z<=10.0&&glyphBest.z<=glyphBest.w*0.35&&length(glyphBest.xy)>=scale)
        glyphOutput[group.xy]=float4(glyphBest.xyz,1);
}
bool glyphGroupColor(float2 p,out float2 value) {
    float scale=max(size.x/size.z,size.y/size.w);
    float2 cell=p/(32.0*scale)-0.5;
    int2 base=int2(floor(cell)),hi=int2(ceil(size.xy/(32.0*scale)))-1;
    float2 velocity=0;float total=0;float2 lo=1e5,high=-1e5;
    [unroll]for(int y=0;y<2;++y)[unroll]for(int x=0;x<2;++x) {
        int2 index=clamp(base+int2(x,y),0,hi);
        float4 model=glyphMotion.Load(int3(index,0));
        float2 center=(float2(index)+0.5)*32.0*scale+mode.w*model.xy;
        float2 weight=saturate(1.0-abs(p-center)/(24.0*scale));
        float a=weight.x*weight.y*model.w;
        if(a>0){velocity+=model.xy*a;total+=a;lo=min(lo,model.xy);high=max(high,model.xy);}
    }
    value=0;
    if(total<0.02||length(high-lo)>1.5*scale)return false;
    velocity/=total;
    float2 a=p-mode.w*velocity,b=p+(1.0-mode.w)*velocity;
    if(!inside(a)||!inside(b))return false;
    float2 chroma=max(abs(uvAt(a,false)*255.0-128.0),abs(uvAt(b,true)*255.0-128.0));
    if(max(chroma.x,chroma.y)>3.0)return false;
    if(abs(yAt(a,false)-yAt(b,true))*255.0>80.0)return false;
    value=lerp(color(a,false),color(b,true),mode.w);return true;
}

float4 midpoint(float4 screen:SV_Position):SV_Target {
    float2 p=screen.xy*(mode.y!=0?2.0:1.0);
    float2 groupColor;
    if(glyphGroupColor(p,groupColor))return float4(groupColor,0,1);
    float2 q=p*size.zw/size.xy-0.5;
    int2 cell=int2(floor(q)),hi=int2(size.zw)-1;
    float2 f=frac(q);
    int2 ia=clamp(cell,0,hi),ib=clamp(cell+int2(1,0),0,hi);
    int2 ic=clamp(cell+int2(0,1),0,hi),id=clamp(cell+1,0,hi);
    float4 oa=inverseOffsets.Load(int3(ia,0)),ob=inverseOffsets.Load(int3(ib,0));
    float4 oc=inverseOffsets.Load(int3(ic,0)),od=inverseOffsets.Load(int3(id,0));
    float4 wa=inverseWeights.Load(int3(ia,0)),wb=inverseWeights.Load(int3(ib,0));
    float4 wc=inverseWeights.Load(int3(ic,0)),wd=inverseWeights.Load(int3(id,0));
    float4 spread=max(max(oa,ob),max(oc,od))-min(min(oa,ob),min(oc,od));
    float discontinuity=max(max(spread.x,spread.y),max(spread.z,spread.w));
    // Opposite sides of a fold can refer to different motion layers. Sampling
    // their averaged coordinate invents an unrelated third position. Blend
    // independently validated source colors only across such discontinuities.
    if(discontinuity>2.0&&any(f>0.001)) {
        float2 ca=mappedColor(p,oa,wa),cb=mappedColor(p,ob,wb);
        float2 cc=mappedColor(p,oc,wc),cd=mappedColor(p,od,wd);
        return float4(lerp(lerp(ca,cb,f.x),lerp(cc,cd,f.x),f.y),0,1);
    }
    return float4(mappedColor(p,lerp(lerp(oa,ob,f.x),lerp(oc,od,f.x),f.y),
        lerp(lerp(wa,wb,f.x),lerp(wc,wd,f.x),f.y)),0,1);
}
