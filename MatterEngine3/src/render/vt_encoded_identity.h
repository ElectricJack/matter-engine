#pragma once
#include "vt_encoded_pages.h"
#include "vt_snapshot.h"

namespace vt::encoded {
// The receiver digest includes placement; compositor input identity includes
// material/source pixels and producer policy. Keep this domain separate from
// the lower-level four-component identity utility.
inline asset_store::BlobHash page_content_key(asset_store::BlobHash receiver,
                                               asset_store::BlobHash inputs) {
    if (!receiver.valid() || !inputs.valid()) return {};
    uint8_t bytes[40]{};
    asset_store::put_u32(bytes, kKind); asset_store::put_u32(bytes+4, 2);
    asset_store::put_u64(bytes+8, receiver.lo); asset_store::put_u64(bytes+16, receiver.hi);
    asset_store::put_u64(bytes+24, inputs.lo); asset_store::put_u64(bytes+32, inputs.hi);
    return asset_store::hash_bytes(bytes, sizeof(bytes));
}
// Compute once per immutable snapshot on a preparation/streaming worker, never
// per page or per frame. Large arrays are hashed in place; only small scalar
// and array-digest records are assembled. No pointers or process IDs participate.
inline asset_store::BlobHash receiver_key(const VtPartSnapshot& snapshot) {
    if (!snapshot.owns_context_inputs()) return {};
    // The source arrays and GPU scalar ABI are little-endian IEEE binary32.
    // Refuse rather than silently share a key with a different native ABI.
    const uint32_t endian = 1;
    if (*reinterpret_cast<const uint8_t*>(&endian) != 1 || sizeof(float) != 4) return {};
    std::vector<uint8_t> record;
    const auto word = [&](uint32_t value) { asset_store::push_u32(record, value); };
    const auto wide = [&](uint64_t value) { asset_store::push_u64(record, value); };
    const auto bytes = [&](const void* data, size_t count) {
        wide(count);
        const auto digest = asset_store::hash_bytes(data, count);
        wide(digest.lo); wide(digest.hi);
    };
    const auto array = [&](const auto& values) {
        word(sizeof(values[0])); bytes(values.data(), values.size()*sizeof(values[0]));
    };
    const auto floats = [&](const float* values, size_t count) { bytes(values, count*sizeof(float)); };
    word(kKind); word(kVersion); word(1); // receiver-key schema
    const auto& g = *snapshot.geometry;
    const auto& s = *snapshot.surface;
    const auto& c = snapshot.context;
    word(c.vertex_count); word(c.triangle_count); word(c.dominant_material);
    word(c.material_count); word(c.material_stride);
    word(g.atlas.atlas_w); word(g.atlas.atlas_h); wide(g.atlas.charts.size());
    for (const auto& chart : g.atlas.charts) {
        floats(chart.origin, 3); floats(chart.tangent, 3); floats(chart.bitangent, 3);
        word(chart.rect_x); word(chart.rect_y); word(chart.rect_w); word(chart.rect_h);
        floats(&chart.texels_per_meter, 1); word(chart.first_tri); word(chart.tri_count);
    }
    // Unlike a parameterisation-sharing key, exact baked pixels depend on
    // triangle ordering and all interpolation streams, including coverage IDs.
    array(g.atlas.tri_order); array(g.positions); array(g.normals); array(g.surface_uvs);
    array(g.material_table); array(g.material_ids); array(g.indices); array(g.tint_rgba);
    array(s.weights); array(s.materials); array(s.lanes); array(s.finite_source_ids);
    word(s.has_tape_text ? 1 : 0); bytes(s.tape_text.data(), s.tape_text.size());
    wide(s.tape_hash); word(s.lane_count); word(c.surface_material_count); word(c.surface_lane_count);
    word(c.surface_world_anchored); floats(c.surface_local_to_world, 12);
    word(c.periodic.version); word(c.periodic.width); word(c.periodic.height);
    floats(c.periodic.origin, 3); floats(c.periodic.u, 3); floats(c.periodic.v, 3);
    floats(c.periodic.n, 3); floats(c.periodic.period, 2);
    word(s.finite_sources ? 1 : 0);
    if (s.finite_sources) {
        // The validated finite-source builder hashes bindings, lookup and all
        // immutable payload pixels. An unversioned external catalog is a miss.
        if (!s.finite_sources->content_hash) return {};
        wide(s.finite_sources->content_hash); wide(s.finite_sources->payload_hash);
    }
    return asset_store::hash_bytes(record.data(), record.size());
}
} // namespace vt::encoded
