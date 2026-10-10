
Texture2D<uint4> source:register(t0);
float4 ps(float4 position:SV_Position):SV_Target {
 uint4 code=source.Load(int3(int2(position.xy),0))>>6;
 return float4(min((code+2u)>>2,255u))/255.0f;
}
