// Object-space textured triangles share root selection/lifetime with sparse
// bricks. The first primitive/count fields of a surface level address these
// triangles instead of bricks; firstVertex still encodes the level in /6.
struct SurfaceVertex { vec4 position_uvx; vec4 normal_uvy; vec4 color_texture; };
struct SurfaceTriangle { SurfaceVertex vertices[3]; uvec4 projection; };
struct SurfaceMip { uvec4 shape; vec4 sampling; uvec4 pages; }; // width, height, texel offset, mip count; alpha scale
layout(std430,set=0,binding=7) readonly buffer Surfaces { SurfaceTriangle surfaces[]; };
layout(std430,set=0,binding=8) readonly buffer SurfaceMips { SurfaceMip surface_mips[]; };
layout(std430,set=0,binding=9) readonly buffer SurfaceTexels { uvec2 surface_texels[]; };

struct SurfaceRoot { vec4 sphere; vec4 lod_sphere; uvec4 output_range; };
layout(std430,set=0,binding=4) readonly buffer SurfaceRoots { SurfaceRoot surface_roots[]; };
layout(std430,set=0,binding=10) readonly buffer SurfaceTexturePages { uint surface_texture_pages[]; };
uvec2 surface_load_texel(SurfaceMip mip,ivec2 position) {
    if(mip.pages.w==0u) return surface_texels[mip.shape.z+uint(position.y)*mip.shape.x+uint(position.x)];
    uvec2 p=uvec2(position);
    uint page=surface_texture_pages[mip.pages.x+(p.y/8u)*mip.pages.y+p.x/8u];
    if(page==0u) return uvec2(0);
    uint address=mip.pages.z+(page-1u)*3u;
    uint bit=(p.y%8u)*8u+p.x%8u,shift=bit&31u;
    uint lower=surface_texture_pages[address],upper=surface_texture_pages[address+1u];
    uint word=bit<32u?lower:upper;
    if((word&(1u<<shift))==0u) return uvec2(0);
    uint rank=uint(bitCount(word&((1u<<shift)-1u)))+(bit<32u?0u:uint(bitCount(lower)));
    return surface_texels[mip.shape.z+surface_texture_pages[address+2u]+rank];
}
