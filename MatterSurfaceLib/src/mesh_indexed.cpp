#include "mesh_indexed.hpp"

#include <algorithm>
#include <array>
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
