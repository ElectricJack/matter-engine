#ifndef VT_SURFACE_TAPE_GLSL
#define VT_SURFACE_TAPE_GLSL

// vt_surface_tape.glsl — P2 (texel-rate tape): the GPU tape interpreter
// (weight-seam mode 3) and its NOISE TWIN of terrain_field.cpp.
//
// GLSL mirror of MatterEngine3/src/render/vt_surface_tape.h (VtGpuSurfOp,
// kind numbering) and of terrain_field.cpp's noise core (hash2i/hash3i/
// rand01/smooth5/value_noise/value_noise3/fbm2/fbm3 + the fbm3_op warp).
// Keep all three in lockstep.
//
// DETERMINISM (spec §4.4):
//   * hash2i/hash3i are pure uint arithmetic with the exact CPU constants and
//     operation order — bit-exact vs the CPU by construction.
//   * rand01 mirrors the exact CPU conversion ((hash & 0xffffff) / 0x1000000).
//   * The float lerp/fbm chains reproduce the CPU float operation ORDER;
//     `precise` on the accumulators blocks fma contraction from reordering
//     them. Same driver + same inputs => bit-identical pages (the double-fill
//     cmp gate); CPU-vs-GPU equality is tolerance-checked (1e-3), not
//     bit-exact.
//
// Include contract: include vt_chart_types.glsl first (the interpreter reads
// GpuTri lanes). This file declares the tape_ops[] SSBO itself; override
// VT_TAPE_OPS_SET / VT_TAPE_OPS_BINDING before including to relocate it
// (default: set 1 binding 9, the compositor's batch set).

// ---- struct + kind numbering (mirror vt_surface_tape.h exactly) -----------
struct GpuSurfOp {
    uint kind_oct;        // kind | (destination << 8) | (oct << 16) | (warp ? 1u<<31 : 0)
    int  a, b, c;         // register operands; LaneRead: a = lane index
    float f0, f1, f2, f3; // value/freq/gain/lac/edges
    float wf0, wf1;       // warp freq / warp amp
    uint wseed;           // explicit warp seed token
    uint seed;
};

#define VT_SOP_CONST        0u
#define VT_SOP_INPUT        1u
#define VT_SOP_NOISE2       2u
#define VT_SOP_RIDGE2       3u
#define VT_SOP_NOISE2_WORLD 4u
#define VT_SOP_RIDGE2_WORLD 5u
#define VT_SOP_NOISE3       6u
#define VT_SOP_RIDGE3       7u
#define VT_SOP_NOISE3_WORLD 8u
#define VT_SOP_RIDGE3_WORLD 9u
#define VT_SOP_ADD          10u
#define VT_SOP_SUB          11u
#define VT_SOP_MUL          12u
#define VT_SOP_MIN          13u
#define VT_SOP_MAX          14u
#define VT_SOP_CLAMP        15u
#define VT_SOP_BLEND        16u
#define VT_SOP_SMOOTHSTEP   17u
#define VT_SOP_ABS          18u
#define VT_SOP_ONEMINUS     19u
#define VT_SOP_POW          20u
#define VT_SOP_FRACT        21u
#define VT_SOP_LANE_READ    22u
#define VT_SOP_FOOTPRINT    23u
#define VT_SOP_CELL_NOISE2  24u
#define VT_SOP_CELLULAR3    25u

// SurfaceInput codes an Input op can still carry on the GPU (field-derived
// codes were rewritten to LaneRead by the CPU packer). Mirror terrain_field.h.
#define VT_SIN_LX    0
#define VT_SIN_LY    1
#define VT_SIN_LZ    2
#define VT_SIN_NY    3
#define VT_SIN_SLOPE 4
#define VT_SIN_WX    5
#define VT_SIN_WY    6
#define VT_SIN_WZ    7

#define VT_TAPE_MAX_REGS 96
#define VT_TAPE_MAX_SOURCE_OPS 512

// ---- P3 appearance lanes (texel-tape spec section 5) ----------------------
// The clamp ranges and the wetness response mirror terrain_field.h's
// kSurfaceTintMax / kSurfaceRoughBiasLimit / kSurfaceWetAlbedoScale /
// kSurfaceWetRoughness (SurfaceRuntime::appearance_at is the CPU twin).
// VT_APP_NO_REG mirrors vt_surface_tape.h's kVtNoAppearanceReg: registers are
// 0..95, so 0xFF is an unambiguous "directive absent".
#define VT_APP_NO_REG      0xFFu
#define VT_APP_TINT_MAX    2.0
#define VT_APP_ROUGH_LIMIT 0.5
#define VT_APP_WET_ALBEDO  0.55
#define VT_APP_WET_ROUGH   0.08

#ifndef VT_TAPE_OPS_SET
#define VT_TAPE_OPS_SET 1
#endif
#ifndef VT_TAPE_OPS_BINDING
#define VT_TAPE_OPS_BINDING 9
#endif
layout(std430, set = VT_TAPE_OPS_SET, binding = VT_TAPE_OPS_BINDING)
    readonly buffer TapeOpsBuf { GpuSurfOp tape_ops[]; };

// ---- noise twin (terrain_field.cpp anonymous namespace, line for line) ----
//
// Extracted VERBATIM to vt_noise.glsl on 2026-07-31 so vol_density.comp can
// carve its cloud layers with the same vt_fbm3 rather than growing a third
// copy of the engine's value noise. The move is text-identical and the SPIR-V
// this file compiles to is byte-for-byte unchanged by it.
#include "vt_noise.glsl"

// CPU twin: surface_cellular3. Relative coordinates and ordered arithmetic
// preserve site identity at page/mip boundaries, including negative positions.
void vt_cellular3_visit(ivec3 cell, vec3 f, ivec3 offset, uint seed,
                       inout float first, inout float second, inout float value) {
    ivec3 c=cell+offset;
    precise float px=float(offset.x)+vt_rand01_3(c.x,c.y,c.z,seed)-f.x;
    precise float py=float(offset.y)+vt_rand01_3(c.x,c.y,c.z,seed^0x9e37u)-f.y;
    precise float pz=float(offset.z)+vt_rand01_3(c.x,c.y,c.z,seed^0x7f4au)-f.z;
    precise float d=(px*px+py*py)+pz*pz;
    if(d<first) {second=first;first=d;value=vt_rand01_3(c.x,c.y,c.z,seed^0xa511e9b3u);}
    else if(d<second)second=d;
}
vec3 vt_cellular3_values(vec3 p, uint seed) {
    if (!all(greaterThanEqual(p,vec3(-16777216.0))) ||
        !all(lessThan(p,vec3(16777216.0)))) return vec3(0.0);
    ivec3 cell=ivec3(floor(p));vec3 f=p-vec3(cell);
    precise float first=100.0,second=100.0;
    float value=0.0;
    for(int z=-1;z<=1;++z)for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x)
        vt_cellular3_visit(cell,f,ivec3(x,y,z),seed,first,second,value);
    vec3 edge=min(f,1.0-f);
    precise float outside=1.0+min(edge.x,min(edge.y,edge.z));
    if(second>outside*outside) {
        for(int z=-2;z<=2;++z)for(int y=-2;y<=2;++y)for(int x=-2;x<=2;++x)
            if(abs(x)==2 || abs(y)==2 || abs(z)==2)
                vt_cellular3_visit(cell,f,ivec3(x,y,z),seed,first,second,value);
    }
    return vec3(sqrt(first),max(0.0,second-first),value);
}

// fbm3 with the op's optional domain-warp tail (terrain_field fbm3_op): the
// sample point is displaced per axis before the fbm, three axis seeds derived
// from the explicit wseed token (wseed, wseed^0x9e37, wseed^0x7f4a).
float vt_fbm3_op(GpuSurfOp op, float x, float y, float z, bool ridged) {
    if ((op.kind_oct & 0x80000000u) != 0u) {
        precise float sx = x * op.wf0;
        precise float sy = y * op.wf0;
        precise float sz = z * op.wf0;
        x += op.wf1 * (vt_value_noise3(sx, sy, sz, op.wseed)            * 2.0 - 1.0);
        y += op.wf1 * (vt_value_noise3(sx, sy, sz, op.wseed ^ 0x9e37u)  * 2.0 - 1.0);
        z += op.wf1 * (vt_value_noise3(sx, sy, sz, op.wseed ^ 0x7f4au)  * 2.0 - 1.0);
    }
    int oct = int((op.kind_oct >> 16) & 0x7FFFu);
    return vt_fbm3(x, y, z, op.seed, oct, op.f1, op.f2, op.f0, ridged);
}

// ---- per-vertex f16 lane read (mode-3 GpuTri packing) ---------------------
float vt_tape_lane(GpuTri tri, vec3 bary, uint lane) {
    uint word = lane >> 1u;
    uint half_idx = lane & 1u;
    float v0 = unpackHalf2x16(tri.wA[word])[half_idx];
    float v1 = unpackHalf2x16(tri.wB[word])[half_idx];
    float v2 = unpackHalf2x16(tri.wC[word])[half_idx];
    return bary.x * v0 + bary.y * v1 + bary.z * v2;
}

// ---- the interpreter -------------------------------------------------------
// Evaluates ops [ops_offset, ops_offset + ops_count) into regs[]. Inputs:
//   lpos      part-local reconstructed position (lx/ly/lz)
//   ny/slope  from the interpolated (normalized) shading normal
//   wpos      local_to_world * lpos (identity when not world-anchored; the
//             packer pre-resolved world ops for non-anchored parts, so wpos
//             is never read there)
//   tri/bary  for the barycentric field-lane reads.
// Bounded loop, shared opcode dispatch (all threads of a fill run the same
// tape). An operator may still have internally divergent work.
void vt_tape_eval(uint ops_offset, uint ops_count,
                  vec3 lpos, float ny, float slope, vec3 wpos,
                  GpuTri tri, vec3 bary, float footprint_m,
                  out float regs[VT_TAPE_MAX_REGS]) {
    for (int i = 0; i < VT_TAPE_MAX_REGS; ++i) regs[i] = 0.0;
    // One result tuple serves the distance/gap/value ops at identical inputs.
    // Keep this local to one invocation: normal derivatives, other layers and
    // neighboring texels must evaluate their own positions.
    bool have_cellular=false;
    vec3 cellular_position=vec3(0.0),cellular_values=vec3(0.0);
    uint cellular_seed=0u;
    uint count = min(ops_count, uint(VT_TAPE_MAX_SOURCE_OPS));
    for (uint i = 0u; i < count; ++i) {
        GpuSurfOp op = tape_ops[ops_offset + i];
        uint kind = op.kind_oct & 0xFFu;
        int oct = int((op.kind_oct >> 16) & 0x7FFFu);
        float r = 0.0;
        switch (kind) {
        case VT_SOP_CONST:
            r = op.f0;
            break;
        case VT_SOP_FOOTPRINT:
            r = max(footprint_m, 0.0);
            break;
        case VT_SOP_INPUT: {
            // Only local + world POSITION codes reach the GPU (field codes
            // were rewritten to LaneRead; non-anchored world codes to Const).
            switch (oct) {
            case VT_SIN_LX:    r = lpos.x; break;
            case VT_SIN_LY:    r = lpos.y; break;
            case VT_SIN_LZ:    r = lpos.z; break;
            case VT_SIN_NY:    r = ny; break;
            case VT_SIN_SLOPE: r = slope; break;
            case VT_SIN_WX:    r = wpos.x; break;
            case VT_SIN_WY:    r = wpos.y; break;
            case VT_SIN_WZ:    r = wpos.z; break;
            default:           r = 0.0; break;
            }
            break;
        }
        case VT_SOP_NOISE2:
            r = vt_fbm2(lpos.x, lpos.z, op.seed, oct, op.f1, op.f2, op.f0, false);
            break;
        case VT_SOP_RIDGE2:
            r = vt_fbm2(lpos.x, lpos.z, op.seed, oct, op.f1, op.f2, op.f0, true);
            break;
        case VT_SOP_NOISE2_WORLD:
            r = vt_fbm2(wpos.x, wpos.z, op.seed, oct, op.f1, op.f2, op.f0, false);
            break;
        case VT_SOP_RIDGE2_WORLD:
            r = vt_fbm2(wpos.x, wpos.z, op.seed, oct, op.f1, op.f2, op.f0, true);
            break;
        case VT_SOP_NOISE3:
            r = vt_fbm3_op(op, lpos.x, lpos.y, lpos.z, false);
            break;
        case VT_SOP_RIDGE3:
            r = vt_fbm3_op(op, lpos.x, lpos.y, lpos.z, true);
            break;
        case VT_SOP_NOISE3_WORLD:
            r = vt_fbm3_op(op, wpos.x, wpos.y, wpos.z, false);
            break;
        case VT_SOP_RIDGE3_WORLD:
            r = vt_fbm3_op(op, wpos.x, wpos.y, wpos.z, true);
            break;
        case VT_SOP_CELL_NOISE2: {
            float x = regs[op.a], y = regs[op.b];
            r = 0.0;
            if (x >= -16777216.0 && x < 16777216.0 &&
                y >= -16777216.0 && y < 16777216.0)
                r = vt_rand01(int(floor(x)), int(floor(y)), op.seed);
            break;
        }
        case VT_SOP_CELLULAR3: {
            vec3 p=vec3(regs[op.a],regs[op.b],regs[op.c]);
            if(!have_cellular || op.seed!=cellular_seed || any(notEqual(p,cellular_position))) {
                cellular_values=vt_cellular3_values(p,op.seed);
                cellular_position=p;cellular_seed=op.seed;have_cellular=true;
            }
            r=oct>=0 && oct<=2?cellular_values[oct]:0.0;
            break;
        }
        case VT_SOP_ADD:
            r = regs[op.a] + regs[op.b];
            break;
        case VT_SOP_SUB:
            r = regs[op.a] - regs[op.b];
            break;
        case VT_SOP_MUL:
            r = regs[op.a] * regs[op.b];
            break;
        case VT_SOP_MIN:
            r = min(regs[op.a], regs[op.b]);
            break;
        case VT_SOP_MAX:
            r = max(regs[op.a], regs[op.b]);
            break;
        case VT_SOP_CLAMP:
            // CPU: max(f0, min(f1, x)).
            r = max(op.f0, min(op.f1, regs[op.a]));
            break;
        case VT_SOP_BLEND: {
            float t = regs[op.c];
            r = regs[op.a] * (1.0 - t) + regs[op.b] * t;
            break;
        }
        case VT_SOP_SMOOTHSTEP: {
            // Mirror the CPU expression exactly (not GLSL smoothstep, whose
            // edge handling is unspecified for e0 == e1).
            float t = max(0.0, min(1.0, (regs[op.a] - op.f0) / (op.f1 - op.f0)));
            r = t * t * (3.0 - 2.0 * t);
            break;
        }
        case VT_SOP_ABS:
            r = abs(regs[op.a]);
            break;
        case VT_SOP_ONEMINUS:
            r = 1.0 - regs[op.a];
            break;
        case VT_SOP_POW:
            // CPU clamps the base to >= 0 (GLSL pow is undefined below it).
            r = pow(max(regs[op.a], 0.0), op.f0);
            break;
        case VT_SOP_FRACT:
            r = regs[op.a] - floor(regs[op.a]);
            break;
        case VT_SOP_LANE_READ:
            r = vt_tape_lane(tri, bary, uint(op.a));
            break;
        default:
            r = 0.0;
            break;
        }
        regs[(op.kind_oct >> 8) & 0xFFu] = r;
    }
}

// ---- P3: appearance-lane application ---------------------------------------
// Modulates ONE COMPOSITED texel (after the top-2 height blend, before BC
// encode) in the fixed order tint -> roughbias -> wetness -> metallic. `app`
// is the fill request's packed directive registers: x = tint regs
// (r | g << 8 | b << 16), y = roughbias reg, z = wetness reg, w = metallic
// reg — each byte VT_APP_NO_REG when the tape declares no such directive, in
// which case that lane is a no-op and the texel is bit-identical to a tape
// without the directive at all. `regs` is the post-vt_tape_eval register
// file, so this is mode-3 only by construction.
void vt_apply_appearance(uvec4 app, float regs[VT_TAPE_MAX_REGS],
                         inout vec3 albedo, inout vec3 orm) {
    uint tint_regs = app.x;
    if ((tint_regs & 0xFFu) != VT_APP_NO_REG) {
        vec3 tint = vec3(regs[tint_regs & 0xFFu],
                         regs[(tint_regs >> 8) & 0xFFu],
                         regs[(tint_regs >> 16) & 0xFFu]);
        albedo *= clamp(tint, 0.0, VT_APP_TINT_MAX);
    }
    if (app.y != VT_APP_NO_REG) {
        float bias = clamp(regs[app.y], -VT_APP_ROUGH_LIMIT,
                           VT_APP_ROUGH_LIMIT);
        orm.g = clamp(orm.g + bias, 0.0, 1.0);
    }
    if (app.z != VT_APP_NO_REG) {
        float wet = clamp(regs[app.z], 0.0, 1.0);
        albedo *= mix(1.0, VT_APP_WET_ALBEDO, wet);
        orm.g = mix(orm.g, VT_APP_WET_ROUGH, wet);
    }
    // metallic WRITES orm.b (base materials bake metalness 0); [0, 1] clamp
    // mirrors SurfaceRuntime::appearance_at.
    if (app.w != VT_APP_NO_REG) {
        orm.b = clamp(regs[app.w], 0.0, 1.0);
    }
}

// Direct source v1. This output conversion is shared by page composition and
// future layer/splat consumers; normal generation belongs to the receiver.
struct VtSourceSample {
    vec3 albedo;
    vec3 orm;
    float height_m;
    float coverage;
};
float vt_source_value(float value, float lo, float hi, float fallback) {
    return isnan(value) || isinf(value) ? fallback : clamp(value, lo, hi);
}
// Thin coating: preserve substrate relief, AO and normals. Interpolate
// squared perceptual roughness like the structural finite-source compositor.
void vt_apply_coating(uvec4 coat,float regs[VT_TAPE_MAX_REGS],inout vec3 albedo,inout vec3 orm) {
    if(coat.y==VT_APP_NO_REG) return;
    float a=vt_source_value(regs[coat.y],0.,1.,0.);
    if(a<=0.) return;
    vec3 color=vec3(vt_source_value(regs[coat.x&255u],0.,1.,0.),
        vt_source_value(regs[(coat.x>>8)&255u],0.,1.,0.),vt_source_value(regs[(coat.x>>16)&255u],0.,1.,0.));
    float rough=vt_source_value(regs[coat.z],0.,1.,1.);
    albedo=mix(albedo,color,a);
    orm.g=sqrt(mix(orm.g*orm.g,rough*rough,a));
    orm.b*=1.-a; // Paint and ink are dielectric.
}
VtSourceSample vt_source_sample(uvec4 a, uvec4 b, vec2 height_range,
                                float regs[VT_TAPE_MAX_REGS]) {
    VtSourceSample s;
    s.albedo = vec3(vt_source_value(regs[a.x], 0.0, 1.0, 0.0),
                    vt_source_value(regs[a.y], 0.0, 1.0, 0.0),
                    vt_source_value(regs[a.z], 0.0, 1.0, 0.0));
    s.orm = vec3(vt_source_value(regs[b.y], 0.0, 1.0, 1.0),
                 vt_source_value(regs[a.w], 0.0, 1.0, 0.8),
                 vt_source_value(regs[b.x], 0.0, 1.0, 0.0));
    s.height_m = vt_source_value(regs[b.z], height_range.x, height_range.y, height_range.x);
    s.coverage = 1.0;
    return s;
}

#endif  // VT_SURFACE_TAPE_GLSL
