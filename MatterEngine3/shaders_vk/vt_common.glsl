#ifndef VT_COMMON_GLSL
#define VT_COMMON_GLSL

// Chart-space virtual texturing — sampling helper (WP-E, contract C2).
//
// Pass-agnostic on purpose: gbuffer.frag consumes it today, the RT hit shaders
// consume it verbatim (WP-G). It never reads gl_FragCoord, never uses
// dFdx/dFdy internally (the caller supplies the atlas-UV derivatives, which a
// ray hit computes from its cone), and never writes anything except through
// the explicitly-called vt_write_feedback().
//
// Include contract — define BEFORE including:
//   VT_SET                 descriptor set index
//   VT_POOL_BINDING        sampler2DArray vt_pool[4]   (albedo, normal, ORM, aux)
//   VT_INDIRECTION_BINDING readonly storage buffer of packed u32 entries
//   VT_VARIANTS_BINDING    readonly buffer of VtVariantRecord
// Optionally define VT_FEEDBACK_BINDING (a uimage2D) to enable
// vt_write_feedback(); without it the function compiles to a no-op.
//
// Everything below mirrors MatterEngine3/src/render/vt_residency.h. Keep the
// two in lockstep: page geometry, table layout, entry packing.
//
// --- Indirection table layout (mirrors vt_build_layout) ---------------------
// The indirection is ONE u32 storage buffer shared by every (variant, rung);
// each registration owns an exact-sized table at record.table_offset. Inside a
// table the virtual mip grids are concatenated finest-first:
//
//   pw(m) = ceil(max(atlas_w >> m, 1) / 128) = (max(atlas_w >> m, 1) + 127) >> 7
//         = ceil(atlas_w / (128 << m))            (integer-division identity)
//   ph(m) likewise from atlas_h
//   mip_offset[m] = sum_{k < m} pw(k) * ph(k)     (precomputed in the record,
//                                                  so no loop here)
//   entry(m, px, py) = vt_indirection[table_offset + mip_offset[m]
//                                     + py * pw(m) + px]
//
// Entry packing: low 16 bits = physical page slot, high 16 = the mip whose
// page that slot actually holds. Every entry is always valid (worst case the
// variant's pinned tail), so resolution is ONE buffer load plus arithmetic —
// no walk loop, no fault path.

#ifndef VT_SET
#error "vt_common.glsl: define VT_SET before including this file"
#endif
#ifndef VT_POOL_BINDING
#error "vt_common.glsl: define VT_POOL_BINDING before including this file"
#endif
#ifndef VT_INDIRECTION_BINDING
#error "vt_common.glsl: define VT_INDIRECTION_BINDING before including this file"
#endif
#ifndef VT_VARIANTS_BINDING
#error "vt_common.glsl: define VT_VARIANTS_BINDING before including this file"
#endif

// --- geometry (chart_atlas.h / vt_types.h) ---------------------------------
#define VT_PAGE_PAYLOAD      128.0
#define VT_PAGE_BORDER       4.0
#define VT_PAGE_STRIDE       136.0
#define VT_PAGES_PER_EDGE    16u
#define VT_PAGES_PER_LAYER   256u
#define VT_POOL_LAYER_EDGE   2176.0
#define VT_MAX_MIPS          8

#define VT_CHANNEL_ALBEDO 0
#define VT_CHANNEL_NORMAL 1
#define VT_CHANNEL_ORM    2
#define VT_CHANNEL_AUX    3

layout(set = VT_SET, binding = VT_POOL_BINDING)
uniform sampler2DArray vt_pool[4];
layout(set = VT_SET, binding = VT_INDIRECTION_BINDING, std430)
readonly buffer VtIndirection {
    uint vt_indirection[];
};

// Mirrors VtResidency::VariantRecordGpu (vt_residency.h) — append-only, keep
// in lockstep.
struct VtVariantRecord {
    uint atlas_w;
    uint atlas_h;
    uint mip_count;
    uint flags;            // bit0 = valid
    uint mip_offset[VT_MAX_MIPS];   // word offsets of each mip grid, in-table
    uint table_offset;     // word offset of this variant's table in the SSBO
    uint pages_w;          // finest-mip page grid dims (= pw(0)/ph(0))
    uint pages_h;
    uint generation;       // debug correlation only; never read here
};
layout(set = VT_SET, binding = VT_VARIANTS_BINDING, std430)
readonly buffer VtVariants {
    VtVariantRecord vt_variants[];
};

#ifdef VT_FEEDBACK_BINDING
layout(set = VT_SET, binding = VT_FEEDBACK_BINDING, rgba16ui)
uniform writeonly uimage2D vt_feedback;
#endif

// Everything one sample needs, resolved once and shared by all four channels.
struct VtAddress {
    bool  valid;
    uint  layer;           // variant slot index
    uint  desired_mip;     // what the pixel footprint asked for
    uint  mapped_mip;      // what is actually resident
    uint  desired_px;      // page coords at desired_mip (for feedback)
    uint  desired_py;
    vec2  physical_uv;     // normalized coords into the pool layer
    float physical_layer;
};

// Desired virtual mip from atlas-UV derivatives. `duv_dx`/`duv_dy` are the
// screen-space (or cone-footprint) derivatives of the normalized atlas UV.
float vt_desired_mip(uint slot, vec2 duv_dx, vec2 duv_dy) {
    if (slot == 0u) return 0.0;
    VtVariantRecord record = vt_variants[slot - 1u];
    vec2 size = vec2(float(record.atlas_w), float(record.atlas_h));
    vec2 dx = duv_dx * size;
    vec2 dy = duv_dy * size;
    float rho = max(length(dx), length(dy));
    return max(0.0, log2(max(rho, 1e-6)));
}

// Resolves an atlas UV to a physical pool location. `slot` is the transported
// vt slot (variant index + 1); 0 means "no VT" and yields address.valid ==
// false. Never faults: the indirection always carries at least the pinned
// tail entry, so one buffer load is enough — no walk loop, no unbounded work
// in the shader.
VtAddress vt_resolve(uint slot, vec2 atlas_uv, float lod) {
    VtAddress address;
    address.valid = false;
    address.layer = 0u;
    address.desired_mip = 0u;
    address.mapped_mip = 0u;
    address.desired_px = 0u;
    address.desired_py = 0u;
    address.physical_uv = vec2(0.0);
    address.physical_layer = 0.0;
    if (slot == 0u) return address;

    uint layer = slot - 1u;
    address.layer = layer;
    VtVariantRecord record = vt_variants[layer];
    if ((record.flags & 1u) == 0u || record.mip_count == 0u) return address;

    uint max_mip = record.mip_count - 1u;
    uint mip = uint(clamp(floor(lod), 0.0, float(max_mip)));

    // Page coords at the desired mip: pw(m) closed-form from the atlas dims
    // (see the table-layout note at the top of this file).
    vec2 uv = clamp(atlas_uv, vec2(0.0), vec2(0.999999));
    uint aw = max(record.atlas_w >> mip, 1u);
    uint ah = max(record.atlas_h >> mip, 1u);
    uint pages_x = (aw + 127u) >> 7u;
    uint pages_y = (ah + 127u) >> 7u;
    vec2 texel = uv * vec2(float(aw), float(ah));
    uint px = min(uint(floor(texel.x / VT_PAGE_PAYLOAD)), pages_x - 1u);
    uint py = min(uint(floor(texel.y / VT_PAGE_PAYLOAD)), pages_y - 1u);
    address.desired_mip = mip;
    address.desired_px = px;
    address.desired_py = py;

    // Indirection fetch: one load from the variant's exact-sized table.
    uint entry_index =
        record.table_offset + record.mip_offset[mip] + py * pages_x + px;
    uint entry = vt_indirection[entry_index];
    uint physical_slot = entry & 0xFFFFu;
    uint mapped_mip = min(entry >> 16u, max_mip);
    address.mapped_mip = mapped_mip;

    // Recompute the in-page position at the mip that is actually resident.
    uint maw = max(record.atlas_w >> mapped_mip, 1u);
    uint mah = max(record.atlas_h >> mapped_mip, 1u);
    uint mpx = (maw + 127u) >> 7u;
    uint mpy = (mah + 127u) >> 7u;
    vec2 mtexel = uv * vec2(float(maw), float(mah));
    float mpage_x = min(floor(mtexel.x / VT_PAGE_PAYLOAD), float(mpx - 1u));
    float mpage_y = min(floor(mtexel.y / VT_PAGE_PAYLOAD), float(mpy - 1u));
    float fx = clamp(mtexel.x - mpage_x * VT_PAGE_PAYLOAD, 0.0, VT_PAGE_PAYLOAD);
    float fy = clamp(mtexel.y - mpage_y * VT_PAGE_PAYLOAD, 0.0, VT_PAGE_PAYLOAD);

    // Physical slot -> pool layer + page origin, then payload offset.
    uint pool_layer = physical_slot / VT_PAGES_PER_LAYER;
    uint local = physical_slot % VT_PAGES_PER_LAYER;
    float ox = float(local % VT_PAGES_PER_EDGE) * VT_PAGE_STRIDE + VT_PAGE_BORDER;
    float oy = float(local / VT_PAGES_PER_EDGE) * VT_PAGE_STRIDE + VT_PAGE_BORDER;
    address.physical_uv = (vec2(ox, oy) + vec2(fx, fy)) / VT_POOL_LAYER_EDGE;
    address.physical_layer = float(pool_layer);
    address.valid = true;
    return address;
}

vec4 vt_sample_channel(VtAddress address, int channel) {
    if (!address.valid) return vec4(0.0);
    return textureLod(vt_pool[channel],
                      vec3(address.physical_uv, address.physical_layer), 0.0);
}

// Tangent-space normal relative to the interpolated geometric normal (BC5
// stores XY; Z is reconstructed). Rotate into world space with
// tileset_rotate_normal(decoded, geometric_normal) — the compositor encodes
// in exactly that function's frame.
vec3 vt_decode_normal(vec4 encoded) {
    vec2 xy = encoded.xy * 2.0 - 1.0;
    float z = sqrt(max(0.0, 1.0 - dot(xy, xy)));
    return vec3(xy, z);
}

// Writes one feedback request per 8x8 screen block. `pixel` is the integer
// screen coordinate of the fragment. Last writer wins within a block — the
// request is a hint, not a contract, so the race is benign and deliberate.
void vt_write_feedback(VtAddress address, ivec2 pixel) {
#ifdef VT_FEEDBACK_BINDING
    if (!address.valid) return;
    if ((pixel.x & 7) != 0 || (pixel.y & 7) != 0) return;
    imageStore(vt_feedback, pixel >> 3,
               uvec4(address.layer + 1u, address.desired_px,
                     address.desired_py, address.desired_mip));
#endif
}

#endif  // VT_COMMON_GLSL
