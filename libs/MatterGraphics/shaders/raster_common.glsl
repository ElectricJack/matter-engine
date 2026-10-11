#ifndef MATTER_GRAPHICS_RASTER_COMMON
#define MATTER_GRAPHICS_RASTER_COMMON
#include "material_common.glsl"
struct RasterDraw {
    mat4 model;
    mat4 normal_matrix;
    vec4 card;
    uvec4 ids; // material, identity, atlas+1 or zero, parallax
};
layout(set=0,binding=0,std140) uniform RasterCamera {
    mat4 world_to_clip;
    vec4 eye;
} camera;
layout(set=1,binding=3,std430) readonly buffer RasterDraws { RasterDraw draws[]; };
struct RasterOptions { vec4 sampling; uvec4 flags; };
layout(set=1,binding=4,std430) readonly buffer MaterialOptions { RasterOptions options[]; };
layout(push_constant) uniform RasterPush { uint draw_index; } raster_push;
#endif
