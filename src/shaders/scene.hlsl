
Texture2D<float> sourceA:register(t0);Texture2D<float> sourceB:register(t1);
Texture2D<float2> chromaA:register(t2);Texture2D<float2> chromaB:register(t3);
RWStructuredBuffer<uint> stats:register(u0);
cbuffer Dimensions:register(b0){uint width;uint height;uint2 padding;};
groupshared uint different;
[numthreads(16,16,1)] void main(uint3 id:SV_DispatchThreadID,uint groupIndex:SV_GroupIndex){
 if(groupIndex==0)different=0;GroupMemoryBarrierWithGroupSync();
 bool changed=false;
 if(id.x<width&&id.y<height){int3 p=int3(id.xy,0);changed=sourceA.Load(p)!=sourceB.Load(p);}
 if(id.x<width/2&&id.y<height/2){int3 p=int3(id.xy,0);changed=changed||any(chromaA.Load(p)!=chromaB.Load(p));}
 if(changed)InterlockedOr(different,1);GroupMemoryBarrierWithGroupSync();
 if(groupIndex==0&&different)InterlockedOr(stats[69],1);
 if(id.x>=64||id.y>=36)return;
 uint a=0,b=0;
 [unroll]for(uint oy=1;oy<=3;oy+=2)[unroll]for(uint ox=1;ox<=3;ox+=2){
  int3 p=int3((id.x*4+ox)*width/256,(id.y*4+oy)*height/144,0);
  a+=uint(round(sourceA.Load(p)*255));b+=uint(round(sourceB.Load(p)*255));
 }
 a=(a+2)/4;b=(b+2)/4;int delta=int(b)-int(a);uint diff=uint(abs(delta));
 InterlockedAdd(stats[0],1);InterlockedAdd(stats[1],diff);
 InterlockedAdd(stats[2],asuint(delta));InterlockedAdd(stats[3],uint(delta*delta));
 if(diff>=32)InterlockedAdd(stats[4],1);
 InterlockedAdd(stats[5+(a>>3)],1);InterlockedAdd(stats[37+(b>>3)],1);
}
