#ifndef FINITE_SURFACE_STAMP_GLSL
#define FINITE_SURFACE_STAMP_GLSL
// Binding numbers are supplied by the consumer. Identical finite-source
// semantics serve base materials and splats; neither caller wraps UVs.
struct StampChannels {
    vec4 albedo_coverage, orm_height, normal_detail, geometric_reserved;
};
layout(std430,set=STAMP_SET,binding=STAMP_PIXELS_BINDING) readonly buffer StampPixels {
    StampChannels stamp_pixels[];
};
layout(std430,set=STAMP_SET,binding=STAMP_LEVELS_BINDING) readonly buffer StampLevels {
    uvec4 stamp_levels[]; // offset, width, height, zero
};
StampChannels stamp_zero() {
    StampChannels r;
    r.albedo_coverage=r.orm_height=r.normal_detail=r.geometric_reserved=vec4(0);
    return r;
}
void stamp_accumulate(inout StampChannels a,StampChannels b,float w) {
    a.albedo_coverage+=b.albedo_coverage*w;
    a.orm_height+=b.orm_height*w;
    a.normal_detail+=b.normal_detail*w;
    a.geometric_reserved+=b.geometric_reserved*w;
}
StampChannels stamp_filtered_level(uint level,vec2 uv,vec2 domain_span,float footprint) {
    uvec4 m=stamp_levels[level];
    vec2 size=vec2(m.yz);
    vec2 half_width=.5*max(1./size,footprint/domain_span);
    StampChannels r=stamp_zero();
    if (any(isinf(half_width))) return r;
    vec2 lo=max(vec2(0),(uv-half_width)*size),hi=min(size,(uv+half_width)*size);
    if (any(lessThanEqual(hi,lo))) return r;
    uvec2 end=uvec2(ceil(hi));
    for (uint y=uint(lo.y);y<end.y;++y) for (uint x=uint(lo.x);x<end.x;++x) {
        vec2 p=vec2(x,y);
        vec2 weights=(min(hi,p+1)-max(lo,p))/(2*half_width*size);
        stamp_accumulate(r,stamp_pixels[m.x+y*m.y+x],weights.x*weights.y);
    }
    return r;
}
vec3 stamp_unit(vec3 n) {
    float d=dot(n,n);
    return d<1e-20 ? vec3(0,0,1) : n*inversesqrt(d);
}
// The prepared descriptor/indices must have passed CPU admission. The sample
// itself is total for nonfinite or far-outside query coordinates/footprints.
StampChannels sample_finite_stamp(uint first_level,uint level_count,vec4 domain,
                                  vec2 uv_m,float footprint_m) {
    StampChannels r=stamp_zero();
    if (level_count==0u || any(isnan(uv_m)) || any(isinf(uv_m)) ||
        isnan(footprint_m) || isinf(footprint_m) || footprint_m<0) return r;
    vec2 uv=(uv_m-domain.xy)/domain.zw;
    if (any(isnan(uv)) || any(isinf(uv))) return r;
    vec2 pitches=domain.zw/vec2(stamp_levels[first_level].yz);
    float lod=min(float(level_count-1u),log2(max(1.,footprint_m/min(pitches.x,pitches.y))));
    uint lo=uint(lod),hi=min(lo+1u,level_count-1u);
    float f=lod-float(lo);
    stamp_accumulate(r,stamp_filtered_level(first_level+lo,uv,domain.zw,footprint_m),1-f);
    if (f>0) stamp_accumulate(r,stamp_filtered_level(first_level+hi,uv,domain.zw,footprint_m),f);
    float coverage=r.albedo_coverage.a;
    if (coverage<=0) return stamp_zero();
    r.albedo_coverage.rgb/=coverage;
    r.orm_height/=coverage; r.normal_detail/=coverage; r.geometric_reserved/=coverage;
    r.orm_height.g=sqrt(max(0.,r.orm_height.g));
    r.normal_detail.xyz=stamp_unit(r.normal_detail.xyz);
    r.geometric_reserved.xyz=stamp_unit(r.geometric_reserved.xyz);
    return r;
}
#endif
