// libs/MatterSurfaceLib/src/mesh_indexed.cpp
//
// Conversion between the engine's two mesh representations:
//
//   - `std::vector<Tri>` (+ a parallel `std::vector<TriEx>`) — the non-indexed
//     triangle soup produced by the meshers and consumed at the BLAS/raytrace
//     boundary. Every triangle carries its own three vertex copies.
//   - `MeshIndexed` (see `include/mesh_indexed.hpp`) — positions + uint32
//     indices + an optional parallel `TriEx` array. This is the format the
//     MatterSurfaceLib mesh-transformation pipeline works in
//     (`mesh_simplifier`, `mesh_retopo`, `mesh_smooth`, `mesh_transform`).
//
// `float3`, `Tri` and `TriEx` come from SpatialQueryLib (`tri.h` / `precomp.h`),
// which despite its name owns the engine's core geometry types.
//
// Welding convention. `from_tri` welds by SNAPPING each coordinate to an
// integer grid of spacing `opts.epsilon` (default 1e-4 world units) and using
// the resulting integer triple as a hash key. This is grid quantization, not a
// true distance tolerance: two vertices closer than epsilon that happen to
// straddle a grid boundary land in different cells and stay separate. That is
// accepted at these tolerances — see the `KeyGen` comment below. Note that
// `mesh_simplifier.cpp` re-welds internally on its own 1e-5 grid, so the two
// welds are not the same granularity.
//
// Attribute survival. `TriEx` is per-TRIANGLE, so it is never affected by
// vertex welding and is simply copied across when the caller supplies an array
// of the right length. Neither function allocates GPU resources, takes a lock,
// or touches global state; both are pure and safe to call from any thread.
#include "mesh_indexed.hpp"

#include <algorithm>
#include <array>
#include <cmath>      // std::llround (was arriving transitively)
#include <cstdint>
#include <cstring>
#include <unordered_map>

namespace {

// Quantize a float coordinate to an integer grid key. `epsilon` sets the grid
// spacing: positions closer than `epsilon` are near-guaranteed to map to the
// same key (modulo grid-boundary edge cases, which are acceptable at 1e-4).
struct KeyGen {
    float epsilon;
    long long qx, qy, qz;
    void quantize(float3 p) {
        auto q = [&](float v) -> long long {
            return (long long)std::llround((double)v / (double)epsilon);
        };
        qx = q(p.x); qy = q(p.y); qz = q(p.z);
    }
};

// FNV-1a over the raw bytes of the three quantized coordinates. Used as the
// hash for the weld lookup table; the map still compares full keys on
// collision, so a hash collision costs a probe, never a wrong weld.
struct KeyHash {
    size_t operator()(const std::array<long long, 3>& k) const noexcept {
        uint64_t h = 14695981039346656037ull;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(k.data());
        for (size_t i = 0; i < sizeof(k); ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
        return (size_t)h;
    }
};

} // namespace

// Vertices are emitted in first-seen order, so `out.positions` is ordered by
// the order the triangles reference them — deterministic for a given input.
// The POSITION kept for a weld cluster is the first one encountered, not an
// average, so output positions are always exact input positions.
//
// `triex` is copied through only when it is non-null AND exactly parallel to
// `tris`. A null or wrong-length `triex` is silently ignored and the result
// comes back with `triex` empty (i.e. "no TriEx attached"), not with an error.
MeshIndexed from_tri(const std::vector<Tri>& tris,
                     const std::vector<TriEx>* triex,
                     const WeldOptions& opts) {
    MeshIndexed out;
    if (tris.empty()) return out;

    std::unordered_map<std::array<long long, 3>, uint32_t, KeyHash> lookup;
    lookup.reserve(tris.size() * 3);

    KeyGen kg{opts.epsilon, 0, 0, 0};

    auto emit_vertex = [&](float3 p) -> uint32_t {
        kg.quantize(p);
        std::array<long long, 3> k = {kg.qx, kg.qy, kg.qz};
        auto it = lookup.find(k);
        if (it != lookup.end()) return it->second;
        uint32_t idx = (uint32_t)out.positions.size();
        out.positions.push_back(p);
        lookup.emplace(k, idx);
        return idx;
    };

    out.indices.reserve(tris.size() * 3);
    for (const Tri& t : tris) {
        out.indices.push_back(emit_vertex(t.vertex0));
        out.indices.push_back(emit_vertex(t.vertex1));
        out.indices.push_back(emit_vertex(t.vertex2));
    }

    if (triex && triex->size() == tris.size()) {
        out.triex = *triex;
    }
    return out;
}

// Both output vectors are cleared first, so this overwrites whatever the
// caller passed in. Each emitted `Tri` is value-initialized and then given
// only `vertex0/1/2` and a freshly averaged `centroid`; any other field of
// `Tri` comes back zeroed. A `from_tri` -> `to_tri` round trip therefore
// preserves geometry and TriEx but not other per-Tri bookkeeping.
void to_tri(const MeshIndexed& in,
            std::vector<Tri>& tris_out,
            std::vector<TriEx>& triex_out) {
    tris_out.clear();
    triex_out.clear();
    if (in.indices.empty()) return;

    size_t tri_count = in.indices.size() / 3;
    tris_out.reserve(tri_count);

    auto vertex = [&](uint32_t i) -> float3 { return in.positions[i]; };

    for (size_t i = 0; i < tri_count; ++i) {
        float3 a = vertex(in.indices[i*3 + 0]);
        float3 b = vertex(in.indices[i*3 + 1]);
        float3 c = vertex(in.indices[i*3 + 2]);
        Tri t{};
        t.vertex0 = a; t.vertex1 = b; t.vertex2 = c;
        t.centroid = make_float3((a.x+b.x+c.x)/3.0f,
                                 (a.y+b.y+c.y)/3.0f,
                                 (a.z+b.z+c.z)/3.0f);
        tris_out.push_back(t);
    }

    if (in.triex.size() == tri_count) {
        triex_out = in.triex;
    }
}
