#pragma once
// Embedded to make the signed plugin independent of source-tree paths/resources.
static constexpr const char* waveformMetalSource = R"METAL(
#include <metal_stdlib>
using namespace metal;
struct Column { float low[2], high[2]; uint present; };
struct Point { float x, values[2]; };
struct Params { uint count, traces, overlay, raw; float width, height, scale, range, threshold, xStep, xOffset; };
struct Palette { float4 colour[2], interior, warning; };
struct Out { float4 position [[position]]; float4 colour; float top [[flat]], bottom [[flat]]; uint contour [[flat]]; float value; };
float yFor(float v, constant Params& p) { return p.height * (0.5f - clamp(v / p.range,-1.0f,1.0f)*0.5f); }
vertex Out waveVertex(uint vid [[vertex_id]], uint instance [[instance_id]],
    device const Column* data [[buffer(0)]], constant Params& p [[buffer(1)]], constant Palette& pal [[buffer(2)]]) {
    uint x=instance%p.count, shape=instance/p.count;
    uint kind=p.overlay?shape%4+3:shape%7, trace=shape/(p.overlay?4:7);
    Column b=data[x]; float lo=b.low[trace],hi=b.high[trace]; bool active=b.present;
    float top=0,bottom=0; float4 ink=pal.warning; bool contour=kind==3||kind==4;
    if(kind==0) {active&=trace==0&&lo<=p.threshold&&hi>=-p.threshold; top=yFor(min(hi,p.threshold),p);bottom=yFor(max(lo,-p.threshold),p);ink=pal.interior;}
    if(kind==1) {active&=hi>p.threshold;top=yFor(hi,p);bottom=yFor(max(lo,p.threshold),p);}
    if(kind==2) {active&=lo< -p.threshold;top=yFor(min(hi,-p.threshold),p);bottom=yFor(lo,p);}
    if(contour) {
        active&=kind==3||lo!=hi;float c=yFor(kind==3?hi:lo,p),l=c,r=c;
        if(x>0&&data[x-1].present)l=(c+yFor(kind==3?data[x-1].high[trace]:data[x-1].low[trace],p))*0.5f;
        if(x+1<p.count&&data[x+1].present)r=(c+yFor(kind==3?data[x+1].high[trace]:data[x+1].low[trace],p))*0.5f;
        top=min(min(l,c),r)-1.5f*p.scale;bottom=max(max(l,c),r)+1.5f*p.scale;
        ink=pal.colour[trace]*(p.overlay?0.85f:0.95f);
    }
    if(kind==5) {active&=hi>p.range;top=0;bottom=3*p.scale;ink=hi>p.threshold?pal.warning:pal.colour[trace];}
    if(kind==6) {active&=lo< -p.range;top=p.height-3*p.scale;bottom=p.height;ink=lo< -p.threshold?pal.warning:pal.colour[trace];}
    float xx=(x+float(vid&1))*p.xStep+p.xOffset;
    float yy=(vid&2)?min(ceil(bottom),p.height):max(floor(top),0.0f);
    Out o;o.position=active?float4(xx/p.width*2-1,1-yy/p.height*2,0,1):float4(-2,-2,0,1);
    o.colour=ink;o.top=top;o.bottom=bottom;o.contour=contour?1u+((hi>p.threshold||lo< -p.threshold)?2u:0u):0;o.value=0;return o;
}
// Position.y includes the viewport origin; params below carry it for coverage.
struct Origin { float y; };
fragment float4 contourFragment(Out in [[stage_in]], constant Params& p [[buffer(1)]], constant Palette& pal [[buffer(2)]], constant Origin& origin [[buffer(3)]]) {
    if(!in.contour)return in.colour;
    float y=floor(in.position.y-origin.y);
    float coverage=clamp(min(in.bottom,y+1)-max(in.top,y),0.0f,1.0f);
    float v=(0.5f-(in.position.y-origin.y)/p.height)*2*p.range;
    float4 ink=(in.contour&2)&&abs(v)>p.threshold?pal.warning*(p.overlay?0.85f:0.95f):in.colour;
    return ink*coverage;
}
vertex Out rawVertex(uint vid [[vertex_id]], uint instance [[instance_id]],
    device const Point* data [[buffer(0)]], constant Params& p [[buffer(1)]], constant Palette& pal [[buffer(2)]]) {
    uint i=instance%p.count, shape=instance/p.count, trace=shape/3, kind=shape%3;
    Point a=data[i], b=data[min(i+1,p.count-1)];
    bool join=i+1<p.count&&b.x-a.x<=p.xStep*1.01f;
    float2 aa=float2(a.x,yFor(a.values[trace],p)),bb=join?float2(b.x,yFor(b.values[trace],p)):aa+float2(0.5f*p.scale,0);
    if(!join)aa.x-=0.5f*p.scale;
    float2 delta=bb-aa;float2 normal=float2(-delta.y,delta.x)/max(length(delta),0.001f)*1.5f*p.scale;
    float2 xy=((vid&1)?bb:aa)+((vid&2)?normal:-normal);
    bool active=true;
    if(kind!=0) {
        active=kind==1?a.values[trace]>p.range:a.values[trace]< -p.range;
        xy.x=a.x+((vid&1)?0.5f:-0.5f)*p.scale;
        xy.y=kind==1?((vid&2)?3*p.scale:0):(p.height-((vid&2)?0:3*p.scale));
    }
    Out o;o.position=active?float4(xy.x/p.width*2-1,1-xy.y/p.height*2,0,1):float4(-2,-2,0,1);
    o.colour=pal.colour[trace];o.value=kind==0&&(vid&1)&&join?b.values[trace]:a.values[trace];o.top=o.bottom=0;o.contour=0;return o;
}
fragment float4 rawFragment(Out in [[stage_in]], constant Params& p [[buffer(1)]], constant Palette& pal [[buffer(2)]]) {
    return abs(in.value)>p.threshold?pal.warning:in.colour;
}
)METAL";
