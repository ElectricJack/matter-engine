// Tests for modifier_apply: ordered stack semantics, failure-skip, and the
// retopo blacklist chunk hash. Built WITHOUT MATTER_HAVE_AUTOREMESHER, so the
// Retopo case exercises the warn+skip path.
#include "modifier_apply.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <map>
#include <utility>
#include <vector>

namespace {

MeshIndexed make_sphere(int levels) {
    // identical generator to MatterSurfaceLib/tests/mesh_smooth_tests.cpp
    MeshIndexed m;
    m.positions = {
        make_float3( 1, 0, 0), make_float3(-1, 0, 0),
        make_float3( 0, 1, 0), make_float3( 0,-1, 0),
        make_float3( 0, 0, 1), make_float3( 0, 0,-1),
    };
    m.indices = { 0,2,4, 2,1,4, 1,3,4, 3,0,4,
                  2,0,5, 1,2,5, 3,1,5, 0,3,5 };
    for (int l = 0; l < levels; ++l) {
        std::map<std::pair<uint32_t,uint32_t>, uint32_t> mid;
        auto midpoint = [&](uint32_t a, uint32_t b) -> uint32_t {
            auto key = a < b ? std::make_pair(a, b) : std::make_pair(b, a);
            auto it = mid.find(key);
            if (it != mid.end()) return it->second;
            float3 pa = m.positions[a], pb = m.positions[b];
            float3 p = make_float3((pa.x+pb.x)*0.5f, (pa.y+pb.y)*0.5f, (pa.z+pb.z)*0.5f);
            float len = std::sqrt(p.x*p.x + p.y*p.y + p.z*p.z);
            p = make_float3(p.x/len, p.y/len, p.z/len);
            uint32_t idx = (uint32_t)m.positions.size();
            m.positions.push_back(p);
            mid[key] = idx;
            return idx;
        };
        std::vector<uint32_t> out;
        for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
            uint32_t a = m.indices[t], b = m.indices[t+1], c = m.indices[t+2];
            uint32_t ab = midpoint(a,b), bc = midpoint(b,c), ca = midpoint(c,a);
            uint32_t tri[12] = { a,ab,ca, b,bc,ab, c,ca,bc, ab,bc,ca };
            out.insert(out.end(), tri, tri + 12);
        }
        m.indices = out;
    }
    return m;
}

dsl::ModifierSpec simplify_spec(float ratio) {
    dsl::ModifierSpec s{}; s.kind = dsl::ModifierKind::Simplify; s.ratio = ratio; return s;
}
dsl::ModifierSpec smooth_spec() {
    dsl::ModifierSpec s{}; s.kind = dsl::ModifierKind::Smooth; return s;
}

bool positions_equal(const MeshIndexed& a, const MeshIndexed& b) {
    if (a.positions.size() != b.positions.size()) return false;
    for (size_t i = 0; i < a.positions.size(); ++i)
        if (a.positions[i].x != b.positions[i].x ||
            a.positions[i].y != b.positions[i].y ||
            a.positions[i].z != b.positions[i].z) return false;
    return true;
}

void test_stack_order_matters() {
    MeshIndexed m = make_sphere(2);
    MeshIndexed ab = modifier_apply::apply_stack(m, { simplify_spec(0.5f), smooth_spec() }, "t");
    MeshIndexed ba = modifier_apply::apply_stack(m, { smooth_spec(), simplify_spec(0.5f) }, "t");
    assert(!ab.positions.empty() && !ba.positions.empty());
    assert(!positions_equal(ab, ba));  // simplify-then-smooth != smooth-then-simplify
}

void test_failed_modifier_is_skipped() {
    MeshIndexed m = make_sphere(1);
    // mu > 0 bypasses binding validation (spec constructed directly) and makes
    // smooth() return !ok -> apply_stack must skip it and still run simplify.
    dsl::ModifierSpec bad = smooth_spec(); bad.mu = 0.5f;
    MeshIndexed out = modifier_apply::apply_stack(m, { bad, simplify_spec(0.5f) }, "t");
    assert(!out.positions.empty());
    assert(out.indices.size() < m.indices.size());  // simplify still ran
}

void test_retopo_unavailable_is_skipped() {
#ifndef MATTER_HAVE_AUTOREMESHER
    MeshIndexed m = make_sphere(1);
    dsl::ModifierSpec r{}; r.kind = dsl::ModifierKind::Retopo;
    MeshIndexed out = modifier_apply::apply_stack(m, { r, smooth_spec() }, "t");
    assert(out.positions.size() == m.positions.size());  // retopo skipped, smooth ran
#endif
}

void test_chunk_hash_sensitivity() {
    MeshIndexed m = make_sphere(1);
    dsl::ModifierSpec r{}; r.kind = dsl::ModifierKind::Retopo;
    uint64_t h0 = modifier_apply::chunk_retopo_hash(m, r);
    assert(h0 == modifier_apply::chunk_retopo_hash(m, r));  // stable

    dsl::ModifierSpec r2 = r; r2.seed = 7;
    assert(modifier_apply::chunk_retopo_hash(m, r2) != h0);  // params change key

    MeshIndexed m2 = m; m2.positions[0].x += 0.25f;
    assert(modifier_apply::chunk_retopo_hash(m2, r) != h0);  // mesh change keys

    dsl::ModifierSpec r3 = r; r3.target_ratio = 0.5f;
    assert(modifier_apply::chunk_retopo_hash(m, r3) != h0);
}

} // namespace

int main() {
    printf("modifier_apply_tests\n");
    test_stack_order_matters();
    test_failed_modifier_is_skipped();
    test_retopo_unavailable_is_skipped();
    test_chunk_hash_sensitivity();
    printf("all modifier_apply tests passed\n");
    return 0;
}
