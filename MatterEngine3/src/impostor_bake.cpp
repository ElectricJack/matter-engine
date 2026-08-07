#include "impostor_bake.h"
#include "part_bundle.h"   // M4: the atlas is a bundle section

#include "part_asset.h"   // part_asset::fnv1a64, replace_file_atomic lives in v2
#include "part_asset_v2.h"
#include "material_registry.h"   // MaterialDef::albedo -- the VT path's albedo

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace impostor {
namespace {

// ---------------------------------------------------------------------------
// Small deterministic vector helpers. Deliberately NOT precomp.h's normalize(),
// which goes through rsqrtf: an approximate reciprocal square root is the one
// operation in this file whose result could plausibly differ between builds,
// and the atlas has to be byte-reproducible.
inline float dot3(const float3& a, const float3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline float3 cross3(const float3& a, const float3& b) {
    return make_float3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
                       a.x * b.y - a.y * b.x);
}
inline float3 unit(const float3& v) {
    const float len = std::sqrt(dot3(v, v));
    if (!(len > 1e-20f)) return make_float3(0.0f, 0.0f, 1.0f);
    return make_float3(v.x / len, v.y / len, v.z / len);
}

// Octahedral encode of a unit vector into [0,1]^2, the inverse of the decode
// warp_field/raster.vert already carry (same branch structure, same wrap).
inline void oct_encode(const float3& n_in, float& ox, float& oy) {
    const float3 n = unit(n_in);
    const float l1 = std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z);
    float ex = 0.0f, ey = 0.0f;
    if (l1 > 1e-20f) { ex = n.x / l1; ey = n.y / l1; }
    if (n.z < 0.0f) {
        const float sx = ex >= 0.0f ? 1.0f : -1.0f;
        const float sy = ey >= 0.0f ? 1.0f : -1.0f;
        const float tx = (1.0f - std::fabs(ey)) * sx;
        const float ty = (1.0f - std::fabs(ex)) * sy;
        ex = tx; ey = ty;
    }
    ox = ex * 0.5f + 0.5f;
    oy = ey * 0.5f + 0.5f;
}

inline uint8_t to_u8(float v) {
    const float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return static_cast<uint8_t>(c * 255.0f + 0.5f);
}

// Octahedral decode, the inverse of oct_encode above and the same branch
// structure as gbuffer.frag's decode. Used only by the edge padding, which
// averages neighbour normals in VECTOR space rather than in encoded RG —
// averaging the encoding directly would be wrong across the z<0 wrap seam.
inline float3 oct_decode(float ox, float oy) {
    float ex = ox * 2.0f - 1.0f;
    float ey = oy * 2.0f - 1.0f;
    const float ez = 1.0f - std::fabs(ex) - std::fabs(ey);
    if (ez < 0.0f) {
        const float sx = ex >= 0.0f ? 1.0f : -1.0f;
        const float sy = ey >= 0.0f ? 1.0f : -1.0f;
        const float tx = (1.0f - std::fabs(ey)) * sx;
        const float ty = (1.0f - std::fabs(ex)) * sy;
        ex = tx; ey = ty;
    }
    return unit(make_float3(ex, ey, ez));
}


// One supersample of one view: the nearest surface hit, or empty.
struct SubSample {
    float    depth = -3.4e38f;     // larger = nearer the camera
    bool     hit = false;
    float3   normal{};
    float    ao = 1.0f;
    float4   tint{};
    int      material = -1;
};

} // namespace

const char* load_failure_text(LoadFailure f) {
    switch (f) {
        case LoadFailure::None:      return "ok";
        case LoadFailure::Absent:    return "no impostor sidecar";
        case LoadFailure::Open:      return "sidecar present but unreadable";
        case LoadFailure::Header:    return "bad magic";
        case LoadFailure::Version:   return "impostor format version mismatch";
        case LoadFailure::Identity:  return "sidecar belongs to a different part";
        case LoadFailure::Stale:     return "atlas depicts a different mesh (stale)";
        case LoadFailure::Truncated: return "truncated payload";
        case LoadFailure::Checksum:  return "payload checksum mismatch";
        case LoadFailure::Malformed: return "malformed record";
    }
    return "unknown";
}

// M4: the atlas is the IMPO section of the part's bundle, so this returns the
// bundle path like every other cache_path_* helper. The old `.fimp` sidecar
// beside the flat artifact is gone -- along with the class of failure where a
// flat carrying impostor rungs outlived its atlas, or an orphan atlas outlived
// its flat. One file, one identity, one atomic publish.
std::string cache_path_impostor(uint64_t resolved_hash) {
    return part_bundle::cache_path_bundle(resolved_hash);
}

// ---------------------------------------------------------------------------
// depicts-hash: what the atlas is a picture OF.
//
// The .flat.part is content-addressed on the part's resolved hash, but the
// TERMINAL RUNG is a function of the ladder rule as well as the source mesh --
// M1.5's benefit floor moved several parts' terminal rung without changing any
// part hash. Folding the terminal triangles themselves, plus the atlas layout
// constants, means the sidecar is invalidated by exactly the things that would
// change its pixels, and by nothing else.
uint64_t depicts_hash_begin() {
    uint64_t h = 1469598103934665603ull;
    // M4: kFormatVersion is no longer folded here -- it is a component of the
    // version vector, which depicts_hash_finish folds in one place for the
    // whole engine. Only the atlas LAYOUT constants (which are not versions)
    // stay in this stream.
    // cell_px() is in this stream, so a resolution change makes every existing
    // atlas report Stale rather than decoding at the wrong scale.
    const uint32_t params[3] = {kViews, cell_px(), kSuperSample};
    const auto* b = reinterpret_cast<const uint8_t*>(params);
    for (size_t i = 0; i < sizeof(params); ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

void depicts_hash_add_cluster(uint64_t& h, uint32_t cluster_index,
                              const std::vector<Tri>& tris,
                              const std::vector<TriEx>& triex) {
    const auto fold = [&h](const void* data, size_t len) {
        const auto* b = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < len; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    };
    fold(&cluster_index, sizeof(cluster_index));
    const uint64_t count = tris.size();
    fold(&count, sizeof(count));
    // Only the vertices: Tri's centroid is derived, and its 16-byte alignment
    // padding is not guaranteed to be zeroed by every producer.
    for (const Tri& t : tris) {
        const float v[9] = {t.vertex0.x, t.vertex0.y, t.vertex0.z,
                            t.vertex1.x, t.vertex1.y, t.vertex1.z,
                            t.vertex2.x, t.vertex2.y, t.vertex2.z};
        fold(v, sizeof(v));
    }

    // The tint layer is now baked from the MATERIAL REGISTRY's albedo (see
    // AlbedoLut), so the registry is an input to the pixels and therefore has
    // to be an input to the identity. Without this, recolouring a material
    // leaves every existing card showing the old colour, and the sidecar
    // reports Fresh while doing it -- the same class of bug part_flatten.h's
    // ladder_shape_digest note describes: "the bake configuration and the
    // artifact's identity must be derived from ONE set of readers, or a knob
    // can change the ladder without changing the identity".
    //
    // Only the materials this cluster actually REFERENCES are folded, so an
    // unrelated material edit does not invalidate this part. Sorted and
    // uniqued first: triangle order must not change the digest.
    //
    // `present` is folded beside each colour so that "material 29 is absent
    // from the registry" and "material 29 is black" are different identities.
    // They bake differently -- absent falls back to the vertex tint -- so
    // colliding them would let a part baked before its world's dynamic
    // materials were defined validate against one baked after.
    std::vector<int> ids;
    if (triex.size() == tris.size()) {
        ids.reserve(triex.size());
        for (const TriEx& e : triex)
            if (e.materialId >= 0 && e.materialId < 4096)
                ids.push_back(e.materialId);
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    }
    const uint64_t id_count = ids.size();
    fold(&id_count, sizeof(id_count));
    const int registry_count = MaterialRegistryCount();
    for (int id : ids) {
        fold(&id, sizeof(id));
        // `id < registry_count`, not a NULL test: MaterialRegistryGet hands
        // back a default gray for an out-of-range id rather than nothing, so
        // the pointer can never report absence. Same rule AlbedoLut uses --
        // they have to agree, or the identity would claim a colour the bake
        // did not use.
        const bool present = id < registry_count;
        const MaterialDef* def = present ? MaterialRegistryGet(id) : nullptr;
        const uint8_t present_byte = present ? 1u : 0u;
        fold(&present_byte, sizeof(present_byte));
        const float rgb[3] = {def ? def->albedo[0] : 0.0f,
                              def ? def->albedo[1] : 0.0f,
                              def ? def->albedo[2] : 0.0f};
        fold(rgb, sizeof(rgb));
    }
}

uint64_t depicts_hash_finish(uint64_t h) { return matter_version::fold(h); }

// ---------------------------------------------------------------------------
// THE CARD'S ALBEDO MUST BE THE VIRTUAL TEXTURE'S ALBEDO, NOT THE VERTEX TINT.
//
// gbuffer.frag shades a mesh two different ways depending on whether its rung
// reached a VT page, and the two disagree about where colour comes from:
//
//   VT path    vt_composite.comp sample_material(): a material with no detail
//              tileset slot -- which every prop material is; the only slots
//              are the five ground tilesets -- takes `s.albedo = m.albedo.rgb`,
//              filled from MaterialGpuRecord::base_roughness.
//   flat path  resolveBaseColor() = mix(base_roughness.rgb, tint.rgb, tint.a),
//              and DSL geometry authors tint.a = 1, so it returns the VERTEX
//              TINT and the material's own colour is never consulted.
//
// Those are two independently authored numbers that nothing reconciles; on
// world_demo's conifers they differ by ~3x. The impostor bake sampled TriEx
// tint, so a card was always on the flat side of that split -- and once the
// mesh rungs were fixed to reach their pages (a6331fba), the cards were the
// only thing left showing the pale value, which is what made them stand out.
//
// So resolve the same albedo the compositor would: the material's registry
// albedo, per subsample, AVERAGED rather than voted. Averaging is what makes
// a bark/foliage boundary antialias inside a texel exactly as the tint
// average did; a dominant-material pick would harden every such boundary.
//
// The alpha written alongside is 1.0, which makes resolveBaseColor return
// these bytes unchanged -- so this needs no shader change at all.
//
// FALLBACK is the old behaviour, deliberately: a material the registry does
// not know (id < 0, past the table, or a diagnostic-offset id) keeps its
// vertex tint rather than baking black. That covers the have_ex == false path
// and any part baked before its world's dynamic materials were defined.
struct AlbedoLut {
    // Indexed by materialId. `known` distinguishes "registry gave us a colour"
    // from "id absent", which is what selects the tint fallback per subsample.
    std::vector<float3> albedo;
    std::vector<uint8_t> known;

    void build(const std::vector<TriEx>& triex) {
        int hi = -1;
        for (const TriEx& e : triex)
            if (e.materialId >= 0 && e.materialId < 4096)
                hi = std::max(hi, e.materialId);
        if (hi < 0) return;
        albedo.assign(static_cast<size_t>(hi) + 1, make_float3(0, 0, 0));
        known.assign(static_cast<size_t>(hi) + 1, 0u);
        // The `id < count` test is the ONLY thing separating a real material
        // from an unknown one: MaterialRegistryGet never returns NULL -- an
        // out-of-range id gets a default GRAY back. Testing the pointer would
        // bake that gray into the card and look like a shading bug rather
        // than a missing material.
        const int count = MaterialRegistryCount();
        for (int id = 0; id <= hi && id < count; ++id) {
            const MaterialDef* def = MaterialRegistryGet(id);
            albedo[static_cast<size_t>(id)] =
                make_float3(def->albedo[0], def->albedo[1], def->albedo[2]);
            known[static_cast<size_t>(id)] = 1u;
        }
    }

    bool lookup(int id, float3& out) const {
        if (id < 0 || static_cast<size_t>(id) >= known.size()) return false;
        if (!known[static_cast<size_t>(id)]) return false;
        out = albedo[static_cast<size_t>(id)];
        return true;
    }
};

// ---------------------------------------------------------------------------
// The bake.

bool bake_cluster(uint32_t cluster_index, const std::vector<Tri>& tris,
                  const std::vector<TriEx>& triex, ClusterImpostor& out) {
    if (tris.empty()) return false;
    const bool have_ex = triex.size() == tris.size();
    AlbedoLut albedo_lut;
    if (have_ex) albedo_lut.build(triex);

    // Centre: the midpoint of the vertex AABB. Half-extent: the radius of the
    // bounding SPHERE about that centre, so one square quad of that size
    // contains the mesh from EVERY azimuth -- a per-view fit would make the
    // billboard's world size depend on the view, which is a pop.
    float mn[3] = {3.4e38f, 3.4e38f, 3.4e38f};
    float mx[3] = {-3.4e38f, -3.4e38f, -3.4e38f};
    const auto grow = [&](const float3& p) {
        mn[0] = std::fmin(mn[0], p.x); mx[0] = std::fmax(mx[0], p.x);
        mn[1] = std::fmin(mn[1], p.y); mx[1] = std::fmax(mx[1], p.y);
        mn[2] = std::fmin(mn[2], p.z); mx[2] = std::fmax(mx[2], p.z);
    };
    for (const Tri& t : tris) { grow(t.vertex0); grow(t.vertex1); grow(t.vertex2); }
    const float3 center = make_float3(0.5f * (mn[0] + mx[0]),
                                      0.5f * (mn[1] + mx[1]),
                                      0.5f * (mn[2] + mx[2]));
    float radius_sq = 0.0f;
    const auto reach = [&](const float3& p) {
        const float3 d = make_float3(p.x - center.x, p.y - center.y, p.z - center.z);
        radius_sq = std::fmax(radius_sq, dot3(d, d));
    };
    for (const Tri& t : tris) { reach(t.vertex0); reach(t.vertex1); reach(t.vertex2); }
    const float radius = std::sqrt(radius_sq);
    if (!(radius > 1e-6f)) return false;
    // GUARD BAND: derived from the cell size so the margin is a constant
    // number of TEXELS at every resolution. See impostor_bake.h -- a fixed
    // band buys a different margin at every cell size, which is the trap the
    // old hard-coded 1.20 (plus a comment asking the next person to recompute
    // it) left behind now that the cell size is a runtime setting.
    const float half_extent = radius * guard_band();

    out = ClusterImpostor{};
    out.cluster_index = cluster_index;
    out.center[0] = center.x; out.center[1] = center.y; out.center[2] = center.z;
    out.half_extent = half_extent;
    out.source_tris = static_cast<uint32_t>(tris.size());
    // Every size below is derived from the cell resolution ONCE, here, so a
    // getenv between two of them cannot produce an atlas whose halves disagree.
    const uint32_t cell = cell_px();
    const uint32_t sub_edge = cell * kSuperSample;
    const uint32_t layer_edge = layer_px();
    const size_t   layer_sz = layer_bytes();
    out.atlas.assign(atlas_bytes(), 0);

    // Dominant material, area-weighted over covered subsamples.
    std::vector<uint64_t> material_votes;

    std::vector<SubSample> sub(static_cast<size_t>(sub_edge) * sub_edge);
    const float inv_h = 1.0f / half_extent;

    for (uint32_t view = 0; view < kViews; ++view) {
        std::fill(sub.begin(), sub.end(), SubSample{});
        // Azimuth-major: view = elevation * kAzimuths + azimuth (view_index()).
        const uint32_t az_i = view % kAzimuths;
        const uint32_t el_i = view / kAzimuths;
        const float angle = 6.28318530717958647692f *
                            static_cast<float>(az_i) / static_cast<float>(kAzimuths);
        const float elev = static_cast<float>(el_i) * kElevationStep;
        const float ce = std::cos(elev), se = std::sin(elev);
        // view_z points from the object TOWARD the camera. raster.vert derives
        // the same pair from the eye direction as azimuth = atan2(x,z) and
        // elevation = asin(y), which is the exact inverse of this line — the
        // two must stay inverses or the card shows a view it was not baked for.
        const float3 view_z = make_float3(std::sin(angle) * ce, se,
                                          std::cos(angle) * ce);
        // Well-conditioned while |view_z.y| < 1: |cross| = cos(elev) >= 0.5 for
        // the 60-degree top ring, so no degenerate-basis guard is needed HERE.
        // The runtime needs one (it uses the true eye direction, which can look
        // straight down); see raster.vert.
        const float3 right  = unit(cross3(make_float3(0.0f, 1.0f, 0.0f), view_z));
        const float3 up_v   = cross3(view_z, right);

        for (size_t ti = 0; ti < tris.size(); ++ti) {
            const Tri& t = tris[ti];
            const float3 verts[3] = {t.vertex0, t.vertex1, t.vertex2};
            float sx[3], sy[3], sz[3];
            for (int c = 0; c < 3; ++c) {
                const float3 rel = make_float3(verts[c].x - center.x,
                                               verts[c].y - center.y,
                                               verts[c].z - center.z);
                // [-1,1] -> subsample grid coordinates.
                const float nx = dot3(rel, right) * inv_h;
                const float ny = dot3(rel, up_v) * inv_h;
                sx[c] = (nx * 0.5f + 0.5f) * static_cast<float>(sub_edge);
                sy[c] = (0.5f - ny * 0.5f) * static_cast<float>(sub_edge);
                sz[c] = dot3(rel, view_z);
            }
            const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) -
                               (sx[2] - sx[0]) * (sy[1] - sy[0]);
            if (std::fabs(area) < 1e-12f) continue;
            const float inv_area = 1.0f / area;

            int x0 = static_cast<int>(std::floor(std::fmin(sx[0], std::fmin(sx[1], sx[2]))));
            int x1 = static_cast<int>(std::ceil (std::fmax(sx[0], std::fmax(sx[1], sx[2]))));
            int y0 = static_cast<int>(std::floor(std::fmin(sy[0], std::fmin(sy[1], sy[2]))));
            int y1 = static_cast<int>(std::ceil (std::fmax(sy[0], std::fmax(sy[1], sy[2]))));
            x0 = std::max(x0, 0); y0 = std::max(y0, 0);
            x1 = std::min(x1, static_cast<int>(sub_edge) - 1);
            y1 = std::min(y1, static_cast<int>(sub_edge) - 1);

            for (int py = y0; py <= y1; ++py) {
                const float fy = static_cast<float>(py) + 0.5f;
                for (int px = x0; px <= x1; ++px) {
                    const float fx = static_cast<float>(px) + 0.5f;
                    const float w1 = ((fx - sx[0]) * (sy[2] - sy[0]) -
                                      (sx[2] - sx[0]) * (fy - sy[0])) * inv_area;
                    const float w2 = ((sx[1] - sx[0]) * (fy - sy[0]) -
                                      (fx - sx[0]) * (sy[1] - sy[0])) * inv_area;
                    const float w0 = 1.0f - w1 - w2;
                    if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
                    const float depth = w0 * sz[0] + w1 * sz[1] + w2 * sz[2];
                    SubSample& s = sub[static_cast<size_t>(py) * sub_edge + px];
                    // Strict >: the FIRST triangle wins an exact tie, which is
                    // what makes coplanar geometry order-deterministic.
                    if (s.hit && !(depth > s.depth)) continue;
                    s.hit = true;
                    s.depth = depth;
                    if (have_ex) {
                        const TriEx& e = triex[ti];
                        s.normal = make_float3(
                            w0 * e.N0.x + w1 * e.N1.x + w2 * e.N2.x,
                            w0 * e.N0.y + w1 * e.N1.y + w2 * e.N2.y,
                            w0 * e.N0.z + w1 * e.N1.z + w2 * e.N2.z);
                        s.ao = w0 * e.ao0 + w1 * e.ao1 + w2 * e.ao2;
                        s.material = e.materialId;
                        // The VT path's albedo where the registry knows this
                        // material; the authored tint where it does not. See
                        // AlbedoLut above for why this is not the tint.
                        float3 ma{};
                        if (albedo_lut.lookup(e.materialId, ma))
                            s.tint = make_float4(ma.x, ma.y, ma.z, 1.0f);
                        else
                            s.tint = e.tint;
                    } else {
                        s.normal = unit(cross3(t.vertex1 - t.vertex0,
                                               t.vertex2 - t.vertex0));
                        s.ao = 1.0f;
                        s.tint = make_float4(1.0f, 1.0f, 1.0f, 0.0f);
                        s.material = -1;
                    }
                }
            }
        }

        // Resolve the view's subsamples into its cell of both layers.
        const uint32_t cell_x = (view % kGridDim) * cell;
        const uint32_t cell_y = (view / kGridDim) * cell;
        for (uint32_t cy = 0; cy < cell; ++cy) {
            for (uint32_t cx = 0; cx < cell; ++cx) {
                int covered = 0;
                float3 nsum = make_float3(0.0f, 0.0f, 0.0f);
                float ao_sum = 0.0f;
                float depth_sum = 0.0f;
                float tr = 0.0f, tg = 0.0f, tb = 0.0f, ta = 0.0f;
                for (uint32_t sy2 = 0; sy2 < kSuperSample; ++sy2) {
                    for (uint32_t sx2 = 0; sx2 < kSuperSample; ++sx2) {
                        const SubSample& s =
                            sub[static_cast<size_t>(cy * kSuperSample + sy2) * sub_edge +
                                (cx * kSuperSample + sx2)];
                        if (!s.hit) continue;
                        ++covered;
                        nsum = make_float3(nsum.x + s.normal.x, nsum.y + s.normal.y,
                                           nsum.z + s.normal.z);
                        ao_sum += s.ao;
                        depth_sum += s.depth;
                        tr += s.tint.x; tg += s.tint.y; tb += s.tint.z; ta += s.tint.w;
                        // Bounded: a corrupt or diagnostic-offset materialId
                        // (build_indexed_part_geometry adds 1e6 for one debug
                        // mode) must not size a vote table by an arbitrary
                        // integer read out of a mesh.
                        if (s.material >= 0 && s.material < 4096) {
                            if (material_votes.size() <=
                                static_cast<size_t>(s.material))
                                material_votes.resize(
                                    static_cast<size_t>(s.material) + 1, 0);
                            ++material_votes[static_cast<size_t>(s.material)];
                        }
                    }
                }
                const size_t texel =
                    (static_cast<size_t>(cell_y + cy) * layer_edge + (cell_x + cx)) * 4;
                if (covered == 0) continue;   // atlas was zero-filled
                const float inv_n = 1.0f / static_cast<float>(covered);
                float ox = 0.5f, oy = 0.5f;
                oct_encode(nsum, ox, oy);
                uint8_t* shade = out.atlas.data() + texel;
                shade[0] = to_u8(ox);
                shade[1] = to_u8(oy);
                // DEPTH, not AO. The AO this channel used to carry was a
                // constant 1.0 for every DSL-built part (TriEx::ao is never
                // populated outside the surfacing path), so eight bits were
                // being spent transporting the number one. Depth is what the
                // card actually lacked: without it every impostor pixel
                // reports the plane through the object's CENTRE, which is why
                // a shadow ray from one starts inside the volume and why
                // rotating around a tree snaps between azimuths instead of
                // parallaxing.
                //
                // Measured from the NEAR bound (sz = +half_extent) and
                // normalized over the 2h extent, so d = 0 at the front of the
                // bound and 1 at the back. That convention is load-bearing
                // twice over: the runtime card sits at the near bound, so
                // every depth write pushes AWAY from the camera and stays
                // legal under gbuffer.frag's `layout(depth_less)`; and the
                // parallax march is always inward, like every other POM.
                shade[2] = to_u8((1.0f - depth_sum * inv_n * inv_h) * 0.5f);
                shade[3] = to_u8(static_cast<float>(covered) /
                                 static_cast<float>(kSuperSample * kSuperSample));
                uint8_t* tint = out.atlas.data() + layer_sz + texel;
                tint[0] = to_u8(tr * inv_n);
                tint[1] = to_u8(tg * inv_n);
                tint[2] = to_u8(tb * inv_n);
                tint[3] = to_u8(ta * inv_n);
            }
        }
    }

    uint64_t best_votes = 0;
    out.material_index = 0;
    for (size_t m = 0; m < material_votes.size(); ++m) {
        if (material_votes[m] > best_votes) {
            best_votes = material_votes[m];
            out.material_index = static_cast<uint32_t>(m);
        }
    }
    if (best_votes == 0) out.material_index = UINT32_MAX;

    // Last step: pad the silhouette so runtime bilinear taps never blend a
    // covered texel against the zero-fill (see the layout note in the header,
    // and the function itself for the depth decision).
    pad_cluster_atlas(out.atlas);
    return true;
}

// ---------------------------------------------------------------------------
// Edge padding (format v7). See impostor_bake.h's atlas-layout note for what
// this fixes; the mechanics and the one non-obvious decision live here.
//
// WHAT IS PADDED. shade R,G (octahedral normal), shade B (depth) and the whole
// tint texel. Coverage (shade A) is never written: the cutout and the
// fractional-coverage antialiasing must see exactly the silhouette the
// rasterizer produced, or the padding would GROW the drawn impostor.
//
// DEPTH IS PADDED TOO, deliberately. An uncovered texel's zero decodes as
// "surface AT the card's near-bound plane" — the frontmost value the channel
// can carry. A bilinear tap that straddles the silhouette therefore biased its
// depth toward the front, which under-drives the parallax re-tap AND writes
// the rim's gl_FragDepth nearer than the surface the rim visually belongs to.
// The neighbour's depth is the right filler, and it is SAFE by construction:
// baked depth only ever pushes the fragment away from the camera, so no padded
// value can violate gbuffer.frag's layout(depth_less) — the failure mode of a
// wrong padded depth is a rim pixel sitting at its neighbour's depth instead
// of at the card plane, which is strictly more correct.
//
// NORMALS ARE AVERAGED IN VECTOR SPACE (decode, sum, re-encode), not in
// encoded RG, because the octahedral map has a wrap seam for z < 0 and a
// byte-space average across it produces a direction unrelated to either
// neighbour — the exact class of artifact this padding exists to remove.
//
// DETERMINISM. Row-major texel order, a fixed neighbour order, integer
// averages with explicit rounding, and the same sqrt-based unit() as the
// rasterizer. Ring N+1 reads ring N's already-written values (the mask is
// updated only between passes, so a pass never reads what it wrote), which is
// the ping-pong that makes the result independent of anything but the input.
void pad_cluster_atlas(std::vector<uint8_t>& atlas) {
    // The layout comes from the buffer itself: two RGBA8 layers of an
    // (kGridDim * cell)^2 grid. Deriving it from atlas.size() means this
    // function cannot disagree with the buffer it was handed even if the cell
    // setting has moved since the buffer was made.
    if (atlas.empty() || (atlas.size() % (kGridDim * kGridDim * 8)) != 0)
        return;
    const size_t texels = atlas.size() / 8;   // per layer, 4 bytes per texel
    uint32_t layer_edge = static_cast<uint32_t>(std::sqrt(double(texels)));
    while (static_cast<size_t>(layer_edge) * layer_edge < texels) ++layer_edge;
    if (static_cast<size_t>(layer_edge) * layer_edge != texels ||
        layer_edge % kGridDim != 0)
        return;
    const uint32_t cell = layer_edge / kGridDim;
    const size_t layer_sz = texels * 4;

    // Neighbour order is part of the byte-reproducibility contract.
    static constexpr int kOff[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0},
                                       {1, 0},   {-1, 1}, {0, 1},  {1, 1}};

    std::vector<uint8_t> mask(static_cast<size_t>(cell) * cell);
    std::vector<uint32_t> filled;
    for (uint32_t gy = 0; gy < kGridDim; ++gy) {
        for (uint32_t gx = 0; gx < kGridDim; ++gx) {
            const uint32_t cx0 = gx * cell, cy0 = gy * cell;
            bool any_covered = false, all_covered = true;
            for (uint32_t cy = 0; cy < cell; ++cy) {
                for (uint32_t cx = 0; cx < cell; ++cx) {
                    const size_t t =
                        (static_cast<size_t>(cy0 + cy) * layer_edge + cx0 + cx) * 4;
                    const bool covered = atlas[t + 3] != 0;
                    mask[static_cast<size_t>(cy) * cell + cx] = covered ? 1 : 0;
                    any_covered |= covered;
                    all_covered &= covered;
                }
            }
            if (!any_covered || all_covered) continue;   // empty view, or no rim
            for (uint32_t pass = 0; pass < kPadTexels; ++pass) {
                filled.clear();
                for (uint32_t cy = 0; cy < cell; ++cy) {
                    for (uint32_t cx = 0; cx < cell; ++cx) {
                        if (mask[static_cast<size_t>(cy) * cell + cx]) continue;
                        float3 nsum = make_float3(0.0f, 0.0f, 0.0f);
                        uint32_t depth_sum = 0;
                        uint32_t tint_sum[4] = {0, 0, 0, 0};
                        uint32_t count = 0;
                        for (const auto& o : kOff) {
                            const int nx = static_cast<int>(cx) + o[0];
                            const int ny = static_cast<int>(cy) + o[1];
                            if (nx < 0 || ny < 0 || nx >= static_cast<int>(cell) ||
                                ny >= static_cast<int>(cell))
                                continue;
                            if (!mask[static_cast<size_t>(ny) * cell + nx]) continue;
                            const size_t s =
                                (static_cast<size_t>(cy0 + ny) * layer_edge +
                                 cx0 + nx) * 4;
                            const float3 n = oct_decode(atlas[s] / 255.0f,
                                                        atlas[s + 1] / 255.0f);
                            nsum = make_float3(nsum.x + n.x, nsum.y + n.y,
                                               nsum.z + n.z);
                            depth_sum += atlas[s + 2];
                            const uint8_t* tn = atlas.data() + layer_sz + s;
                            for (int k = 0; k < 4; ++k) tint_sum[k] += tn[k];
                            ++count;
                        }
                        if (count == 0) continue;
                        const size_t t =
                            (static_cast<size_t>(cy0 + cy) * layer_edge +
                             cx0 + cx) * 4;
                        float ox = 0.5f, oy = 0.5f;
                        oct_encode(nsum, ox, oy);
                        atlas[t]     = to_u8(ox);
                        atlas[t + 1] = to_u8(oy);
                        atlas[t + 2] = static_cast<uint8_t>(
                            (depth_sum + count / 2) / count);
                        // atlas[t + 3] is coverage and stays exactly 0.
                        uint8_t* tint = atlas.data() + layer_sz + t;
                        for (int k = 0; k < 4; ++k)
                            tint[k] = static_cast<uint8_t>(
                                (tint_sum[k] + count / 2) / count);
                        filled.push_back(cy * cell + cx);
                    }
                }
                if (filled.empty()) break;
                for (uint32_t idx : filled) mask[idx] = 1;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// The billboard's ladder geometry.

void build_quad(const ClusterImpostor& imp, std::vector<Tri>& tris,
                std::vector<TriEx>& triex) {
    tris.clear();
    triex.clear();
    const float h = imp.half_extent;
    const float3 c = make_float3(imp.center[0], imp.center[1], imp.center[2]);
    // Corner ordinals, and the sign pair raster.vert reconstructs from them:
    //   0 -> (-1,-1)   1 -> (+1,-1)   2 -> (+1,+1)   3 -> (-1,+1)
    const float sx[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
    const float sy[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
    float3 p[4];
    for (int i = 0; i < 4; ++i)
        p[i] = make_float3(c.x + sx[i] * h, c.y + sy[i] * h, c.z);

    const int idx[2][3] = {{0, 1, 2}, {0, 2, 3}};
    for (int t = 0; t < 2; ++t) {
        Tri tri{};
        tri.vertex0 = p[idx[t][0]];
        tri.vertex1 = p[idx[t][1]];
        tri.vertex2 = p[idx[t][2]];
        tri.centroid = make_float3((tri.vertex0.x + tri.vertex1.x + tri.vertex2.x) / 3.0f,
                                   (tri.vertex0.y + tri.vertex1.y + tri.vertex2.y) / 3.0f,
                                   (tri.vertex0.z + tri.vertex1.z + tri.vertex2.z) / 3.0f);
        TriEx ex{};
        // uv carries the marker + corner ordinal; build_raster_mesh_data
        // forwards uv into surface_uvs, which becomes VkRasterVertex::surface
        // .xy, so the vertex stage sees it with NO new vertex attribute and no
        // stride change. ao carries the half-extent (the impostor's AO comes
        // from the atlas, so the channel is free) -- that is what lets the
        // vertex stage recover the quad's centre from any single corner.
        ex.uv0 = make_float2(kQuadMarker, static_cast<float>(idx[t][0]));
        ex.uv1 = make_float2(kQuadMarker, static_cast<float>(idx[t][1]));
        ex.uv2 = make_float2(kQuadMarker, static_cast<float>(idx[t][2]));
        ex.N0 = ex.N1 = ex.N2 = make_float3(0.0f, 0.0f, 1.0f);
        ex.materialId = static_cast<int>(imp.material_index);
        // tint transports the atlas slot: r,g = slot (patched by the renderer
        // once a layer is assigned), b = this impostor's ordinal within the
        // part, a = 0. See VkSceneRenderer::assign_impostor_slots.
        ex.tint = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
        ex.ao0 = ex.ao1 = ex.ao2 = h;
        tris.push_back(tri);
        triex.push_back(ex);
    }
}

// ---------------------------------------------------------------------------
// Sidecar I/O.

namespace {

constexpr char kMagic[4] = {'F', 'I', 'M', 'P'};

struct Header {
    char     magic[4];
    uint32_t version;
    uint32_t views;
    uint32_t cell_px;
    uint32_t grid_dim;
    uint32_t supersample;
    uint64_t part_hash;
    uint64_t depicts_hash;
    uint32_t cluster_count;
    uint32_t reserved;
    uint64_t payload_bytes;
    uint64_t payload_checksum;
};
static_assert(sizeof(Header) == 64, "impostor sidecar header layout");

constexpr size_t kRecordFixed = 4 + 12 + 4 + 4 + 4;   // idx, centre, extent, mat, tris

template <class T>
void put(std::vector<uint8_t>& v, const T& value) {
    const auto* b = reinterpret_cast<const uint8_t*>(&value);
    v.insert(v.end(), b, b + sizeof(T));
}

} // namespace

bool save(const std::string& path, uint64_t part_hash, uint64_t depicts_hash,
          const PartImpostor& in) {
    if (in.clusters.empty()) return false;

    std::vector<uint8_t> payload;
    payload.reserve(in.clusters.size() * (kRecordFixed + atlas_bytes()));
    for (const auto& c : in.clusters) {
        if (c.atlas.size() != atlas_bytes()) return false;
        put(payload, c.cluster_index);
        put(payload, c.center[0]); put(payload, c.center[1]); put(payload, c.center[2]);
        put(payload, c.half_extent);
        put(payload, c.material_index);
        put(payload, c.source_tris);
        payload.insert(payload.end(), c.atlas.begin(), c.atlas.end());
    }

    Header h{};
    std::memcpy(h.magic, kMagic, 4);
    h.version = kFormatVersion;
    h.views = kViews;
    h.cell_px = cell_px();
    h.grid_dim = kGridDim;
    h.supersample = kSuperSample;
    h.part_hash = part_hash;
    h.depicts_hash = depicts_hash;
    h.cluster_count = static_cast<uint32_t>(in.clusters.size());
    h.reserved = 0;
    h.payload_bytes = payload.size();
    h.payload_checksum = part_asset::fnv1a64(payload.data(), payload.size());

    // M4: header + payload become one bundle section, byte-for-byte what the
    // `.fimp` file held, so the atlas keeps validating its own magic, format,
    // part identity and depicts-hash independently of the bundle around it.
    std::vector<uint8_t> section;
    section.reserve(sizeof(h) + payload.size());
    const auto* hb = reinterpret_cast<const uint8_t*>(&h);
    section.insert(section.end(), hb, hb + sizeof(h));
    section.insert(section.end(), payload.begin(), payload.end());
    return part_bundle::write_section(path, part_hash,
                                      part_bundle::kSectionImpostor,
                                      section.data(), section.size());
}

bool load(const std::string& path, uint64_t part_hash, uint64_t depicts_hash,
          PartImpostor& out, LoadFailure* fail, std::string* reason) {
    out.clusters.clear();
    const auto reject = [&](LoadFailure f, const std::string& why) {
        if (fail) *fail = f;
        if (reason) *reason = why;
        out.clusters.clear();
        return false;
    };
    if (fail) *fail = LoadFailure::None;

    std::vector<uint8_t> section;
    if (!part_bundle::read_section(path, part_hash,
                                   part_bundle::kSectionImpostor, section))
        return reject(LoadFailure::Absent, load_failure_text(LoadFailure::Absent));

    Header h{};
    if (section.size() < sizeof(h))
        return reject(LoadFailure::Truncated, "header short read");
    std::memcpy(&h, section.data(), sizeof(h));
    if (std::memcmp(h.magic, kMagic, 4) != 0) {
        return reject(LoadFailure::Header, load_failure_text(LoadFailure::Header));
    }
    if (h.version != kFormatVersion || h.views != kViews ||
        h.cell_px != cell_px() || h.grid_dim != kGridDim ||
        h.supersample != kSuperSample) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "format v%u/%uview/%upx, engine wants v%u/%uview/%upx",
                      h.version, h.views, h.cell_px, kFormatVersion, kViews,
                      cell_px());
        return reject(LoadFailure::Version, buf);
    }
    if (h.part_hash != part_hash) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "sidecar names part %016llx",
                      static_cast<unsigned long long>(h.part_hash));
        return reject(LoadFailure::Identity, buf);
    }
    if (h.depicts_hash != depicts_hash) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "depicts %016llx, ladder terminal is %016llx",
                      static_cast<unsigned long long>(h.depicts_hash),
                      static_cast<unsigned long long>(depicts_hash));
        return reject(LoadFailure::Stale, buf);
    }
    if (h.cluster_count == 0) {
        return reject(LoadFailure::Malformed, "zero clusters");
    }
    const uint64_t expect_bytes =
        static_cast<uint64_t>(h.cluster_count) * (kRecordFixed + atlas_bytes());
    if (h.payload_bytes != expect_bytes) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "payload declares %llu bytes, expected %llu",
                      static_cast<unsigned long long>(h.payload_bytes),
                      static_cast<unsigned long long>(expect_bytes));
        return reject(LoadFailure::Malformed, buf);
    }

    if (section.size() - sizeof(h) != static_cast<size_t>(h.payload_bytes))
        return reject(LoadFailure::Truncated, "payload short read");
    std::vector<uint8_t> payload(section.begin() + sizeof(h), section.end());
    if (part_asset::fnv1a64(payload.data(), payload.size()) != h.payload_checksum)
        return reject(LoadFailure::Checksum,
                      load_failure_text(LoadFailure::Checksum));

    const uint8_t* p = payload.data();
    out.clusters.resize(h.cluster_count);
    for (uint32_t i = 0; i < h.cluster_count; ++i) {
        ClusterImpostor& c = out.clusters[i];
        std::memcpy(&c.cluster_index, p, 4); p += 4;
        std::memcpy(c.center, p, 12); p += 12;
        std::memcpy(&c.half_extent, p, 4); p += 4;
        std::memcpy(&c.material_index, p, 4); p += 4;
        std::memcpy(&c.source_tris, p, 4); p += 4;
        c.atlas.assign(p, p + atlas_bytes()); p += atlas_bytes();
        if (!(c.half_extent > 0.0f) || !std::isfinite(c.half_extent) ||
            !std::isfinite(c.center[0]) || !std::isfinite(c.center[1]) ||
            !std::isfinite(c.center[2])) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "cluster %u has a non-finite extent/centre", c.cluster_index);
            return reject(LoadFailure::Malformed, buf);
        }
    }
    return true;
}

} // namespace impostor
