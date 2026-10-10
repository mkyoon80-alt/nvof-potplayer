
Texture2D<float4> previousTexture:register(t0);
Texture2D<float4> currentTexture:register(t1);
cbuffer BlendWeight:register(b0){uint weight;uint tenBit;uint2 padding;};
float4 vs(uint vertex:SV_VertexID):SV_Position {
 float2 position=vertex==0?float2(-1,-1):(vertex==1?float2(-1,3):float2(3,-1));
 return float4(position,0,1);
}
float4 ps(float4 position:SV_Position):SV_Target {
 int3 p=int3(int2(position.xy),0);
 float scale=tenBit!=0?65535.0f/64.0f:255.0f;
 uint4 a=uint4(round(previousTexture.Load(p)*scale));
 uint4 b=uint4(round(currentTexture.Load(p)*scale));
 uint4 value=(a*(65536u-weight)+b*weight+32768u)>>16;
 return float4(value)/scale;
}
