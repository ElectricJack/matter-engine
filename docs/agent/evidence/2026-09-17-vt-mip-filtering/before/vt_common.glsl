#ifndef VT_COMMON_GLSL
#define VT_COMMON_GLSL

// Chart-space virtual texturing — sampling helper (WP-E, contract C2).
//
// Pass-agnostic on purpose: gbuffer.frag consumes it today, the RT hit shaders
// consume it verbatim (WP-G). It never reads gl_FragCoord, never uses
// dFdx/dFdy internally (the caller supplies the atlas-UV derivatives, which a
// ray hit computes from its cone), and never writes storage resources.
//
// Include contract — define BEFORE including:
//   VT_SET                 descriptor set index
//   VT_POOL_BINDING        sampler2DArray vt_pool[5]   (albedo, normal, ORM, aux, height)
//   VT_INDIRECTION_BINDING readonly storage buffer of packed u32 entries
//   VT_VARIANTS_BINDING    readonly buffer of VtVariantRecord
// Raster callers output vt_feedback_request() through a depth-tested integer
// color attachment. RT callers only sample the existing pages.
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
// Entry packing: low 16 bits = receiver page slot, high 16 = the mip whose
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
#define VT_CHANNEL_HEIGHT 4

// THE ARRAY LENGTH MUST EQUAL vt::kVtChannelCount. The C++ side sizes its
// descriptor array from that constant (vk_scene_renderer.cpp binds
// descriptorCount = kVtChannelCount at bindings 10 and 17), so a shader that
// declares fewer is a descriptor-count mismatch -- a validation error at best
// and a read of an unwritten binding at worst.
layout(set = VT_SET, binding = VT_POOL_BINDING)
uniform sampler2DArray vt_pool[5];
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
    uint generation;       // publication token; lifetime still belongs to CPU
};
layout(set = VT_SET, binding = VT_VARIANTS_BINDING, std430)
readonly buffer VtVariants {
    VtVariantRecord vt_variants[];
};


#ifndef VT_INPUT_BINDING
#error "vt_common.glsl: define VT_INPUT_BINDING before including this file"
#endif
// Buffer references require GL_EXT_buffer_reference2 and uvec2 in callers.
// The mapping table and geometry are immutable, retained by page publication.
struct VtPageMetadata {
    uvec4 inputs; // input bank, min/range float bits, height version
    uvec2 charts;
    uvec2 triangles;
    uvec4 geometry_counts; // chart count, triangle count, page flags, reserved
    uvec4 material; // pixel slot, mapping BDA low/high, chart count
    uvec4 occlusion; // immutable packed R16 receiver factor BDA, reserved
};
layout(set = VT_SET, binding = VT_INPUT_BINDING, std430)
readonly buffer VtPageInputs { VtPageMetadata vt_page_inputs[]; };

struct VtReceiverMaterial {
    uvec4 binding;
    vec4 uv_u, uv_v, normal_xy, metrics;
};
layout(buffer_reference, std430, buffer_reference_align = 16)
readonly buffer VtReceiverMaterials { VtReceiverMaterial values[]; };

// Everything one sample needs, resolved once and shared by all five channels.
struct VtAddress {
    bool  valid;
    uint  layer;           // variant slot index
    uint  desired_mip;     // what the pixel footprint asked for
    uint  mapped_mip;      // what is actually resident
    uint  desired_px;      // page coords at desired_mip (for feedback)
    uint  desired_py;
    vec2  physical_uv;     // receiver AUX coordinates into the pool layer
    float physical_layer;
    uint input_snapshot; // bounded immutable draw-input bank; UINT_MAX is legacy
    vec2 height_decode;  // minimum and range in local metres
    uint height_version;
    uint physical_slot;
    vec2 material_uv;      // color/normal/ORM/height may be shared by receivers
    float material_layer;
    uvec4 module_request;
    uint material_chart; // UINT_MAX if no published mapping table applies
    uint material_mapped_mip;
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
    // Preserve sub-texel footprints until the independent module transform.
    // Each actual lookup clamps its own mip after applying that transform.
    return log2(max(rho, 1e-6));
}

// Resolves an atlas UV to a physical pool location. `slot` is the transported
// vt slot (variant index + 1); 0 means "no VT" and yields address.valid ==
// false. Never faults: the indirection always carries at least the pinned
// tail entry. Finite receivers use one load. Periodic domains additionally
// resolve the actual parent coordinates when falling back across odd mip sizes.
VtAddress vt_resolve_address(uint slot, vec2 atlas_uv, float lod, bool periodic) {
    VtAddress address;
    address.valid = false;
    address.input_snapshot = 0xFFFFFFFFu;
    address.height_decode = vec2(0.0);
    address.module_request = uvec4(0);
    address.material_chart = 0xffffffffu;
    address.material_mapped_mip = 0u;
    address.height_version = 0u;
    address.physical_slot = 0u;
    address.layer = 0u;
    address.desired_mip = 0u;
    address.mapped_mip = 0u;
    address.desired_px = 0u;
    address.desired_py = 0u;
    address.physical_uv = vec2(0.0);
    address.physical_layer = 0.0;
    address.material_uv = vec2(0.0);
    address.material_layer = 0.0;
    if (slot == 0u) return address;

    uint layer = slot - 1u;
    address.layer = layer;
    VtVariantRecord record = vt_variants[layer];
    if ((record.flags & 1u) == 0u || record.mip_count == 0u) return address;

    uint max_mip = record.mip_count - 1u;
    uint mip = uint(clamp(floor(lod), 0.0, float(max_mip)));

    // Page coords at the desired mip: pw(m) closed-form from the atlas dims
    // (see the table-layout note at the top of this file).
    // Periodic material domains wrap in their own logical dimensions, before
    // either page lookup. Keep derivatives outside this operation. In
    // particular, do not clamp the last fraction of a periodic texel away.
    vec2 uv = periodic ? fract(atlas_uv) : clamp(atlas_uv, vec2(0.0), vec2(0.999999));
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
    address.physical_slot = physical_slot;
    uvec4 page = vt_page_inputs[physical_slot].inputs;
    address.input_snapshot = page.x;
    address.height_decode = uintBitsToFloat(page.yz);
    address.height_version = page.w;
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
    uint material_slot = vt_page_inputs[physical_slot].material.x;
    uint material_local = material_slot % VT_PAGES_PER_LAYER;
    vec2 material_origin = vec2(material_local % VT_PAGES_PER_EDGE,
                                material_local / VT_PAGES_PER_EDGE) * VT_PAGE_STRIDE + VT_PAGE_BORDER;
    address.material_uv = (material_origin + vec2(fx, fy)) / VT_POOL_LAYER_EDGE;
    address.material_layer = float(material_slot / VT_PAGES_PER_LAYER);
    address.valid = true;
    return address;
}

// Periodic resolution is raw: module pages never recursively bind receivers.
VtAddress vt_resolve_periodic(uint slot, vec2 module_uv, float lod) {
    VtAddress address=vt_resolve_address(slot,module_uv,lod,true);
    if (!address.valid) return address;
    uvec3 desired=uvec3(address.desired_mip,address.desired_px,address.desired_py);
    uint lookup=address.desired_mip;
    // CPU page ancestry can straddle an NPOT parent cut. Follow the actual
    // normalized coordinate at each fallback mip. Keep this loop out of the
    // finite receiver resolver, which is inlined throughout raster/RT POM.
    for (uint hop=0u;hop<VT_MAX_MIPS;++hop) {
        if (address.mapped_mip==lookup) break;
        if (address.mapped_mip<lookup) {address.valid=false;return address;}
        lookup=address.mapped_mip;
        address=vt_resolve_address(slot,module_uv,float(lookup),true);
        if (!address.valid) return address;
    }
    address.desired_mip=desired.x;address.desired_px=desired.y;address.desired_py=desired.z;
    return address;
}

// Coverage-only pages own no material slot. Published modules are retained and
// tail-ready, but malformed/stale bindings must still never read unwritten
// material memory. Every receiver tail keeps a complete finite fallback.
VtAddress vt_finite_material_fallback(VtAddress receiver,vec2 uv) {
    if(!receiver.valid || (vt_page_inputs[receiver.physical_slot].geometry_counts.z&1u)==0u) return receiver;
    VtVariantRecord owner=vt_variants[receiver.layer];
    VtAddress tail=vt_resolve_address(receiver.layer+1u,uv,float(owner.mip_count-1u),false);
    if(!tail.valid || (vt_page_inputs[tail.physical_slot].geometry_counts.z&1u)!=0u) {
        receiver.valid=false;return receiver;
    }
    // Use one complete finite page: keeping fine AUX/geometry with coarse
    // material would falsely validate height filtering at the finer footprint.
    // Preserve demand so recovery still asks for the originally desired page.
    tail.desired_mip=receiver.desired_mip;
    tail.desired_px=receiver.desired_px;tail.desired_py=receiver.desired_py;
    return tail;
}

VtAddress vt_apply_receiver_material(VtAddress receiver, vec2 uv, float lod, uint chart) {
    if (!receiver.valid) return receiver;
    uvec4 metadata=vt_page_inputs[receiver.physical_slot].material;
    if (chart>=metadata.w || all(equal(metadata.yz,uvec2(0)))) return vt_finite_material_fallback(receiver,uv);
    VtReceiverMaterial mapping=VtReceiverMaterials(metadata.yz).values[chart];
    // Unmapped charts use the same shell envelope, with their finite decode.
    receiver.material_chart=chart;
    uint slot=mapping.binding.x;
    if (slot==0u || slot>uint(vt_variants.length())) return vt_finite_material_fallback(receiver,uv);
    VtVariantRecord record=vt_variants[slot-1u];
    if (record.generation!=mapping.binding.y || (record.flags&1u)==0u) return vt_finite_material_fallback(receiver,uv);
    vec2 module_uv=vec2(dot(mapping.uv_u.xyz,vec3(uv,1)),dot(mapping.uv_v.xyz,vec3(uv,1)));
    // Finite end treatments keep their complete receiver material. Bounds
    // are unwrapped, so repeating the module never repeats the finite mask.
    vec2 interval=uintBitsToFloat(mapping.binding.zw);
    if (module_uv.x<interval.x || module_uv.x>interval.y) return vt_finite_material_fallback(receiver,uv);
    VtAddress material=vt_resolve_periodic(slot,module_uv,lod+mapping.uv_u.w);
    if (!material.valid || material.height_version!=1u) return vt_finite_material_fallback(receiver,uv);
    receiver.material_uv=material.material_uv;
    receiver.material_layer=material.material_layer;
    receiver.height_decode=material.height_decode+vec2(mapping.uv_v.w,0);
    receiver.height_version=material.height_version;
    receiver.module_request=uvec4(slot,material.desired_px,material.desired_py,material.desired_mip);
    receiver.material_mapped_mip=material.mapped_mip;
    return receiver;
}

// Coverage and geometry stay finite. Only material coordinates repeat.
VtAddress vt_resolve(uint slot, vec2 atlas_uv, float lod) {
    VtAddress address=vt_resolve_address(slot,atlas_uv,lod,false);
    if (!address.valid) return address;
    if (vt_page_inputs[address.physical_slot].material.w==0u) return vt_finite_material_fallback(address,atlas_uv);
    ivec2 size=textureSize(vt_pool[VT_CHANNEL_AUX],0).xy;
    uvec4 aux=uvec4(texelFetch(vt_pool[VT_CHANNEL_AUX],
        ivec3(ivec2(floor(address.physical_uv*vec2(size))),int(address.physical_layer)),0)*255.0+.5);
    if (aux.a<2u || aux.a>4u) return vt_finite_material_fallback(address,atlas_uv);
    return vt_apply_receiver_material(address,atlas_uv,lod,aux.g|(aux.b<<8u));
}
// A geometry walk has already proved the chart; its identity takes precedence
// over a coarse/dilated categorical texel next to a physical edge.
VtAddress vt_resolve_chart(uint slot,vec2 uv,float lod,uint chart) {
    return vt_apply_receiver_material(vt_resolve_address(slot,uv,lod,false),uv,lod,chart);
}

VtReceiverMaterial vt_receiver_mapping(VtAddress address) {
    return VtReceiverMaterials(vt_page_inputs[address.physical_slot].material.yz).values[address.material_chart];
}
vec2 vt_height_envelope(VtAddress address) {
    return address.material_chart==0xffffffffu ? address.height_decode : vt_receiver_mapping(address).metrics.zw;
}
vec4 vt_material_gradient(VtAddress address) {
    if (address.module_request.x==0u) return vec4(0);
    VtReceiverMaterial mapping=vt_receiver_mapping(address);
    VtVariantRecord record=vt_variants[address.module_request.x-1u];
    vec2 size=vec2(max(uvec2(record.atlas_w,record.atlas_h)>>address.module_request.w,uvec2(1)));
    return vec4(mapping.uv_u.xy*size.x,mapping.uv_v.xy*size.y);
}
float vt_material_texel_m(VtAddress address) {
    if (address.module_request.x==0u) return 0.0;
    VtVariantRecord record=vt_variants[address.module_request.x-1u];
    vec2 size=vec2(max(uvec2(record.atlas_w,record.atlas_h)>>address.material_mapped_mip,uvec2(1)));
    vec2 period=vt_receiver_mapping(address).metrics.xy;
    return max(period.x/size.x,period.y/size.y);
}
vec4 vt_transform_material_normal(VtAddress address,vec4 value) {
    if (address.module_request.x==0u) return value;
    vec2 xy=value.xy*2.0-1.0;
    vec4 transform=vt_receiver_mapping(address).normal_xy;
    value.xy=vec2(dot(transform.xy,xy),dot(transform.zw,xy))*.5+.5;
    return value;
}

// Keep categorical, raw material and normal reads separate. Besides avoiding
// unnecessary work, this prevents the RT compiler's exhaustive inliner from
// cloning normal-transform/table code into every AUX/height-only POM query.
vec4 vt_sample_aux(VtAddress address) {
    if (!address.valid) return vec4(0);
    ivec2 size=textureSize(vt_pool[VT_CHANNEL_AUX],0).xy;
    return texelFetch(vt_pool[VT_CHANNEL_AUX],
        ivec3(ivec2(floor(address.physical_uv*vec2(size))),int(address.physical_layer)),0);
}
layout(buffer_reference,std430,buffer_reference_align=16) readonly buffer VtReceiverOcclusion {uint values[];};
float vt_occlusion_texel(uvec2 pointer,ivec2 p) {
    uint texel=uint(p.y)*136u+uint(p.x);
    return float((VtReceiverOcclusion(pointer).values[texel>>1u]>>((texel&1u)*16u))&65535u)/65535.;
}
float vt_sample_occlusion(VtAddress address) {
    if(!address.valid)return 1.;
    uvec2 pointer=vt_page_inputs[address.physical_slot].occlusion.xy;
    if(all(equal(pointer,uvec2(0))))return 1.;
    uint local=address.physical_slot%VT_PAGES_PER_LAYER;
    vec2 origin=vec2(local%VT_PAGES_PER_EDGE,local/VT_PAGES_PER_EDGE)*VT_PAGE_STRIDE;
    vec2 grid=clamp(address.physical_uv*VT_POOL_LAYER_EDGE-origin-.5,vec2(0),vec2(135));
    ivec2 p=ivec2(floor(grid)),q=min(p+ivec2(1),ivec2(135));vec2 f=fract(grid);
    return mix(mix(vt_occlusion_texel(pointer,p),vt_occlusion_texel(pointer,ivec2(q.x,p.y)),f.x),
        mix(vt_occlusion_texel(pointer,ivec2(p.x,q.y)),vt_occlusion_texel(pointer,q),f.x),f.y);
}
vec4 vt_sample_material(VtAddress address,int channel) {
    if (!address.valid) return vec4(0);
    return textureLod(vt_pool[channel],vec3(address.material_uv,address.material_layer),0.0);
}
vec4 vt_sample_orm(VtAddress address) {
    vec4 value=vt_sample_material(address,VT_CHANNEL_ORM);
    value.r*=vt_sample_occlusion(address);
    return value;
}
vec4 vt_sample_normal(VtAddress address) {
    return vt_transform_material_normal(address,vt_sample_material(address,VT_CHANNEL_NORMAL));
}
vec4 vt_sample_channel(VtAddress address,int channel) {
    if (channel==VT_CHANNEL_AUX) return vt_sample_aux(address);
    if (channel==VT_CHANNEL_NORMAL) return vt_sample_normal(address);
    if (channel==VT_CHANNEL_ORM) return vt_sample_orm(address);
    return vt_sample_material(address,channel);
}

// Metre decode stays with the physical page when a material's range is edited.
float vt_sample_height_m(VtAddress address) {
    if (!address.valid || address.height_version != 1u) return 0.0;
    return address.height_decode.x + address.height_decode.y *
           vt_sample_material(address, VT_CHANNEL_HEIGHT).r;
}

// AUX A: 255 legacy; 1 flat direct; 2 ordinary interior; 3 dilation; 4 crease interior.
// The page's tag, rather than mutable material flags, selects the shading route.
bool vt_is_direct_source(VtAddress address) {
    if (!address.valid) return false;
    uint tag = uint(vt_sample_aux(address).a * 255.0 + 0.5);
    return tag >= 1u && tag <= 4u;
}

// Tangent-space normal relative to the interpolated geometric normal (BC5
// stores XY; Z is reconstructed). Rotate into world space with
// vt_frame_decode(decoded, object_normal), followed by the instance normal
// transform. Encoding and decoding share vt_normal_frame.glsl.
vec3 vt_decode_normal(vec4 encoded) {
    vec2 xy = encoded.xy * 2.0 - 1.0;
    float z = sqrt(max(0.0, 1.0 - dot(xy, xy)));
    return vec3(xy, z);
}

// Preserve the existing four-u16 request encoding. Visibility is resolved by
// the G-buffer depth test before vt_feedback.comp selects the 8x8 sample grid.
uvec4 vt_feedback_request(VtAddress address) {
    if (!address.valid) return uvec4(0u);
    return uvec4(address.layer + 1u, address.desired_px,
                 address.desired_py, address.desired_mip);
}

#endif  // VT_COMMON_GLSL
