#include "impostor_bake.h"

#include "part_asset.h"   // part_asset::fnv1a64, replace_file_atomic lives in v2
#include "part_asset_v2.h"

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

constexpr uint32_t kSubEdge = kCellPx * kSuperSample;   // 128 subsamples/axis

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

std::string cache_path_impostor(uint64_t resolved_hash) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "parts/%016llx.fimp",
                  static_cast<unsigned long long>(resolved_hash));
    return buf;
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
    const uint32_t params[3] = {kViews, kCellPx, kSuperSample};
    const auto* b = reinterpret_cast<const uint8_t*>(params);
    for (size_t i = 0; i < sizeof(params); ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

void depicts_hash_add_cluster(uint64_t& h, uint32_t cluster_index,
                              const std::vector<Tri>& tris) {
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
}

uint64_t depicts_hash_finish(uint64_t h) { return matter_version::fold(h); }

// ---------------------------------------------------------------------------
// The bake.

bool bake_cluster(uint32_t cluster_index, const std::vector<Tri>& tris,
                  const std::vector<TriEx>& triex, ClusterImpostor& out) {
    if (tris.empty()) return false;
    const bool have_ex = triex.size() == tris.size();

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
    // GUARD BAND. The quad is 10 % larger than the bounding sphere, so the
    // silhouette stops ~1.5 texels short of the cell border on every side.
    // Cells are packed edge to edge in one atlas layer, so a bilinear tap at
    // the border of view v would otherwise reach into view v+1 -- a rock with
    // a sliver of the next azimuth welded to its edge. Measured against
    // kCellPx: a full-texel guard needs >= 1 / (1 - 2/32) = 1.067, and 1.10
    // buys the margin back at a cost of 10 % of the cell's linear resolution,
    // which the view count (not the cell size) already dominates.
    const float half_extent = radius * 1.10f;

    out = ClusterImpostor{};
    out.cluster_index = cluster_index;
    out.center[0] = center.x; out.center[1] = center.y; out.center[2] = center.z;
    out.half_extent = half_extent;
    out.source_tris = static_cast<uint32_t>(tris.size());
    out.atlas.assign(kAtlasBytes, 0);

    // Dominant material, area-weighted over covered subsamples.
    std::vector<uint64_t> material_votes;

    std::vector<SubSample> sub(static_cast<size_t>(kSubEdge) * kSubEdge);
    const float inv_h = 1.0f / half_extent;

    for (uint32_t view = 0; view < kViews; ++view) {
        std::fill(sub.begin(), sub.end(), SubSample{});
        const float angle = 6.28318530717958647692f *
                            static_cast<float>(view) / static_cast<float>(kViews);
        // view_z points from the object TOWARD the camera.
        const float3 view_z = make_float3(std::sin(angle), 0.0f, std::cos(angle));
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
                sx[c] = (nx * 0.5f + 0.5f) * static_cast<float>(kSubEdge);
                sy[c] = (0.5f - ny * 0.5f) * static_cast<float>(kSubEdge);
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
            x1 = std::min(x1, static_cast<int>(kSubEdge) - 1);
            y1 = std::min(y1, static_cast<int>(kSubEdge) - 1);

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
                    SubSample& s = sub[static_cast<size_t>(py) * kSubEdge + px];
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
                        s.tint = e.tint;
                        s.material = e.materialId;
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
        const uint32_t cell_x = (view % kGridDim) * kCellPx;
        const uint32_t cell_y = (view / kGridDim) * kCellPx;
        for (uint32_t cy = 0; cy < kCellPx; ++cy) {
            for (uint32_t cx = 0; cx < kCellPx; ++cx) {
                int covered = 0;
                float3 nsum = make_float3(0.0f, 0.0f, 0.0f);
                float ao_sum = 0.0f;
                float tr = 0.0f, tg = 0.0f, tb = 0.0f, ta = 0.0f;
                for (uint32_t sy2 = 0; sy2 < kSuperSample; ++sy2) {
                    for (uint32_t sx2 = 0; sx2 < kSuperSample; ++sx2) {
                        const SubSample& s =
                            sub[static_cast<size_t>(cy * kSuperSample + sy2) * kSubEdge +
                                (cx * kSuperSample + sx2)];
                        if (!s.hit) continue;
                        ++covered;
                        nsum = make_float3(nsum.x + s.normal.x, nsum.y + s.normal.y,
                                           nsum.z + s.normal.z);
                        ao_sum += s.ao;
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
                    (static_cast<size_t>(cell_y + cy) * kLayerPx + (cell_x + cx)) * 4;
                if (covered == 0) continue;   // atlas was zero-filled
                const float inv_n = 1.0f / static_cast<float>(covered);
                float ox = 0.5f, oy = 0.5f;
                oct_encode(nsum, ox, oy);
                uint8_t* shade = out.atlas.data() + texel;
                shade[0] = to_u8(ox);
                shade[1] = to_u8(oy);
                shade[2] = to_u8(ao_sum * inv_n);
                shade[3] = to_u8(static_cast<float>(covered) /
                                 static_cast<float>(kSuperSample * kSuperSample));
                uint8_t* tint = out.atlas.data() + kLayerBytes + texel;
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
    return true;
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
    payload.reserve(in.clusters.size() * (kRecordFixed + kAtlasBytes));
    for (const auto& c : in.clusters) {
        if (c.atlas.size() != kAtlasBytes) return false;
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
    h.cell_px = kCellPx;
    h.grid_dim = kGridDim;
    h.supersample = kSuperSample;
    h.part_hash = part_hash;
    h.depicts_hash = depicts_hash;
    h.cluster_count = static_cast<uint32_t>(in.clusters.size());
    h.reserved = 0;
    h.payload_bytes = payload.size();
    h.payload_checksum = part_asset::fnv1a64(payload.data(), payload.size());

    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(&h, sizeof(h), 1, f) == 1 &&
              std::fwrite(payload.data(), 1, payload.size(), f) == payload.size();
    ok = (std::fclose(f) == 0) && ok;
    if (!ok) { std::remove(tmp.c_str()); return false; }
    if (!part_asset::replace_file_atomic(tmp, path)) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
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

    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return reject(LoadFailure::Absent, load_failure_text(LoadFailure::Absent));

    Header h{};
    if (std::fread(&h, sizeof(h), 1, f) != 1) {
        std::fclose(f);
        return reject(LoadFailure::Truncated, "header short read");
    }
    if (std::memcmp(h.magic, kMagic, 4) != 0) {
        std::fclose(f);
        return reject(LoadFailure::Header, load_failure_text(LoadFailure::Header));
    }
    if (h.version != kFormatVersion || h.views != kViews ||
        h.cell_px != kCellPx || h.grid_dim != kGridDim ||
        h.supersample != kSuperSample) {
        std::fclose(f);
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "format v%u/%uview/%upx, engine wants v%u/%uview/%upx",
                      h.version, h.views, h.cell_px, kFormatVersion, kViews, kCellPx);
        return reject(LoadFailure::Version, buf);
    }
    if (h.part_hash != part_hash) {
        std::fclose(f);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "sidecar names part %016llx",
                      static_cast<unsigned long long>(h.part_hash));
        return reject(LoadFailure::Identity, buf);
    }
    if (h.depicts_hash != depicts_hash) {
        std::fclose(f);
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "depicts %016llx, ladder terminal is %016llx",
                      static_cast<unsigned long long>(h.depicts_hash),
                      static_cast<unsigned long long>(depicts_hash));
        return reject(LoadFailure::Stale, buf);
    }
    if (h.cluster_count == 0) {
        std::fclose(f);
        return reject(LoadFailure::Malformed, "zero clusters");
    }
    const uint64_t expect_bytes =
        static_cast<uint64_t>(h.cluster_count) * (kRecordFixed + kAtlasBytes);
    if (h.payload_bytes != expect_bytes) {
        std::fclose(f);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "payload declares %llu bytes, expected %llu",
                      static_cast<unsigned long long>(h.payload_bytes),
                      static_cast<unsigned long long>(expect_bytes));
        return reject(LoadFailure::Malformed, buf);
    }

    std::vector<uint8_t> payload(static_cast<size_t>(h.payload_bytes));
    const size_t got = std::fread(payload.data(), 1, payload.size(), f);
    std::fclose(f);
    if (got != payload.size())
        return reject(LoadFailure::Truncated, "payload short read");
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
        c.atlas.assign(p, p + kAtlasBytes); p += kAtlasBytes;
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
