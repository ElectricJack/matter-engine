// Warped ground field (VT Phase 2) — production solver. See warp_field.h and
// the design spec §4. Everything here is single-threaded and iterates arrays
// in index order only (hash maps are used for lookup, never iterated), so an
// identical input byte-for-byte produces an identical Field byte-for-byte.
//
// Solver shape: local/global ARAP (as-rigid-as-possible) parameterisation of
// the welded sector surface onto (u, v), with
//   * the sector border pinned by the deterministic shared-chain rule (§4.2)
//     so neighbouring sectors agree bitwise at their shared border;
//   * clamped cotangent weights (positive => the harmonic init has the
//     convex-combination structure that keeps folds rare);
//   * a harmonic init from the anchored world-XZ guess;
//   * per-triangle best-fit rotations (closed-form 2x2 polar, det +1 always,
//     so the local step never targets a reflection);
//   * a Jacobi-preconditioned conjugate-gradient global step, warm-started,
//     fixed budget;
//   * a bounded fold-relax post-pass accepted only while it strictly reduces
//     the fold count.
//
// Metric conventions (stated once, used for the §4.3 gates and the world-XZ
// baseline alike): phi is the CONTENT map uv -> surface; sigma1 >= sigma2 are
// the singular values of d(phi) per triangle. stretch = sigma1 (one texture
// metre covers sigma1 surface metres at the worst direction), compress =
// 1/sigma2, aniso = sigma1/sigma2. Percentiles are weighted by 3D triangle
// area. delta-J is measured on the FORWARD map J = [grad u; grad v]
// (world -> uv, the thing the shader freezes per fragment): Frobenius norm of
// the difference across each interior manifold edge, weighted by the pair's
// summed 3D area — this times the marched offset bounds the marched-content
// jump across a triangle edge (§4.1).

#include "warp_field.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace warp_field {

namespace {

// ---------------------------------------------------------------------------
// Small deterministic helpers (no rsqrtf approximations in the solver).
// ---------------------------------------------------------------------------
inline float3 f3(float x, float y, float z) { return make_float3(x, y, z); }
inline float3 norm3(const float3& v) {
    const float l = std::sqrt(dot(v, v));
    return l > 0.0f ? v * (1.0f / l) : f3(0, 0, 0);
}

struct double2 {
    double x = 0.0, y = 0.0;
};

// Bitwise position weld key (same discipline as the render weld:
// indexed_part_geometry.cpp uses exact-bit keys).
struct PosKey {
    float x, y, z;
    bool operator==(const PosKey& o) const {
        return std::memcmp(this, &o, sizeof(PosKey)) == 0;
    }
};
struct PosKeyHash {
    size_t operator()(const PosKey& k) const {
        const unsigned char* b = reinterpret_cast<const unsigned char*>(&k);
        size_t h = 1469598103934665603ull;
        for (size_t i = 0; i < sizeof(PosKey); ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
        return h;
    }
};

inline uint64_t edge_key(uint32_t a, uint32_t b) {
    if (a > b) std::swap(a, b);
    return (uint64_t(a) << 32) | uint64_t(b);
}

// Welded solve mesh shared by solve() / border_pins() / world_xz_stats().
struct SolveMesh {
    std::vector<float3> positions;
    std::vector<uint32_t> indices;   // usable triangles only
    std::vector<float> tri_area;     // 3D area per usable triangle
    size_t vert_count() const { return positions.size(); }
    size_t tri_count() const { return indices.size() / 3; }
};

bool build_solve_mesh(const Tri* tris, size_t tri_count,
                      const uint8_t* skirt_mask, SolveMesh& mesh) {
    if (!tris || tri_count == 0) return false;
    std::unordered_map<PosKey, uint32_t, PosKeyHash> weld;
    weld.reserve(tri_count * 2);
    mesh.positions.reserve(tri_count);
    mesh.indices.reserve(tri_count * 3);
    mesh.tri_area.reserve(tri_count);
    for (size_t t = 0; t < tri_count; ++t) {
        if (skirt_mask && skirt_mask[t]) continue;
        const float3 corners[3] = {tris[t].vertex0, tris[t].vertex1,
                                   tris[t].vertex2};
        const float3 n =
            cross(corners[1] - corners[0], corners[2] - corners[0]);
        const float area2 = std::sqrt(dot(n, n));  // 2x area
        if (!(area2 > 1e-10f)) continue;           // degenerate (or NaN)
        uint32_t idx[3];
        for (int c = 0; c < 3; ++c) {
            PosKey key{corners[c].x, corners[c].y, corners[c].z};
            auto [it, inserted] =
                weld.try_emplace(key, uint32_t(mesh.positions.size()));
            if (inserted) mesh.positions.push_back(corners[c]);
            idx[c] = it->second;
        }
        if (idx[0] == idx[1] || idx[1] == idx[2] || idx[0] == idx[2]) continue;
        mesh.indices.insert(mesh.indices.end(), {idx[0], idx[1], idx[2]});
        mesh.tri_area.push_back(0.5f * area2);
    }
    return mesh.tri_count() >= 16;
}

// ---------------------------------------------------------------------------
// Border chains + pins (§4.2). World coordinates throughout: the pin value is
// a function of the (bitwise shared) world chain alone, so both sectors of a
// border compute identical pins.
// ---------------------------------------------------------------------------
struct PinSet {
    std::vector<uint8_t> pinned;   // per vertex
    std::vector<double2> uv;       // valid where pinned
};

void compute_border_pins(const SolveMesh& mesh, const SolveOptions& opts,
                         PinSet& pins) {
    const size_t nv = mesh.vert_count();
    const size_t nt = mesh.tri_count();
    pins.pinned.assign(nv, 0);
    pins.uv.assign(nv, double2{});

    // Boundary edges: exactly one incident triangle.
    std::unordered_map<uint64_t, uint32_t> edge_count;
    edge_count.reserve(nt * 3);
    for (size_t t = 0; t < nt; ++t) {
        const uint32_t* i = &mesh.indices[t * 3];
        for (int e = 0; e < 3; ++e)
            edge_count[edge_key(i[e], i[(e + 1) % 3])]++;
    }

    // Side classification bands, sector-local: the mesher's shared border
    // cell rows are one voxel wide, so left-border vertices live in
    // [-voxel, ~0] and right-border ones in [S - voxel, S] (terrain_mesher's
    // ownership rule; see its border comment). Corner-zone edges fall in an
    // X band AND a Z band; X wins, deterministically, from both sectors'
    // points of view (the band classification is translation-consistent).
    // The voxel mesher's shared border cell columns land at local
    // [-voxel, 0] (left/near) and [S - voxel, S] (right/far); the heightfield
    // mesher's border verts sit exactly at 0 and S. Both are covered by:
    //   left/near: coord <= ~0        right/far: coord >= S - voxel - eps
    const float S = opts.sector_size;
    const float lo_edge = 1e-3f;
    const float hi_edge = S - opts.voxel - 1e-3f;
    auto side_of_vertex = [&](const float3& p, int& sx, int& sz) {
        sx = p.x <= lo_edge ? -1 : (p.x >= hi_edge ? 1 : 0);
        sz = p.z <= lo_edge ? -1 : (p.z >= hi_edge ? 1 : 0);
    };

    // Collect boundary edges classified per side. Side ids: 0 = x-left,
    // 1 = x-right, 2 = z-near, 3 = z-far.
    struct SideEdge {
        uint32_t a, b;
    };
    std::vector<SideEdge> side_edges[4];
    for (size_t t = 0; t < nt; ++t) {
        const uint32_t* i = &mesh.indices[t * 3];
        for (int e = 0; e < 3; ++e) {
            const uint32_t a = i[e], b = i[(e + 1) % 3];
            if (edge_count[edge_key(a, b)] != 1) continue;
            int axa, aza, axb, azb;
            side_of_vertex(mesh.positions[a], axa, aza);
            side_of_vertex(mesh.positions[b], axb, azb);
            // Edge belongs to a side when BOTH endpoints are in that side's
            // band. X wins over Z in corner zones.
            if (axa != 0 && axa == axb)
                side_edges[axa < 0 ? 0 : 1].push_back({a, b});
            else if (aza != 0 && aza == azb)
                side_edges[aza < 0 ? 2 : 3].push_back({a, b});
        }
    }

    // World coordinates (double): anchor + local. The anchor is a multiple of
    // the sector size, so this addition is the same operation both sectors
    // perform on their (bitwise shared, differently-anchored) local coords.
    auto world = [&](uint32_t v) {
        const float3& p = mesh.positions[v];
        return f3(float(opts.anchor_x + double(p.x)), p.y,
                  float(opts.anchor_z + double(p.z)));
    };
    auto lex_less = [&](uint32_t a, uint32_t b) {
        const float3 wa = world(a), wb = world(b);
        if (wa.x != wb.x) return wa.x < wb.x;
        if (wa.y != wb.y) return wa.y < wb.y;
        return wa.z < wb.z;
    };

    // Processing order gives X-side chains PRIORITY at corner vertices: a
    // corner vertex belongs to one x-chain and one z-chain, whose traces
    // disagree there on steep terrain. Z sides are written first, X sides
    // overwrite — deterministically, and identically from every sector that
    // shares the x-chain. The two sectors sharing a Z border therefore agree
    // on every pinned vertex EXCEPT the two chain-end corner vertices (each
    // overridden by its owner's own x-chain) — the spec's accepted
    // corner-knot residual, localized to single vertices.
    const int side_order[4] = {2, 3, 0, 1};
    for (int side_i = 0; side_i < 4; ++side_i) {
        const int side = side_order[side_i];
        auto& edges = side_edges[side];
        if (edges.empty()) continue;
        // Vertex adjacency within this side (indices into `edges`).
        std::unordered_map<uint32_t, std::vector<uint32_t>> adj;
        adj.reserve(edges.size() * 2);
        for (uint32_t ei = 0; ei < edges.size(); ++ei) {
            adj[edges[ei].a].push_back(ei);
            adj[edges[ei].b].push_back(ei);
        }
        // Chain start vertices: degree 1 (path endpoints), traversed in
        // deterministic order (edge array order gives first-appearance
        // determinism; we sort candidate starts lexicographically).
        std::vector<uint32_t> starts;
        for (uint32_t ei = 0; ei < edges.size(); ++ei) {
            for (uint32_t v : {edges[ei].a, edges[ei].b}) {
                if (adj[v].size() == 1) starts.push_back(v);
            }
        }
        std::sort(starts.begin(), starts.end(),
                  [&](uint32_t a, uint32_t b) { return lex_less(a, b); });
        starts.erase(std::unique(starts.begin(), starts.end()), starts.end());

        std::vector<uint8_t> edge_used(edges.size(), 0);
        auto trace_chain = [&](uint32_t start) {
            std::vector<uint32_t> chain{start};
            uint32_t v = start;
            for (;;) {
                uint32_t next_edge = UINT32_MAX;
                for (uint32_t ei : adj[v]) {
                    if (!edge_used[ei]) {
                        next_edge = ei;
                        break;
                    }
                }
                if (next_edge == UINT32_MAX) break;
                edge_used[next_edge] = 1;
                v = edges[next_edge].a == v ? edges[next_edge].b
                                            : edges[next_edge].a;
                chain.push_back(v);
                if (v == start) break;  // closed loop
            }
            return chain;
        };

        std::vector<std::vector<uint32_t>> chains;
        for (uint32_t s : starts) {
            // May already be consumed as the far end of an earlier chain.
            bool has_free = false;
            for (uint32_t ei : adj[s])
                if (!edge_used[ei]) has_free = true;
            if (!has_free) continue;
            chains.push_back(trace_chain(s));
        }
        // Leftover loops (no degree-1 start): start at the lexicographically
        // smallest unused-edge vertex.
        for (;;) {
            uint32_t best = UINT32_MAX;
            for (uint32_t ei = 0; ei < edges.size(); ++ei) {
                if (edge_used[ei]) continue;
                for (uint32_t v : {edges[ei].a, edges[ei].b})
                    if (best == UINT32_MAX || lex_less(v, best)) best = v;
            }
            if (best == UINT32_MAX) break;
            chains.push_back(trace_chain(best));
        }

        for (auto& chain : chains) {
            if (chain.size() < 2) continue;
            // Canonical order: from the lexicographically smaller endpoint,
            // so both sectors trace (and round) identically.
            if (lex_less(chain.back(), chain.front()))
                std::reverse(chain.begin(), chain.end());
            const size_t n = chain.size();
            // Chain-smoothed GEOMETRY: box-filter the chain's world points
            // over a +-2 m arc window before measuring anything. A voxel
            // rim zigzags at cell scale (especially descending a cliff), and
            // tracing the raw polyline accumulates every wiggle as spread —
            // measured as jagged pins that fold the first interior ring all
            // along the border. The macro shape (the 300 m descent) survives
            // the filter; the 2-4 m noise does not.
            std::vector<double> px(n), py(n), pz(n), raw_t(n, 0.0);
            for (size_t i = 0; i < n; ++i) {
                const float3 w = world(chain[i]);
                px[i] = w.x;
                py[i] = w.y;
                pz[i] = w.z;
                if (i)
                    raw_t[i] =
                        raw_t[i - 1] +
                        std::sqrt((px[i] - px[i - 1]) * (px[i] - px[i - 1]) +
                                  (py[i] - py[i - 1]) * (py[i] - py[i - 1]) +
                                  (pz[i] - pz[i - 1]) * (pz[i] - pz[i - 1]));
            }
            const double kGeomWindow = 2.0;
            std::vector<double> qx(n), qy(n), qz(n);
            for (size_t i = 0; i < n; ++i) {
                double sx = 0, sy = 0, sz = 0, cnt = 0;
                for (size_t j = i;; --j) {
                    if (raw_t[i] - raw_t[j] > kGeomWindow) break;
                    sx += px[j];
                    sy += py[j];
                    sz += pz[j];
                    cnt += 1;
                    if (j == 0) break;
                }
                for (size_t j = i + 1; j < n; ++j) {
                    if (raw_t[j] - raw_t[i] > kGeomWindow) break;
                    sx += px[j];
                    sy += py[j];
                    sz += pz[j];
                    cnt += 1;
                }
                qx[i] = sx / cnt;
                qy[i] = sy / cnt;
                qz[i] = sz / cnt;
            }
            // Segment lengths (smoothed 3D and XZ) and XZ directions.
            std::vector<double> seg_len(n - 1), seg_xz(n - 1);
            std::vector<double2> seg_dir(n - 1);
            for (size_t i = 0; i + 1 < n; ++i) {
                const double dx = qx[i + 1] - qx[i];
                const double dy = qy[i + 1] - qy[i];
                const double dz = qz[i + 1] - qz[i];
                seg_len[i] = std::sqrt(dx * dx + dy * dy + dz * dz);
                seg_xz[i] = std::sqrt(dx * dx + dz * dz);
                seg_dir[i] = double2{dx, dz};
            }
            // Chain-smoothed XZ directions: arc-length box window (+-2 m)
            // over the raw XZ deltas, then normalized; a window that comes
            // out degenerate (a vertically zig-zagging cliff border) carries
            // the previous smoothed direction forward.
            const double kWindow = 2.0;
            std::vector<double2> dir(n - 1);
            double2 carry{0.0, 0.0};
            bool have_carry = false;
            for (size_t i = 0; i + 1 < n; ++i) {
                double2 acc{0.0, 0.0};
                // walk backwards / forwards within the arc-length window
                double budget = kWindow;
                for (size_t j = i;; --j) {
                    acc.x += seg_dir[j].x;
                    acc.y += seg_dir[j].y;
                    budget -= seg_len[j];
                    if (j == 0 || budget <= 0.0) break;
                }
                budget = kWindow;
                for (size_t j = i + 1; j + 1 < n && budget > 0.0; ++j) {
                    acc.x += seg_dir[j].x;
                    acc.y += seg_dir[j].y;
                    budget -= seg_len[j];
                }
                const double l = std::sqrt(acc.x * acc.x + acc.y * acc.y);
                if (l > 1e-9) {
                    dir[i] = double2{acc.x / l, acc.y / l};
                    carry = dir[i];
                    have_carry = true;
                } else if (have_carry) {
                    dir[i] = carry;
                } else {
                    // No usable direction yet: fall back to the side axis.
                    dir[i] = (side < 2) ? double2{0.0, 1.0} : double2{1.0, 0.0};
                }
            }
            // Directional cumulative trace in canonical order, then anchor:
            // uv = trace shifted so the chain's lexicographically-smallest
            // vertex lands exactly on its world XZ. NO endpoint
            // canonicalization: a chain crossing a cliff wall spreads to its
            // true 3D arc length — that spread is the whole point (§4.2) —
            // and pulling its endpoints back to world-XZ corners would crush
            // the spread back out (measured: the wall then cannot beat the
            // world-XZ baseline at all). The price is that chains MEETING at
            // a sector corner can disagree there; see the priority note
            // below ("corner knots", the spec's accepted residual (a)).
            const bool is_loop = chain.front() == chain.back() && n > 2;
            // Per-segment geometric-mean spread: each segment advances by
            // sqrt(len3d * lenxz) — the LOCAL conformal compromise between
            // spreading a cliff crossing to its full 3D arc (which
            // concentrates an untenable conflict where the chain meets an
            // unspread cross-slope chain at a corner; measured ~19% folds)
            // and no spread at all (the world-XZ compression this field
            // exists to remove). Exactly len3d == lenxz on flat ground, and
            // it adapts along a chain whose slope varies, which one global
            // per-chain factor cannot. Floored so a near-vertical stretch
            // still advances (a zero-length uv stretch would be an infinite
            // border stretch of its own).
            std::vector<double2> tr(n);
            std::vector<double> t(n, 0.0);
            tr[0] = double2{0.0, 0.0};
            for (size_t i = 0; i + 1 < n; ++i) {
                const double step =
                    seg_len[i] > 1e-12
                        ? std::max(std::sqrt(seg_len[i] * seg_xz[i]),
                                   0.25 * seg_len[i])
                        : 0.0;
                tr[i + 1] = double2{tr[i].x + step * dir[i].x,
                                    tr[i].y + step * dir[i].y};
                t[i + 1] = t[i] + seg_len[i];
            }
            if (is_loop) {
                // A closed loop must close: distribute the trace's closure
                // error linearly over arc length.
                const double ex = tr[0].x - tr[n - 1].x;
                const double ez = tr[0].y - tr[n - 1].y;
                const double total = t[n - 1] > 1e-9 ? t[n - 1] : 1.0;
                for (size_t i = 0; i < n; ++i) {
                    tr[i].x += ex * (t[i] / total);
                    tr[i].y += ez * (t[i] / total);
                }
            }
            // Anchor at the ARC-MIDPOINT vertex's world XZ. The spec's
            // as-written anchor (the lex-smallest vertex) parks the whole
            // residual spread-vs-corner conflict at ONE end of the chain;
            // midpoint anchoring splits it across both ends, halving each
            // knot. Deterministic and chain-local, so both sectors of a
            // shared border still compute identical pins.
            size_t anchor = 0;
            {
                const double half = t[n - 1] * 0.5;
                double bd = 1e300;
                for (size_t i = 0; i < n - (is_loop ? 1 : 0); ++i) {
                    const double d = std::fabs(t[i] - half);
                    if (d < bd) {
                        bd = d;
                        anchor = i;
                    }
                }
            }
            const float3 wa = world(chain[anchor]);
            const double bx = double(wa.x) - tr[anchor].x;
            const double bz = double(wa.z) - tr[anchor].y;
            for (size_t i = 0; i < n; ++i) {
                const uint32_t v = chain[i];
                pins.pinned[v] = 1;
                pins.uv[v] = double2{tr[i].x + bx, tr[i].y + bz};
            }
        }
    }
}

// ---------------------------------------------------------------------------
// ARAP machinery.
// ---------------------------------------------------------------------------
struct TriRef {
    // Isometric 2D reference coords (x0 = origin).
    float x1x, x2x, x2y;       // x1 = (x1x, 0)
    float w[3];                // clamped cotan weight per edge (i, i+1)
};

struct Csr {
    // Symmetric Laplacian in adjacency-list form, per vertex, neighbor
    // entries sorted by index. diag[i] and (nbr, w) lists.
    std::vector<float> diag;
    std::vector<uint32_t> offsets;
    std::vector<uint32_t> nbr;
    std::vector<float> w;
};

// `orientation` (+1/-1): the sign the uv-plane triangle areas take under the
// field's convention. The engine's meshes wind counter-clockwise seen from
// OUTSIDE, and with uv anchored to world XZ an up-facing triangle's (u, v)
// signed area is NEGATIVE (terrain_mesher's push_hf_tri documents the same
// xz-plane sign). The reference triangles must be built with the SAME
// orientation as the uv image, or the local step's det+1 rotation fit fights
// a reflection on every triangle and the solve shreds. The caller measures
// the majority sign of the init uv and passes it here.
void build_reference(const SolveMesh& mesh, float orientation,
                     std::vector<TriRef>& refs) {
    const size_t nt = mesh.tri_count();
    refs.resize(nt);
    for (size_t t = 0; t < nt; ++t) {
        const uint32_t* i = &mesh.indices[t * 3];
        const float3 p0 = mesh.positions[i[0]];
        const float3 e1 = mesh.positions[i[1]] - p0;
        const float3 e2 = mesh.positions[i[2]] - p0;
        const float l1 = std::sqrt(dot(e1, e1));
        const float3 nvec = cross(e1, e2);
        const float area2 = std::sqrt(dot(nvec, nvec));
        TriRef r;
        r.x1x = l1;
        r.x2x = l1 > 0.0f ? dot(e1, e2) / l1 : 0.0f;
        r.x2y = l1 > 0.0f ? orientation * area2 / l1 : 0.0f;
        // Cotan weights: edge (i, i+1) is opposite vertex i+2. cot = cos/sin
        // over the reference triangle; clamped positive for the Tutte-like
        // init structure (documented distortion trade). Mirroring preserves
        // angle magnitudes, so the weights are orientation-independent.
        const float2 x[3] = {make_float2(0.0f, 0.0f), make_float2(r.x1x, 0.0f),
                             make_float2(r.x2x, r.x2y)};
        for (int e = 0; e < 3; ++e) {
            const float2 a = x[e], b = x[(e + 1) % 3], c = x[(e + 2) % 3];
            const float2 ca = make_float2(a.x - c.x, a.y - c.y);
            const float2 cb = make_float2(b.x - c.x, b.y - c.y);
            const float cosv = ca.x * cb.x + ca.y * cb.y;
            const float sinv = std::fabs(ca.x * cb.y - ca.y * cb.x);
            float cot = sinv > 1e-9f ? cosv / sinv : 20.0f;
            r.w[e] = std::min(std::max(cot, 0.01f), 20.0f) * 0.5f;
        }
        refs[t] = r;
    }
}

void build_laplacian(const SolveMesh& mesh, const std::vector<TriRef>& refs,
                     Csr& L) {
    const size_t nv = mesh.vert_count();
    const size_t nt = mesh.tri_count();
    // Gather (i, j, w) contributions, then bucket per vertex.
    struct Entry {
        uint32_t i, j;
        float w;
    };
    std::vector<Entry> entries;
    entries.reserve(nt * 6);
    for (size_t t = 0; t < nt; ++t) {
        const uint32_t* i = &mesh.indices[t * 3];
        for (int e = 0; e < 3; ++e) {
            const uint32_t a = i[e], b = i[(e + 1) % 3];
            const float w = refs[t].w[e];
            entries.push_back({a, b, w});
            entries.push_back({b, a, w});
        }
    }
    std::vector<uint32_t> counts(nv, 0);
    for (const Entry& e : entries) counts[e.i]++;
    L.offsets.assign(nv + 1, 0);
    for (size_t v = 0; v < nv; ++v) L.offsets[v + 1] = L.offsets[v] + counts[v];
    std::vector<uint32_t> cursor(L.offsets.begin(), L.offsets.end() - 1);
    std::vector<Entry> bucketed(entries.size());
    for (const Entry& e : entries) bucketed[cursor[e.i]++] = e;
    // Sort each vertex's entries by neighbor and merge duplicates.
    L.diag.assign(nv, 0.0f);
    L.nbr.clear();
    L.w.clear();
    std::vector<uint32_t> new_offsets(nv + 1, 0);
    for (size_t v = 0; v < nv; ++v) {
        const uint32_t begin = L.offsets[v], end = L.offsets[v + 1];
        std::sort(bucketed.begin() + begin, bucketed.begin() + end,
                  [](const Entry& a, const Entry& b) { return a.j < b.j; });
        for (uint32_t k = begin; k < end;) {
            uint32_t j = bucketed[k].j;
            float w = 0.0f;
            while (k < end && bucketed[k].j == j) w += bucketed[k++].w;
            L.nbr.push_back(j);
            L.w.push_back(w);
            L.diag[v] += w;
        }
        new_offsets[v + 1] = uint32_t(L.nbr.size());
    }
    L.offsets = std::move(new_offsets);
}

// Jacobi-preconditioned CG on the pinned Laplacian system, both uv
// components at once. Solves L x = b over free vertices with x fixed at pins.
// b_extra is the ARAP rotation rhs (empty => harmonic solve, rhs 0).
void cg_solve(const Csr& L, const PinSet& pins,
              const std::vector<float2>& b_extra, int max_iters, float tol,
              std::vector<float2>& x) {
    const size_t nv = L.diag.size();
    auto apply = [&](const std::vector<float2>& in, std::vector<float2>& out) {
        // out = A * in over free verts (A = L with pinned columns dropped;
        // pinned rows identity, handled by skipping them).
        for (size_t v = 0; v < nv; ++v) {
            if (pins.pinned[v]) {
                out[v] = make_float2(0.0f, 0.0f);
                continue;
            }
            float ox = L.diag[v] * in[v].x;
            float oy = L.diag[v] * in[v].y;
            for (uint32_t k = L.offsets[v]; k < L.offsets[v + 1]; ++k) {
                const uint32_t j = L.nbr[k];
                if (pins.pinned[j]) continue;
                ox -= L.w[k] * in[j].x;
                oy -= L.w[k] * in[j].y;
            }
            out[v] = make_float2(ox, oy);
        }
    };
    // rhs: b_extra + contributions of pinned neighbors.
    std::vector<float2> b(nv, make_float2(0.0f, 0.0f));
    for (size_t v = 0; v < nv; ++v) {
        if (pins.pinned[v]) continue;
        float bx = b_extra.empty() ? 0.0f : b_extra[v].x;
        float by = b_extra.empty() ? 0.0f : b_extra[v].y;
        for (uint32_t k = L.offsets[v]; k < L.offsets[v + 1]; ++k) {
            const uint32_t j = L.nbr[k];
            if (!pins.pinned[j]) continue;
            bx += L.w[k] * float(pins.uv[j].x);
            by += L.w[k] * float(pins.uv[j].y);
        }
        b[v] = make_float2(bx, by);
    }
    // Pin values into x (exact).
    for (size_t v = 0; v < nv; ++v)
        if (pins.pinned[v])
            x[v] = make_float2(float(pins.uv[v].x), float(pins.uv[v].y));

    std::vector<float2> r(nv), z(nv), p(nv), Ap(nv);
    apply(x, Ap);
    for (size_t v = 0; v < nv; ++v)
        r[v] = pins.pinned[v] ? make_float2(0.0f, 0.0f)
                              : make_float2(b[v].x - Ap[v].x, b[v].y - Ap[v].y);
    auto precond = [&](const std::vector<float2>& in, std::vector<float2>& out) {
        for (size_t v = 0; v < nv; ++v) {
            const float d = L.diag[v] > 1e-12f ? 1.0f / L.diag[v] : 0.0f;
            out[v] = make_float2(in[v].x * d, in[v].y * d);
        }
    };
    precond(r, z);
    p = z;
    double rz = 0.0;
    for (size_t v = 0; v < nv; ++v)
        rz += double(r[v].x) * z[v].x + double(r[v].y) * z[v].y;
    double r0 = 0.0;
    for (size_t v = 0; v < nv; ++v)
        r0 += double(r[v].x) * r[v].x + double(r[v].y) * r[v].y;
    const double stop = double(tol) * double(tol) * (r0 > 0.0 ? r0 : 1.0);
    for (int it = 0; it < max_iters; ++it) {
        double rr = 0.0;
        for (size_t v = 0; v < nv; ++v)
            rr += double(r[v].x) * r[v].x + double(r[v].y) * r[v].y;
        if (rr <= stop) break;
        apply(p, Ap);
        double pAp = 0.0;
        for (size_t v = 0; v < nv; ++v)
            pAp += double(p[v].x) * Ap[v].x + double(p[v].y) * Ap[v].y;
        if (!(pAp > 1e-30)) break;
        const float alpha = float(rz / pAp);
        for (size_t v = 0; v < nv; ++v) {
            if (pins.pinned[v]) continue;
            x[v].x += alpha * p[v].x;
            x[v].y += alpha * p[v].y;
            r[v].x -= alpha * Ap[v].x;
            r[v].y -= alpha * Ap[v].y;
        }
        precond(r, z);
        double rz_new = 0.0;
        for (size_t v = 0; v < nv; ++v)
            rz_new += double(r[v].x) * z[v].x + double(r[v].y) * z[v].y;
        const float beta = rz > 1e-30 ? float(rz_new / rz) : 0.0f;
        rz = rz_new;
        for (size_t v = 0; v < nv; ++v)
            p[v] = make_float2(z[v].x + beta * p[v].x, z[v].y + beta * p[v].y);
    }
}

// A fold is a triangle whose uv orientation OPPOSES the field's convention
// (`orientation`, the majority sign — see build_reference). A folded
// triangle renders locally mirrored stochastic content, which §4.3 expects
// to be invisible at the gated rate; what matters is counting them honestly.
inline int fold_count(const SolveMesh& mesh, const std::vector<float2>& uv,
                      float orientation, std::vector<uint8_t>* mask) {
    const size_t nt = mesh.tri_count();
    if (mask) mask->assign(nt, 0);
    int folds = 0;
    for (size_t t = 0; t < nt; ++t) {
        const uint32_t* i = &mesh.indices[t * 3];
        const float2 a = uv[i[0]], b = uv[i[1]], c = uv[i[2]];
        const float area2 =
            (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (area2 * orientation <= 0.0f) {
            ++folds;
            if (mask) (*mask)[t] = 1;
        }
    }
    return folds;
}

// Majority uv-orientation sign over the mesh (+1 or -1, never 0).
inline float majority_orientation(const SolveMesh& mesh,
                                  const std::vector<float2>& uv) {
    const size_t nt = mesh.tri_count();
    double pos = 0.0, neg = 0.0;
    for (size_t t = 0; t < nt; ++t) {
        const uint32_t* i = &mesh.indices[t * 3];
        const float2 a = uv[i[0]], b = uv[i[1]], c = uv[i[2]];
        const float area2 =
            (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (area2 > 0.0f) pos += mesh.tri_area[t];
        else if (area2 < 0.0f) neg += mesh.tri_area[t];
    }
    return pos >= neg ? 1.0f : -1.0f;
}

// Per-triangle forward Jacobian rows (world -> uv), via the closed form
// g = (a*(e2 x n) - b*(e1 x n)) / |n|^2 with n = e1 x e2.
inline void tri_jacobian(const float3& p0, const float3& p1, const float3& p2,
                         const float2& uv0, const float2& uv1,
                         const float2& uv2, float3& gu, float3& gv) {
    const float3 e1 = p1 - p0, e2 = p2 - p0;
    const float3 n = cross(e1, e2);
    const float n2 = dot(n, n);
    if (n2 < 1e-20f) {
        gu = f3(0, 0, 0);
        gv = f3(0, 0, 0);
        return;
    }
    const float3 c2 = cross(e2, n), c1 = cross(e1, n);
    const float inv = 1.0f / n2;
    gu = (c2 * (uv1.x - uv0.x) - c1 * (uv2.x - uv0.x)) * inv;
    gv = (c2 * (uv1.y - uv0.y) - c1 * (uv2.y - uv0.y)) * inv;
}

struct WeightedValue {
    float v, w;
};
float weighted_percentile(std::vector<WeightedValue>& vals, float pct) {
    if (vals.empty()) return 0.0f;
    std::sort(vals.begin(), vals.end(),
              [](const WeightedValue& a, const WeightedValue& b) {
                  return a.v < b.v;
              });
    double total = 0.0;
    for (const auto& x : vals) total += x.w;
    const double target = total * double(pct);
    double acc = 0.0;
    for (const auto& x : vals) {
        acc += x.w;
        if (acc >= target) return x.v;
    }
    return vals.back().v;
}

void compute_stats(const SolveMesh& mesh, const std::vector<float2>& uv,
                   FieldStats& stats) {
    const size_t nt = mesh.tri_count();
    std::vector<WeightedValue> stretch, compress, aniso, dj;
    stretch.reserve(nt);
    compress.reserve(nt);
    aniso.reserve(nt);
    std::vector<float3> gu(nt), gv(nt);
    uint32_t folds = 0;
    for (size_t t = 0; t < nt; ++t) {
        const uint32_t* i = &mesh.indices[t * 3];
        tri_jacobian(mesh.positions[i[0]], mesh.positions[i[1]],
                     mesh.positions[i[2]], uv[i[0]], uv[i[1]], uv[i[2]], gu[t],
                     gv[t]);
        // Singular values of J (2x3, tangent-restricted): eig of J J^T.
        const float a = dot(gu[t], gu[t]);
        const float b = dot(gu[t], gv[t]);
        const float c = dot(gv[t], gv[t]);
        const float tr = a + c;
        const float det = a * c - b * b;
        const float disc = std::sqrt(std::max(0.0f, tr * tr * 0.25f - det));
        const float l1 = tr * 0.5f + disc;  // larger eigenvalue
        const float l2 = std::max(tr * 0.5f - disc, 1e-12f);
        const float sJ1 = std::sqrt(std::max(l1, 1e-12f));  // J's larger sv
        const float sJ2 = std::sqrt(l2);
        // Content map phi = J^-1 on the tangent plane: sigma1(phi) = 1/sJ2.
        const float s1 = 1.0f / sJ2;  // stretch
        const float s2 = 1.0f / sJ1;  // smaller phi sv
        const float w = mesh.tri_area[t];
        stretch.push_back({s1, w});
        compress.push_back({1.0f / std::max(s2, 1e-6f), w});
        aniso.push_back({s1 / std::max(s2, 1e-6f), w});
    }
    // Folds: minority-orientation triangles (see fold_count).
    {
        const float orient = majority_orientation(mesh, uv);
        for (size_t t = 0; t < nt; ++t) {
            const uint32_t* i = &mesh.indices[t * 3];
            const float2 A = uv[i[0]], B = uv[i[1]], C = uv[i[2]];
            const float area2 =
                (B.x - A.x) * (C.y - A.y) - (B.y - A.y) * (C.x - A.x);
            if (area2 * orient <= 0.0f) ++folds;
        }
    }
    // Adjacent-triangle delta-J across interior manifold edges.
    std::unordered_map<uint64_t, uint32_t> first_tri;
    first_tri.reserve(nt * 3);
    for (size_t t = 0; t < nt; ++t) {
        const uint32_t* i = &mesh.indices[t * 3];
        for (int e = 0; e < 3; ++e) {
            const uint64_t k = edge_key(i[e], i[(e + 1) % 3]);
            auto it = first_tri.find(k);
            if (it == first_tri.end()) {
                first_tri.emplace(k, uint32_t(t));
            } else if (it->second != UINT32_MAX) {
                const uint32_t o = it->second;
                const float3 du = gu[t] - gu[o];
                const float3 dv = gv[t] - gv[o];
                const float d = std::sqrt(dot(du, du) + dot(dv, dv));
                dj.push_back({d, mesh.tri_area[t] + mesh.tri_area[o]});
                it->second = UINT32_MAX;  // non-manifold thirds ignored
            }
        }
    }
    stats.tris = uint32_t(nt);
    stats.solved_tris = uint32_t(nt);
    stats.verts = uint32_t(mesh.vert_count());
    stats.folds = folds;
    stats.stretch_p50 = weighted_percentile(stretch, 0.50f);
    stats.stretch_p95 = weighted_percentile(stretch, 0.95f);
    stats.stretch_max = stretch.empty() ? 0.0f : stretch.back().v;
    stats.compress_p50 = weighted_percentile(compress, 0.50f);
    stats.compress_p95 = weighted_percentile(compress, 0.95f);
    stats.aniso_p50 = weighted_percentile(aniso, 0.50f);
    stats.aniso_p95 = weighted_percentile(aniso, 0.95f);
    stats.dj_p50 = weighted_percentile(dj, 0.50f);
    stats.dj_p95 = weighted_percentile(dj, 0.95f);
}

// ---------------------------------------------------------------------------
// Census.
// ---------------------------------------------------------------------------
std::atomic<uint64_t> g_sectors{0}, g_solved{0}, g_solve_us{0},
    g_evaluate_us{0}, g_tris{0}, g_folds{0};

// f32 -> f16 (round to nearest even), no dependencies.
inline uint16_t f32_to_f16(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    uint32_t mag = x & 0x7FFFFFFFu;
    if (mag >= 0x47800000u)  // overflow / inf / nan
        return uint16_t(sign | (mag > 0x7F800000u ? 0x7E00u : 0x7C00u));
    if (mag < 0x38800000u) {  // subnormal half
        const uint32_t shift = 126u - (mag >> 23);
        if (shift > 24) return uint16_t(sign);
        uint32_t m = (mag & 0x7FFFFFu) | 0x800000u;
        const uint32_t rounded = m >> shift;
        const uint32_t rem = m & ((1u << shift) - 1u);
        const uint32_t half = 1u << (shift - 1);
        uint32_t out = rounded;
        if (rem > half || (rem == half && (rounded & 1u))) ++out;
        return uint16_t(sign | out);
    }
    const uint32_t exp = ((mag >> 23) - 112u) << 10;
    const uint32_t man = (mag >> 13) & 0x3FFu;
    uint16_t out = uint16_t(sign | exp | man);
    const uint32_t rem = mag & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (out & 1u))) ++out;
    return out;
}

// Octahedral encode of a unit vector, matching the GLSL decode in
// raster.vert (oct wrap for the lower hemisphere).
inline void oct_encode(const float3& v, float& ox, float& oy) {
    const float l1 = std::fabs(v.x) + std::fabs(v.y) + std::fabs(v.z);
    const float inv = l1 > 1e-12f ? 1.0f / l1 : 0.0f;
    float px = v.x * inv, py = v.y * inv;
    if (v.z < 0.0f) {
        const float ax = std::fabs(px), ay = std::fabs(py);
        const float sx = px >= 0.0f ? 1.0f : -1.0f;
        const float sy = py >= 0.0f ? 1.0f : -1.0f;
        px = (1.0f - ay) * sx;
        py = (1.0f - ax) * sy;
    }
    ox = px;
    oy = py;
}

inline uint32_t pack_half2(float a, float b) {
    return uint32_t(f32_to_f16(a)) | (uint32_t(f32_to_f16(b)) << 16);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API.
// ---------------------------------------------------------------------------

bool border_pins(const Tri* tris, size_t tri_count, const uint8_t* skirt_mask,
                 const SolveOptions& opts, std::vector<float3>& out_positions,
                 std::vector<BorderPin>& out_pins) {
    SolveMesh mesh;
    if (!build_solve_mesh(tris, tri_count, skirt_mask, mesh)) return false;
    PinSet pins;
    compute_border_pins(mesh, opts, pins);
    out_positions = mesh.positions;
    out_pins.clear();
    for (uint32_t v = 0; v < mesh.vert_count(); ++v) {
        if (!pins.pinned[v]) continue;
        out_pins.push_back(
            {v, make_float2(float(pins.uv[v].x), float(pins.uv[v].y))});
    }
    return true;
}

FieldStats world_xz_stats(const Tri* tris, size_t tri_count,
                          const uint8_t* skirt_mask, const SolveOptions& opts) {
    FieldStats stats;
    SolveMesh mesh;
    if (!build_solve_mesh(tris, tri_count, skirt_mask, mesh)) return stats;
    std::vector<float2> uv(mesh.vert_count());
    for (size_t v = 0; v < mesh.vert_count(); ++v) {
        const float3& p = mesh.positions[v];
        uv[v] = make_float2(float(opts.anchor_x + double(p.x)),
                            float(opts.anchor_z + double(p.z)));
    }
    compute_stats(mesh, uv, stats);
    return stats;
}

bool solve(const Tri* tris, size_t tri_count, const uint8_t* skirt_mask,
           const SolveOptions& opts, Field& out) {
    const auto t0 = std::chrono::steady_clock::now();
    g_sectors.fetch_add(1, std::memory_order_relaxed);
    out = Field{};

    SolveMesh mesh;
    if (!build_solve_mesh(tris, tri_count, skirt_mask, mesh)) return false;

    PinSet pins;
    compute_border_pins(mesh, opts, pins);
    // A sector with no pinned border at all (fully interior hole mesh — not
    // a terrain sector shape we produce) still solves: fix the lex-smallest
    // vertex to its anchored world XZ so the system is non-singular.
    bool any_pin = false;
    for (uint8_t p : pins.pinned)
        if (p) {
            any_pin = true;
            break;
        }
    if (!any_pin) {
        uint32_t best = 0;
        for (uint32_t v = 1; v < mesh.vert_count(); ++v) {
            const float3 &a = mesh.positions[v], &b = mesh.positions[best];
            if (a.x < b.x || (a.x == b.x && (a.y < b.y ||
                (a.y == b.y && a.z < b.z))))
                best = v;
        }
        pins.pinned[best] = 1;
        pins.uv[best] =
            double2{opts.anchor_x + double(mesh.positions[best].x),
                    opts.anchor_z + double(mesh.positions[best].z)};
    }

    const size_t nv = mesh.vert_count();
    const size_t nt = mesh.tri_count();

    // Init: anchored world XZ guess, refined into the pinned harmonic field.
    std::vector<float2> uv(nv);
    for (size_t v = 0; v < nv; ++v) {
        const float3& p = mesh.positions[v];
        uv[v] = make_float2(float(opts.anchor_x + double(p.x)),
                            float(opts.anchor_z + double(p.z)));
    }
    // The field's uv orientation convention comes from the init (for the
    // engine's CCW-from-outside terrain and XZ-anchored uv this is -1); the
    // reference triangles are built with the SAME orientation so the local
    // step's rotations are rotations, not reflections.
    const float orient = majority_orientation(mesh, uv);

    std::vector<TriRef> refs;
    build_reference(mesh, orient, refs);
    Csr L;
    build_laplacian(mesh, refs, L);

    cg_solve(L, pins, {}, opts.init_cg_iterations, opts.cg_tolerance, uv);

    // Local/global ARAP.
    std::vector<float2> rhs(nv);
    for (int iter = 0; iter < opts.arap_iterations; ++iter) {
        std::fill(rhs.begin(), rhs.end(), make_float2(0.0f, 0.0f));
        for (size_t t = 0; t < nt; ++t) {
            const uint32_t* idx = &mesh.indices[t * 3];
            const TriRef& r = refs[t];
            const float2 x[3] = {make_float2(0.0f, 0.0f),
                                 make_float2(r.x1x, 0.0f),
                                 make_float2(r.x2x, r.x2y)};
            // Local fit: ARAP's best rotation, det +1 by construction:
            // (cos, sin) ~ (S00 + S11, S10 - S01), the closed 2x2 form.
            // A similarity (rotation + scale) local step was tried and
            // rejected: it drives the whole wall interior toward a uniform
            // conformal shrink that the flat-along-the-chain contour pins —
            // which can never know the wall's dip from chain-local data —
            // cannot follow, and the mismatch tears the borders (measured
            // ~19% folds). Rigidity instead absorbs the un-spreadable
            // direction as SMOOTH anisotropy, which §2.4 measures as the
            // invisible kind of error on stochastic content.
            float s00 = 0, s01 = 0, s10 = 0, s11 = 0;
            for (int e = 0; e < 3; ++e) {
                const int i = e, j = (e + 1) % 3;
                const float w = r.w[e];
                const float ux = uv[idx[i]].x - uv[idx[j]].x;
                const float uy = uv[idx[i]].y - uv[idx[j]].y;
                const float xx = x[i].x - x[j].x;
                const float xy = x[i].y - x[j].y;
                s00 += w * ux * xx;
                s01 += w * ux * xy;
                s10 += w * uy * xx;
                s11 += w * uy * xy;
            }
            const float cs = s00 + s11, sn = s10 - s01;
            const float len = std::sqrt(cs * cs + sn * sn);
            const float c = len > 1e-12f ? cs / len : 1.0f;
            const float s = len > 1e-12f ? sn / len : 0.0f;
            for (int e = 0; e < 3; ++e) {
                const int i = e, j = (e + 1) % 3;
                const float w = r.w[e];
                const float xx = x[i].x - x[j].x;
                const float xy = x[i].y - x[j].y;
                const float rx = c * xx - s * xy;
                const float ry = s * xx + c * xy;
                rhs[idx[i]].x += w * rx;
                rhs[idx[i]].y += w * ry;
                rhs[idx[j]].x -= w * rx;
                rhs[idx[j]].y -= w * ry;
            }
        }
        cg_solve(L, pins, rhs, opts.cg_iterations, opts.cg_tolerance, uv);
    }

    // Fold-relax: bounded local Tutte relaxation of folded triangles' free
    // vertices, accepted only while the fold count strictly decreases.
    std::vector<uint8_t> fold_mask;
    int folds = fold_count(mesh, uv, orient, &fold_mask);
    for (int pass = 0; pass < opts.fold_relax_passes && folds > 0; ++pass) {
        std::vector<uint8_t> touch(nv, 0);
        for (size_t t = 0; t < nt; ++t) {
            if (!fold_mask[t]) continue;
            const uint32_t* i = &mesh.indices[t * 3];
            for (int c = 0; c < 3; ++c)
                if (!pins.pinned[i[c]]) touch[i[c]] = 1;
        }
        std::vector<float2> next = uv;
        for (size_t v = 0; v < nv; ++v) {
            if (!touch[v]) continue;
            float sx = 0, sy = 0, sw = 0;
            for (uint32_t k = L.offsets[v]; k < L.offsets[v + 1]; ++k) {
                sx += uv[L.nbr[k]].x;
                sy += uv[L.nbr[k]].y;
                sw += 1.0f;
            }
            if (sw > 0.0f)
                next[v] = make_float2(0.5f * uv[v].x + 0.5f * sx / sw,
                                      0.5f * uv[v].y + 0.5f * sy / sw);
        }
        const int new_folds = fold_count(mesh, next, orient, &fold_mask);
        if (new_folds < folds) {
            uv = std::move(next);
            folds = new_folds;
        } else {
            fold_count(mesh, uv, orient, &fold_mask);  // restore for stats
            break;
        }
    }

    // Assemble the output field.
    out.positions = std::move(mesh.positions);
    out.indices = std::move(mesh.indices);
    SolveMesh view;  // stats/jacobians need the mesh shape back
    view.positions = out.positions;
    view.indices = out.indices;
    view.tri_area = std::move(mesh.tri_area);
    compute_stats(view, uv, out.stats);

    // Per-vertex uv + area-weighted Jacobian rows.
    out.verts.assign(out.positions.size(), VertexField{});
    std::vector<float> wsum(out.positions.size(), 0.0f);
    for (size_t v = 0; v < out.positions.size(); ++v) out.verts[v].uv = uv[v];
    const size_t nt2 = out.indices.size() / 3;
    for (size_t t = 0; t < nt2; ++t) {
        const uint32_t* i = &out.indices[t * 3];
        float3 gu, gv;
        tri_jacobian(out.positions[i[0]], out.positions[i[1]],
                     out.positions[i[2]], uv[i[0]], uv[i[1]], uv[i[2]], gu, gv);
        const float w = view.tri_area[t];
        for (int c = 0; c < 3; ++c) {
            out.verts[i[c]].gu = out.verts[i[c]].gu + gu * w;
            out.verts[i[c]].gv = out.verts[i[c]].gv + gv * w;
            wsum[i[c]] += w;
        }
    }
    for (size_t v = 0; v < out.positions.size(); ++v) {
        if (wsum[v] > 1e-12f) {
            out.verts[v].gu = out.verts[v].gu * (1.0f / wsum[v]);
            out.verts[v].gv = out.verts[v].gv * (1.0f / wsum[v]);
        }
    }

    // Uniform XZ triangle grid for evaluate().
    {
        float mnx = 1e30f, mnz = 1e30f, mxx = -1e30f, mxz = -1e30f;
        for (const float3& p : out.positions) {
            mnx = std::min(mnx, p.x);
            mxx = std::max(mxx, p.x);
            mnz = std::min(mnz, p.z);
            mxz = std::max(mxz, p.z);
        }
        out.grid_min_x = mnx;
        out.grid_min_z = mnz;
        out.grid_cell = 4.0f;
        out.grid_nx =
            std::max(1, int((mxx - mnx) / out.grid_cell) + 1);
        out.grid_nz =
            std::max(1, int((mxz - mnz) / out.grid_cell) + 1);
        std::vector<uint32_t> counts(size_t(out.grid_nx) * out.grid_nz, 0);
        auto cell_range = [&](size_t t, int& cx0, int& cx1, int& cz0,
                              int& cz1) {
            const uint32_t* i = &out.indices[t * 3];
            float tx0 = 1e30f, tx1 = -1e30f, tz0 = 1e30f, tz1 = -1e30f;
            for (int c = 0; c < 3; ++c) {
                const float3& p = out.positions[i[c]];
                tx0 = std::min(tx0, p.x);
                tx1 = std::max(tx1, p.x);
                tz0 = std::min(tz0, p.z);
                tz1 = std::max(tz1, p.z);
            }
            cx0 = std::max(0, int((tx0 - out.grid_min_x) / out.grid_cell));
            cx1 = std::min(out.grid_nx - 1,
                           int((tx1 - out.grid_min_x) / out.grid_cell));
            cz0 = std::max(0, int((tz0 - out.grid_min_z) / out.grid_cell));
            cz1 = std::min(out.grid_nz - 1,
                           int((tz1 - out.grid_min_z) / out.grid_cell));
        };
        for (size_t t = 0; t < nt2; ++t) {
            int cx0, cx1, cz0, cz1;
            cell_range(t, cx0, cx1, cz0, cz1);
            for (int cz = cz0; cz <= cz1; ++cz)
                for (int cx = cx0; cx <= cx1; ++cx)
                    counts[size_t(cz) * out.grid_nx + cx]++;
        }
        out.grid_offsets.assign(size_t(out.grid_nx) * out.grid_nz + 1, 0);
        for (size_t c = 0; c < counts.size(); ++c)
            out.grid_offsets[c + 1] = out.grid_offsets[c] + counts[c];
        out.grid_tris.assign(out.grid_offsets.back(), 0);
        std::vector<uint32_t> cursor(out.grid_offsets.begin(),
                                     out.grid_offsets.end() - 1);
        for (size_t t = 0; t < nt2; ++t) {
            int cx0, cx1, cz0, cz1;
            cell_range(t, cx0, cx1, cz0, cz1);
            for (int cz = cz0; cz <= cz1; ++cz)
                for (int cx = cx0; cx <= cx1; ++cx)
                    out.grid_tris[cursor[size_t(cz) * out.grid_nx + cx]++] =
                        uint32_t(t);
        }
    }

    out.valid = true;
    out.stats.solve_ms = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
    g_solved.fetch_add(1, std::memory_order_relaxed);
    g_solve_us.fetch_add(uint64_t(out.stats.solve_ms * 1000.0),
                         std::memory_order_relaxed);
    g_tris.fetch_add(out.stats.tris, std::memory_order_relaxed);
    g_folds.fetch_add(out.stats.folds, std::memory_order_relaxed);
    return true;
}

void evaluate(const Field& field, const float* positions, const float* normals,
              size_t count, float* out_uv, uint32_t* out_frame) {
    // Fail-soft default: uv 0, su 0 ("no warp"). Written up front so every
    // early-out path leaves well-defined vertex data.
    for (size_t i = 0; i < count; ++i) {
        out_uv[i * 2 + 0] = 0.0f;
        out_uv[i * 2 + 1] = 0.0f;
        out_frame[i * 2 + 0] = 0u;
        out_frame[i * 2 + 1] = 0u;
    }
    if (!field.valid || field.positions.empty()) return;

    // Exact weld-hit map (rung-0 vertices are bitwise solve vertices).
    std::unordered_map<PosKey, uint32_t, PosKeyHash> exact;
    exact.reserve(field.positions.size() * 2);
    for (uint32_t v = 0; v < field.positions.size(); ++v) {
        const float3& p = field.positions[v];
        exact.try_emplace(PosKey{p.x, p.y, p.z}, v);
    }

    auto emit = [&](size_t i, const float2& uv, const float3& gu,
                    const float3& gv) {
        const float3 n = norm3(f3(normals[i * 3 + 0], normals[i * 3 + 1],
                                  normals[i * 3 + 2]));
        // Project the Jacobian rows onto the vertex's shading-normal tangent
        // plane; T = direction of grad-u, B = N x T, sv = grad-v . B (the
        // grad-v-along-T shear is dropped — the spec's 8-byte frame budget;
        // ARAP output is near-conformal so the residual is small).
        float3 gut = gu - n * dot(n, gu);
        const float su = std::sqrt(dot(gut, gut));
        if (!(su > 1e-6f)) return;  // leave as "no warp"
        const float3 T = gut * (1.0f / su);
        const float3 B = cross(n, T);
        const float sv = dot(gv, B);
        float ox, oy;
        oct_encode(T, ox, oy);
        out_uv[i * 2 + 0] = uv.x;
        out_uv[i * 2 + 1] = uv.y;
        out_frame[i * 2 + 0] = pack_half2(ox, oy);
        out_frame[i * 2 + 1] = pack_half2(su, sv);
    };

    for (size_t i = 0; i < count; ++i) {
        const float3 p = f3(positions[i * 3 + 0], positions[i * 3 + 1],
                            positions[i * 3 + 2]);
        auto hit = exact.find(PosKey{p.x, p.y, p.z});
        if (hit != exact.end()) {
            const VertexField& vf = field.verts[hit->second];
            emit(i, vf.uv, vf.gu, vf.gv);
            continue;
        }
        // Nearest solve triangle via the XZ grid, expanding ring search.
        const int cx =
            std::max(0, std::min(field.grid_nx - 1,
                                 int((p.x - field.grid_min_x) / field.grid_cell)));
        const int cz =
            std::max(0, std::min(field.grid_nz - 1,
                                 int((p.z - field.grid_min_z) / field.grid_cell)));
        float best_d2 = 1e30f;
        float3 best_bary = f3(0, 0, 0);
        uint32_t best_tri = UINT32_MAX;
        const int max_ring = std::max(field.grid_nx, field.grid_nz);
        for (int ring = 0; ring <= max_ring; ++ring) {
            // Once a hit exists, one extra ring is enough to make the
            // nearest-by-distance answer correct for cell-sized margins.
            if (best_tri != UINT32_MAX && ring > 1 &&
                best_d2 < float(ring - 1) * field.grid_cell *
                              float(ring - 1) * field.grid_cell)
                break;
            for (int dz = -ring; dz <= ring; ++dz) {
                for (int dx = -ring; dx <= ring; ++dx) {
                    if (std::max(std::abs(dx), std::abs(dz)) != ring) continue;
                    const int gx = cx + dx, gz = cz + dz;
                    if (gx < 0 || gz < 0 || gx >= field.grid_nx ||
                        gz >= field.grid_nz)
                        continue;
                    const size_t cell = size_t(gz) * field.grid_nx + gx;
                    for (uint32_t k = field.grid_offsets[cell];
                         k < field.grid_offsets[cell + 1]; ++k) {
                        const uint32_t t = field.grid_tris[k];
                        const uint32_t* idx = &field.indices[t * 3];
                        const float3 a = field.positions[idx[0]];
                        const float3 b = field.positions[idx[1]];
                        const float3 c = field.positions[idx[2]];
                        // Closest point on triangle (Ericson).
                        const float3 ab = b - a, ac = c - a, ap = p - a;
                        const float d1 = dot(ab, ap), d2 = dot(ac, ap);
                        float3 bary;
                        float3 q;
                        if (d1 <= 0.0f && d2 <= 0.0f) {
                            q = a;
                            bary = f3(1, 0, 0);
                        } else {
                            const float3 bp = p - b;
                            const float d3 = dot(ab, bp), d4 = dot(ac, bp);
                            if (d3 >= 0.0f && d4 <= d3) {
                                q = b;
                                bary = f3(0, 1, 0);
                            } else {
                                const float vc = d1 * d4 - d3 * d2;
                                if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
                                    const float t2 =
                                        d1 / std::max(d1 - d3, 1e-12f);
                                    q = a + ab * t2;
                                    bary = f3(1 - t2, t2, 0);
                                } else {
                                    const float3 cp = p - c;
                                    const float d5 = dot(ab, cp),
                                                d6 = dot(ac, cp);
                                    if (d6 >= 0.0f && d5 <= d6) {
                                        q = c;
                                        bary = f3(0, 0, 1);
                                    } else {
                                        const float vb = d5 * d2 - d1 * d6;
                                        if (vb <= 0.0f && d2 >= 0.0f &&
                                            d6 <= 0.0f) {
                                            const float t2 =
                                                d2 /
                                                std::max(d2 - d6, 1e-12f);
                                            q = a + ac * t2;
                                            bary = f3(1 - t2, 0, t2);
                                        } else {
                                            const float va = d3 * d6 - d5 * d4;
                                            if (va <= 0.0f &&
                                                (d4 - d3) >= 0.0f &&
                                                (d5 - d6) >= 0.0f) {
                                                const float t2 =
                                                    (d4 - d3) /
                                                    std::max((d4 - d3) +
                                                                 (d5 - d6),
                                                             1e-12f);
                                                q = b + (c - b) * t2;
                                                bary = f3(0, 1 - t2, t2);
                                            } else {
                                                const float denom =
                                                    1.0f /
                                                    std::max(va + vb + vc,
                                                             1e-20f);
                                                const float w1 = vb * denom;
                                                const float w2 = vc * denom;
                                                q = a + ab * w1 + ac * w2;
                                                bary =
                                                    f3(1 - w1 - w2, w1, w2);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        const float3 dqp = p - q;
                        const float dd = dot(dqp, dqp);
                        if (dd < best_d2 ||
                            (dd == best_d2 && t < best_tri)) {
                            best_d2 = dd;
                            best_tri = t;
                            best_bary = bary;
                        }
                    }
                }
            }
        }
        if (best_tri == UINT32_MAX) continue;
        const uint32_t* idx = &field.indices[best_tri * 3];
        const VertexField& v0 = field.verts[idx[0]];
        const VertexField& v1 = field.verts[idx[1]];
        const VertexField& v2 = field.verts[idx[2]];
        const float2 uv = make_float2(best_bary.x * v0.uv.x +
                                          best_bary.y * v1.uv.x +
                                          best_bary.z * v2.uv.x,
                                      best_bary.x * v0.uv.y +
                                          best_bary.y * v1.uv.y +
                                          best_bary.z * v2.uv.y);
        const float3 gu = v0.gu * best_bary.x + v1.gu * best_bary.y +
                          v2.gu * best_bary.z;
        const float3 gv = v0.gv * best_bary.x + v1.gv * best_bary.y +
                          v2.gv * best_bary.z;
        emit(i, uv, gu, gv);
    }
}

WarpCensus warp_census() {
    WarpCensus c;
    c.sectors = g_sectors.load(std::memory_order_relaxed);
    c.solved = g_solved.load(std::memory_order_relaxed);
    c.solve_us = g_solve_us.load(std::memory_order_relaxed);
    c.evaluate_us = g_evaluate_us.load(std::memory_order_relaxed);
    c.tris = g_tris.load(std::memory_order_relaxed);
    c.folds = g_folds.load(std::memory_order_relaxed);
    return c;
}

void warp_census_add_evaluate_us(uint64_t us) {
    g_evaluate_us.fetch_add(us, std::memory_order_relaxed);
}

}  // namespace warp_field
