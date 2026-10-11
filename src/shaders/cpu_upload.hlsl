Texture2D<uint> currentFrame:register(t0);
Texture2D<uint> previousFrame:register(t1);
cbuffer Params:register(b0) {int width,height,format,chromaHeight;int parity,history,plane,fieldHint;};
float4 vs(uint id:SV_VertexID):SV_Position {
    float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);
}
int code(int channel,int x,int y,bool previous) {
    int w=channel==0?width:width/2,h=channel==0?height:chromaHeight;
    x=clamp(x,0,w-1);y=clamp(y,0,h-1);int2 pos;
    if(format==3)pos=int2(channel==0?x*2:x*4+(channel==1?1:3),y);
    else pos=channel==0?int2(x,y):int2(x*2+channel-1,height+y);
    return int((previous?previousFrame.Load(int3(pos,0)):currentFrame.Load(int3(pos,0)))>>6);
}
int field(int channel,int x,int y) {
    if(parity<0 || (y&1)==parity)return code(channel,x,y,false);
    int h=channel==0?height:chromaHeight;
    int top=y-1,bottom=y+1;if(top<0)top=bottom;if(bottom>=h)bottom=top;
    bool still=history!=0;
    [unroll] for(int dy=-1;dy<=1;++dy) [unroll] for(int dx=-1;dx<=1;++dx)
        if(abs(code(channel,x+dx,y+dy,false)-code(channel,x+dx,y+dy,true))>2)still=false;
    if(still)return code(channel,x,y,false);
    int a=code(channel,x,top,false),b=code(channel,x,bottom,false);
    int best=abs(a-b),value=(a+b+1)/2;
    [unroll] for(int i=0;i<4;++i) {
        int d=i==0?-1:i==1?1:i==2?-2:2;
        a=code(channel,x-d,top,false);b=code(channel,x+d,bottom,false);
        int score=abs(a-b)+abs(d)*2;if(score<best){best=score;value=(a+b+1)/2;}
    }
    return value;
}
float4 ps(float4 position:SV_Position):SV_Target {
    int2 p=int2(position.xy);
    if(plane==0)return float4(float(field(0,p.x,p.y)<<6)/65535.0f,0,0,0);
    int2 uv;
    if(chromaHeight==height)uv=int2((field(1,p.x,p.y*2)+field(1,p.x,p.y*2+1)+1)/2,(field(2,p.x,p.y*2)+field(2,p.x,p.y*2+1)+1)/2);
    else uv=int2(field(1,p.x,p.y),field(2,p.x,p.y));
    return float4(float2(uv<<6)/65535.0f,0,0);
}
RWStructuredBuffer<uint> combCount:register(u0);
groupshared uint groupCombs;
[numthreads(8,8,1)]
void comb(uint3 id:SV_DispatchThreadID,uint lane:SV_GroupIndex) {
    if(lane==0)groupCombs=0;
    GroupMemoryBarrierWithGroupSync();
    int x=min(width-1,int((id.x+0.5)*width/128));
    int y=clamp((int((id.y+0.5)*(height-4)/72)&~1)+2+int(id.x&1),2,height-3);
    int v=code(0,x,y,false),a=code(0,x,y-1,false)-v,b=code(0,x,y+1,false)-v;
    int strength=min(abs(a),abs(b));
    int sameField=max(abs(code(0,x,y-2,false)-v),abs(code(0,x,y+2,false)-v));
    bool moving=history!=0 ? (abs(v-code(0,x,y,true))>12 ||
        abs(code(0,x,y-1,false)-code(0,x,y-1,true))>12) : fieldHint!=0;
    // A moving alternating-line pattern, with the same-time rows agreeing.
    // After a same-input seek, an earlier confirmed raw-field hint permits a
    // spatial-only check. No pre-seek pixel is used as temporal evidence.
    if(moving && a*b>0 && strength>24 && sameField*5<strength*3+20)
        InterlockedAdd(groupCombs,1);
    GroupMemoryBarrierWithGroupSync();
    if(lane==0)InterlockedAdd(combCount[0],groupCombs);
}
