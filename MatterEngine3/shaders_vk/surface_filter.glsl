// Shared filtered coverage and shading for raster and ray-query surfaces.
struct Filtered { vec4 premultiplied; vec3 normal; float coverage; };
Filtered sample_mip(uint index,vec2 coord) {
    SurfaceMip mip=surface_mips[index];
    vec2 pos=coord*vec2(mip.shape.xy)-.5;
    ivec2 base=ivec2(floor(pos)); vec2 f=fract(pos);
    Filtered result=Filtered(vec4(0),vec3(0),0);
    for(int y=0;y<2;++y) for(int x=0;x<2;++x) {
        ivec2 p=clamp(base+ivec2(x,y),ivec2(0),ivec2(mip.shape.xy)-1);
        float weight=(x==0?1-f.x:f.x)*(y==0?1-f.y:f.y);
        uvec2 texel=surface_load_texel(mip,p);
        vec4 c=unpackUnorm4x8(texel.x);
        result.premultiplied+=vec4(c.rgb*c.a,c.a)*weight;
        result.normal+=(unpackUnorm4x8(texel.y).xyz*2-1)*c.a*weight;
    }
    result.coverage=result.premultiplied.a*mip.sampling.x;
    return result;
}
