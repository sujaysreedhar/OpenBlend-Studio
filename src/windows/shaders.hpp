// SPDX-License-Identifier: GPL-3.0-only
#pragma once
namespace ps::win {
inline constexpr char shaders[] = R"HLSL(
cbuffer Settings : register(b0) {
 float4 crop; float4 params; float4 widths; float4 response;
 float4 gain; float4 offset; float4 adjustments;
};
Texture2D image : register(t0); Texture2D maskImage : register(t1); Texture2D curveImage : register(t2);
SamplerState linearSampler : register(s0);
struct V { float2 p:POSITION; float2 uv:TEXCOORD0; };
struct P { float4 p:SV_POSITION; float2 uv:TEXCOORD0; };
P warpVS(V i){P o;o.p=float4(i.p.x*2-1,1-i.p.y*2,0,1);o.uv=i.uv;return o;}
P fullVS(uint id:SV_VertexID){P o;o.uv=float2((id<<1)&2,id&2);o.p=float4(o.uv.x*2-1,1-o.uv.y*2,0,1);return o;}
float3 decode(float3 c){return lerp(c/12.92,pow(max((c+.055)/1.055,0),2.4),step(.04045,c));}
float3 encode(float3 c){return lerp(c*12.92,1.055*pow(max(c,0),1/2.4)-.055,step(.0031308,c));}
float3 pattern(float2 uv,int kind){
 if(kind==1)return 1;if(kind==2)return 0;if(kind==3)return float3(1,0,0);if(kind==4)return float3(0,1,0);if(kind==5)return float3(0,0,1);
 if(kind==6)return uv.x.xxx;
 if(kind==7)return fmod(floor(uv.x*16)+floor(uv.y*9),2)>.5?.85:.05;
 if(kind==8)return abs(uv.x-frac(params.w*.2))<.035?float3(1,.4,.1):float3(.02,.025,.03);
 if(abs(uv.x-.5)<.002||abs(uv.y-.5)<.003)return float3(1,.38,.12);
 float2 g=abs(uv*float2(16,9)-round(uv*float2(16,9)));
 if(g.x<.02||g.y<.02)return float3(.4,.65,.8);
 if(uv.x<.003||uv.x>.997||uv.y<.004||uv.y>.996)return 1;
 return float3(.045,.065,.09);
}
float4 warpPS(P i):SV_TARGET {
 float2 uv=crop.xy+i.uv*crop.zw;
 float4 c=params.y<0?image.Sample(linearSampler,uv):float4(pattern(uv,(int)params.y),1);
 float a=saturate(params.x*c.a);if(params.z>0)a*=maskImage.Sample(linearSampler,i.uv).r;
 return float4(decode(c.rgb)*a,a);
}
float ramp(float x){return curveImage.SampleLevel(linearSampler,float2(saturate(x),.5),0).r;}
bool digit(float2 p,int value) {
 uint bits[10]={63,6,91,79,102,109,125,7,127,111};
 uint b=bits[clamp(value,0,9)];
 bool h=p.x>.15&&p.x<.85;
 bool top=h&&abs(p.y-.08)<.06, middle=h&&abs(p.y-.5)<.06, bottom=h&&abs(p.y-.92)<.06;
 bool upper=p.y>.1&&p.y<.48, lower=p.y>.52&&p.y<.9;
 bool left=p.x>.06&&p.x<.18, right=p.x>.82&&p.x<.94;
 return ((b&1)&&top)||((b&2)&&right&&upper)||((b&4)&&right&&lower)||((b&8)&&bottom)||
        ((b&16)&&left&&lower)||((b&32)&&left&&upper)||((b&64)&&middle);
}
float4 outputPS(P i):SV_TARGET {
 if(adjustments.w>0 && i.uv.x>.35&&i.uv.x<.65&&i.uv.y>.3&&i.uv.y<.7){
   float2 p=float2((i.uv.x-.35)/.15,(i.uv.y-.3)/.4);
   int value=p.x<1?(int)adjustments.w/10:(int)adjustments.w%10;
   p.x=frac(p.x);
   return digit(p,value)?float4(1,.95,.65,1):float4(.015,.025,.03,1);
 }
 float4 composed=image.Sample(linearSampler,i.uv);
 if(composed.a<=0)return float4(0,0,0,1);
 float3 c=composed.rgb;
 float w=1;
 if(widths.x>0)w*=ramp(i.uv.x/widths.x);if(widths.y>0)w*=ramp((1-i.uv.x)/widths.y);
 if(widths.z>0)w*=ramp(i.uv.y/widths.z);if(widths.w>0)w*=ramp((1-i.uv.y)/widths.w);
 w=pow(saturate(w),response.x);
 float lum=dot(c,float3(.2126,.7152,.0722));c=lum+(c-lum)*adjustments.z;
 c=((c-.5)*adjustments.y+.5+adjustments.x)*gain.xyz+offset.xyz;
 c=pow(saturate(c*w+response.y*max(0,2*w-1)),1/response.z);
 return float4(encode(c),1);
}
)HLSL";
}
