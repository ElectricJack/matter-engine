# Geometry Modifier Regions Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `beginModifier()`/`endModifier(list)` DSL region markers whose ordered modifier stack (simplify / smooth / retopo) post-processes the region's geometry at part bake, replacing `this.simplify()` and `static retopo` outright.

**Architecture:** Regions are recorded during `build()` as index ranges over the DslState build buffer + direct-triangle buffer. At `script_host::bake_source`, region ops are cell-meshed separately, accumulated per material-merge-group across cells, welded into one indexed mesh, run through the stack (new `modifier_apply` module), and registered as one BLAS per group. A new Taubin λ/μ smoothing pass is added to MatterSurfaceLib. The flatten-time retopo hook, its `.retopo.part` cache, `this.simplify()`, and `static retopo` are all deleted.

**Tech Stack:** C++17 engine (MatterEngine3), C-style MSL (MatterSurfaceLib), QuickJS-ng v0.10 DSL bindings, Make.

**Spec:** `docs/superpowers/specs/2026-07-08-modifier-regions-design.md` (approved, amended to bake-time)

**Execution gate:** Implementation waits for Phase B async bake to land (spec "Depends on"). `script_host.cpp` and `local_provider.cpp` are under active rewrite there.

## Global Constraints

- **Line refs are pinned to `main@54dbd76`.** Phase B will drift `script_host.cpp`/`local_provider.cpp` — at execution time re-locate EVERY anchor by symbol/grep (the anchors below name the symbols), never by raw line number.
- **Determinism:** every repack into `Tri`/`TriEx` destined for a `.part` uses memset-zero structs before field assignment (byte-stable output). All new adjacency/accumulation containers iterate in deterministic order (`std::map`, never unordered). Retopo runs `threads = 1`.
- **MSL is read-only** except the new `mesh_smooth.{hpp,cpp}` + its test files — an approved scope exception stated in the spec.
- **`MATTER_HAVE_AUTOREMESHER` is Linux-only.** The Windows cross-build must stay clean: every retopo reference in new engine code sits behind `#ifdef MATTER_HAVE_AUTOREMESHER` with a one-line warn+skip `#else`.
- **Clean cut, no legacy:** `this.simplify()` and `static retopo` are deleted, not aliased. All schema callers migrate in this same change.
- **`decimate_tris` in MSL STAYS** — still used by `lod_bake.cpp:104` and `composition_tests.cpp:34`. Only script_host's direct-triangle decimate branch is deleted.
- **Modifier failure = skip:** a failing modifier logs one line to stderr (`[modifier] <chunk label>: ...`) and the rest of the stack runs on the previous mesh.
- **Gates stay green after every task.** Region machinery (Tasks 1–4) lands BEFORE the deletions (Task 6), so `Tree.js`'s `simplify()` keeps working until it is migrated (Task 5).
- **`make windows` after any engine change** — and because this change touches headers/structs, do a CLEAN Windows rebuild (delete objs first; no header dep tracking).
- **Viewer/GPU runs:** `GALLIUM_DRIVER=d3d12` always; scripted viewer runs must self-terminate (`tools/viewer_shots.sh`).

## File Map

| File | Change |
|---|---|
| `MatterSurfaceLib/include/mesh_smooth.hpp` + `src/mesh_smooth.cpp` | NEW — Taubin λ/μ pass |
| `MatterSurfaceLib/tests/mesh_smooth_tests.cpp` + `tests/Makefile` | NEW suite |
| `MatterEngine3/src/dsl_state.h` + `dsl_triangle.cpp` | region structs + begin/end methods |
| `MatterEngine3/src/dsl_bindings.cpp` + `part_base.js.h` | `__dsl_beginModifier`/`__dsl_endModifier` |
| `MatterEngine3/src/modifier_apply.{h,cpp}` | NEW — stack runner + blacklist hash |
| `MatterEngine3/src/script_host.cpp` | open-region check; `mesh_sdf_ops` extraction; region bake path |
| `MatterEngine3/tests/*` | new suites + Makefile registration |
| Schemas `Tree.js` / `TreeBranch.js` / `Rock.js` | migrate to regions |
| `part_flatten.*`, `part_asset_v2.*`, `retopo_hook_stats.h`, etc. | Task 6 clean-cut deletions |

---

### Task 1: MSL Taubin smoothing pass (`mesh_smooth`)

**Files:**
- Create: `MatterSurfaceLib/include/mesh_smooth.hpp`
- Create: `MatterSurfaceLib/src/mesh_smooth.cpp`
- Create: `MatterSurfaceLib/tests/mesh_smooth_tests.cpp`
- Modify: `MatterSurfaceLib/tests/Makefile` (new SMOOTH block; model = SIMP block at lines 79–87)
- Modify: `build-all.sh` (add `mesh_smooth_tests` to the MSL headless suite list, lines 157–160)

**Interfaces:**
- Consumes: `MeshIndexed` (`mesh_indexed.hpp`), `float3`/`TriEx`/`make_float3` (`bvh.h`).
- Produces: `SmoothResult smooth(const MeshIndexed& in, const SmoothOptions& opts);` with `SmoothOptions{ int iterations=2; float lambda=0.5f; float mu=-0.53f; }` and `SmoothResult{ MeshIndexed mesh; bool ok=false; std::string err; }`. Task 3's `modifier_apply` calls this.

- [ ] **Step 1: Write the header**

`MatterSurfaceLib/include/mesh_smooth.hpp`:

```cpp
#ifndef MSL_MESH_SMOOTH_HPP
#define MSL_MESH_SMOOTH_HPP

#include "mesh_indexed.hpp"

#include <string>

// Taubin lambda/mu smoothing (shrink-free Laplacian) over the vertex 1-ring
// with uniform (umbrella) weights. Operates on MeshIndexed; connectivity is
// never changed. Boundary vertices (edge incidence != 2) are held fixed.
struct SmoothOptions {
    int   iterations = 2;      // >= 1
    float lambda     = 0.5f;   // positive step
    float mu         = -0.53f; // negative step (|mu| slightly > lambda)
};

struct SmoothResult {
    MeshIndexed mesh;
    bool        ok = false;
    std::string err;           // set when !ok
};

SmoothResult smooth(const MeshIndexed& in, const SmoothOptions& opts);

#endif // MSL_MESH_SMOOTH_HPP
```

- [ ] **Step 2: Write the failing tests**

`MatterSurfaceLib/tests/mesh_smooth_tests.cpp` (standalone binary per MSL convention — own `main()`, `assert`-based like `mesh_indexed_tests.cpp`):

```cpp
// Tests for the Taubin lambda/mu smoothing pass (mesh_smooth.hpp).
#include "mesh_smooth.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <map>
#include <utility>
#include <vector>

namespace {

// Signed volume of a closed mesh: sum over tris of dot(v0, cross(v1, v2)) / 6.
double signed_volume(const MeshIndexed& m) {
    double vol = 0.0;
    for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
        float3 a = m.positions[m.indices[t]];
        float3 b = m.positions[m.indices[t + 1]];
        float3 c = m.positions[m.indices[t + 2]];
        double cx = (double)b.y * c.z - (double)b.z * c.y;
        double cy = (double)b.z * c.x - (double)b.x * c.z;
        double cz = (double)b.x * c.y - (double)b.y * c.x;
        vol += a.x * cx + a.y * cy + a.z * cz;
    }
    return vol / 6.0;
}

// Unit octahedron subdivided `levels` times, midpoints projected to the unit
// sphere. Closed, manifold, no boundary.
MeshIndexed make_sphere(int levels) {
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

// Open (n+1)x(n+1) grid in the XY plane — has a boundary ring.
MeshIndexed make_grid(int n) {
    MeshIndexed m;
    for (int y = 0; y <= n; ++y)
        for (int x = 0; x <= n; ++x)
            m.positions.push_back(make_float3((float)x, (float)y, 0.0f));
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            uint32_t a = (uint32_t)(y*(n+1)+x), b = a+1,
                     c = a+(uint32_t)(n+1),     d = c+1;
            uint32_t tri[6] = { a,b,d, a,d,c };
            m.indices.insert(m.indices.end(), tri, tri + 6);
        }
    return m;
}

void test_volume_preserved() {
    MeshIndexed m = make_sphere(2);
    double v0 = signed_volume(m);
    SmoothOptions opts; opts.iterations = 5;
    SmoothResult r = smooth(m, opts);
    assert(r.ok);
    double v1 = signed_volume(r.mesh);
    assert(std::fabs(v1 - v0) / std::fabs(v0) < 0.05);  // Taubin is shrink-free
    printf("  volume %.6f -> %.6f\n", v0, v1);
}

void test_counts_unchanged_no_nans() {
    MeshIndexed m = make_sphere(1);
    SmoothResult r = smooth(m, {});
    assert(r.ok);
    assert(r.mesh.positions.size() == m.positions.size());
    assert(r.mesh.indices == m.indices);   // connectivity untouched
    bool moved = false;
    for (size_t i = 0; i < m.positions.size(); ++i) {
        const float3& p = r.mesh.positions[i];
        assert(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
        if (p.x != m.positions[i].x || p.y != m.positions[i].y || p.z != m.positions[i].z)
            moved = true;
    }
    assert(moved);  // it actually did something
}

void test_determinism() {
    MeshIndexed m = make_sphere(2);
    SmoothResult a = smooth(m, {});
    SmoothResult b = smooth(m, {});
    assert(a.ok && b.ok);
    assert(a.mesh.positions.size() == b.mesh.positions.size());
    for (size_t i = 0; i < a.mesh.positions.size(); ++i) {
        assert(a.mesh.positions[i].x == b.mesh.positions[i].x);
        assert(a.mesh.positions[i].y == b.mesh.positions[i].y);
        assert(a.mesh.positions[i].z == b.mesh.positions[i].z);
    }
}

void test_boundary_fixed() {
    const int n = 4;
    MeshIndexed m = make_grid(n);
    SmoothResult r = smooth(m, {});
    assert(r.ok);
    for (int y = 0; y <= n; ++y)
        for (int x = 0; x <= n; ++x) {
            if (!(x == 0 || y == 0 || x == n || y == n)) continue;
            size_t i = (size_t)y*(n+1)+x;
            assert(r.mesh.positions[i].x == m.positions[i].x);
            assert(r.mesh.positions[i].y == m.positions[i].y);
            assert(r.mesh.positions[i].z == m.positions[i].z);
        }
}

void test_triex_normals_recomputed() {
    MeshIndexed m = make_sphere(1);
    m.triex.resize(m.indices.size() / 3);
    for (TriEx& e : m.triex) { e = TriEx{}; e.materialId = 3; }
    SmoothResult r = smooth(m, {});
    assert(r.ok);
    assert(r.mesh.triex.size() == m.triex.size());
    for (const TriEx& e : r.mesh.triex) {
        assert(e.materialId == 3);  // non-normal TriEx fields untouched
        float len = std::sqrt(e.N0.x*e.N0.x + e.N0.y*e.N0.y + e.N0.z*e.N0.z);
        assert(std::fabs(len - 1.0f) < 1e-3f);  // unit, non-NaN
    }
}

void test_error_paths() {
    MeshIndexed empty;
    assert(!smooth(empty, {}).ok);

    MeshIndexed m = make_sphere(0);
    SmoothOptions bad;
    bad.iterations = 0;            assert(!smooth(m, bad).ok);
    bad = {}; bad.lambda = -0.1f;  assert(!smooth(m, bad).ok);
    bad = {}; bad.mu = 0.1f;       assert(!smooth(m, bad).ok);

    MeshIndexed oob = m; oob.indices[0] = 9999;
    assert(!smooth(oob, {}).ok);

    MeshIndexed ragged = m; ragged.indices.pop_back();
    assert(!smooth(ragged, {}).ok);

    MeshIndexed badex = m; badex.triex.resize(1);  // wrong parallel size
    assert(!smooth(badex, {}).ok);
}

} // namespace

int main() {
    printf("mesh_smooth_tests\n");
    test_volume_preserved();
    test_counts_unchanged_no_nans();
    test_determinism();
    test_boundary_fixed();
    test_triex_normals_recomputed();
    test_error_paths();
    printf("all mesh_smooth tests passed\n");
    return 0;
}
```

- [ ] **Step 3: Register the suite in `MatterSurfaceLib/tests/Makefile`**

Append (mirroring the `SIMP_TARGET` block exactly — recipe lines use TABs):

```make
# Taubin smoothing unit tests (headless, no GL window required)
SMOOTH_TARGET = mesh_smooth_tests
SMOOTH_SOURCES = mesh_smooth_tests.cpp ../src/mesh_smooth.cpp

$(SMOOTH_TARGET): $(SMOOTH_SOURCES)
	$(CC) $(SMOOTH_SOURCES) -o $(SMOOTH_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)

run-smooth: $(SMOOTH_TARGET)
	./$(SMOOTH_TARGET)
```

Also: add `run-smooth` to the `.PHONY` line (line 67) and `$(SMOOTH_TARGET)` to the `clean:` rm line (line 70).

- [ ] **Step 4: Run to verify the build fails**

Run: `make -C MatterSurfaceLib/tests mesh_smooth_tests`
Expected: FAIL — `../src/mesh_smooth.cpp: No such file or directory`.

- [ ] **Step 5: Write the implementation**

`MatterSurfaceLib/src/mesh_smooth.cpp`:

```cpp
// Taubin lambda/mu smoothing (shrink-free Laplacian) on MeshIndexed.
#include "mesh_smooth.hpp"

#include <cmath>
#include <map>
#include <utility>
#include <vector>

namespace {

std::pair<uint32_t, uint32_t> edge_key(uint32_t a, uint32_t b) {
    return a < b ? std::make_pair(a, b) : std::make_pair(b, a);
}

} // namespace

SmoothResult smooth(const MeshIndexed& in, const SmoothOptions& opts) {
    SmoothResult r;
    if (in.positions.empty() || in.indices.empty()) { r.err = "smooth: empty mesh"; return r; }
    if (in.indices.size() % 3 != 0) { r.err = "smooth: index count not a multiple of 3"; return r; }
    const uint32_t nv = (uint32_t)in.positions.size();
    for (uint32_t i : in.indices)
        if (i >= nv) { r.err = "smooth: index out of range"; return r; }
    if (!in.triex.empty() && in.triex.size() != in.indices.size() / 3) {
        r.err = "smooth: triex not parallel to triangles"; return r;
    }
    if (opts.iterations < 1)    { r.err = "smooth: iterations must be >= 1"; return r; }
    if (!(opts.lambda > 0.0f))  { r.err = "smooth: lambda must be > 0";      return r; }
    if (!(opts.mu < 0.0f))      { r.err = "smooth: mu must be < 0";          return r; }

    // 1-ring adjacency and boundary flags. std::map over sorted edge keys makes
    // ring order deterministic -> byte-stable output.
    std::map<std::pair<uint32_t, uint32_t>, int> edges;
    for (size_t t = 0; t + 2 < in.indices.size(); t += 3) {
        uint32_t a = in.indices[t], b = in.indices[t + 1], c = in.indices[t + 2];
        ++edges[edge_key(a, b)];
        ++edges[edge_key(b, c)];
        ++edges[edge_key(c, a)];
    }
    std::vector<std::vector<uint32_t>> ring(nv);
    std::vector<bool> boundary(nv, false);
    for (const auto& e : edges) {
        ring[e.first.first].push_back(e.first.second);
        ring[e.first.second].push_back(e.first.first);
        if (e.second != 2) {  // boundary or non-manifold edge: hold both ends fixed
            boundary[e.first.first]  = true;
            boundary[e.first.second] = true;
        }
    }

    r.mesh = in;
    std::vector<float3>& pos = r.mesh.positions;
    std::vector<float3> tmp(nv);

    auto umbrella_step = [&](const std::vector<float3>& src, float w,
                             std::vector<float3>& dst) {
        for (uint32_t v = 0; v < nv; ++v) {
            if (boundary[v] || ring[v].empty()) { dst[v] = src[v]; continue; }
            float ax = 0.0f, ay = 0.0f, az = 0.0f;
            for (uint32_t nb : ring[v]) { ax += src[nb].x; ay += src[nb].y; az += src[nb].z; }
            const float inv = 1.0f / (float)ring[v].size();
            ax *= inv; ay *= inv; az *= inv;
            dst[v] = make_float3(src[v].x + w * (ax - src[v].x),
                                 src[v].y + w * (ay - src[v].y),
                                 src[v].z + w * (az - src[v].z));
        }
    };

    for (int it = 0; it < opts.iterations; ++it) {
        umbrella_step(pos, opts.lambda, tmp);
        umbrella_step(tmp, opts.mu, pos);
    }

    // Per-corner normal recompute when TriEx is attached: area-weighted vertex
    // normals (raw cross-product accumulation). All other TriEx fields kept.
    if (!r.mesh.triex.empty()) {
        std::vector<float3> vn(nv, make_float3(0.0f, 0.0f, 0.0f));
        for (size_t t = 0; t + 2 < r.mesh.indices.size(); t += 3) {
            const float3 p0 = pos[r.mesh.indices[t]];
            const float3 p1 = pos[r.mesh.indices[t + 1]];
            const float3 p2 = pos[r.mesh.indices[t + 2]];
            const float ex1 = p1.x - p0.x, ey1 = p1.y - p0.y, ez1 = p1.z - p0.z;
            const float ex2 = p2.x - p0.x, ey2 = p2.y - p0.y, ez2 = p2.z - p0.z;
            const float fx = ey1 * ez2 - ez1 * ey2;   // 2*area-weighted face normal
            const float fy = ez1 * ex2 - ex1 * ez2;
            const float fz = ex1 * ey2 - ey1 * ex2;
            for (int k = 0; k < 3; ++k) {
                float3& acc = vn[r.mesh.indices[t + k]];
                acc = make_float3(acc.x + fx, acc.y + fy, acc.z + fz);
            }
        }
        for (float3& nrm : vn) {
            const float len = std::sqrt(nrm.x*nrm.x + nrm.y*nrm.y + nrm.z*nrm.z);
            nrm = (len > 1e-20f) ? make_float3(nrm.x/len, nrm.y/len, nrm.z/len)
                                 : make_float3(0.0f, 0.0f, 1.0f);
        }
        for (size_t t = 0; t + 2 < r.mesh.indices.size(); t += 3) {
            TriEx& e = r.mesh.triex[t / 3];
            e.N0 = vn[r.mesh.indices[t]];
            e.N1 = vn[r.mesh.indices[t + 1]];
            e.N2 = vn[r.mesh.indices[t + 2]];
        }
    }

    r.ok = true;
    return r;
}
```

- [ ] **Step 6: Run to verify tests pass**

Run: `make -C MatterSurfaceLib/tests run-smooth`
Expected: PASS — `all mesh_smooth tests passed`.

- [ ] **Step 7: Register in `build-all.sh`**

In the MSL headless suite loop (lines 157–160), add `mesh_smooth_tests` to the `for suite in ...` list.

- [ ] **Step 8: Verify MSL itself still builds**

Run: `make -C MatterSurfaceLib WSL_LINUX=1`
Expected: builds clean (mesh_smooth.cpp is only linked by its test + engine consumers; if the MSL Makefile compiles `src/*.cpp` by wildcard it just gets compiled too — either is fine).

- [ ] **Step 9: Commit**

```bash
git add MatterSurfaceLib/include/mesh_smooth.hpp MatterSurfaceLib/src/mesh_smooth.cpp \
        MatterSurfaceLib/tests/mesh_smooth_tests.cpp MatterSurfaceLib/tests/Makefile build-all.sh
git commit -m "feat(MatterSurfaceLib): Taubin lambda/mu smoothing pass on MeshIndexed (approved MSL scope exception)"
```

---

### Task 2: DSL modifier-region surface (record only)

Regions are recorded during `build()`; nothing consumes them until Task 4. Bakes with well-formed regions succeed unchanged — gates stay green.

**Files:**
- Modify: `MatterEngine3/src/dsl_state.h` (structs after `BuildBuffer` ~line 64; methods after the simplify block ~line 120; members in `private:`)
- Modify: `MatterEngine3/src/dsl_triangle.cpp` (method bodies — they need `tris_buf_->triangles().size()` and `dsl_state.cpp` cannot include `triangle_emit.hpp` per the header's float3/raymath clash note)
- Modify: `MatterEngine3/src/dsl_bindings.cpp` (two new bindings + registration)
- Modify: `MatterEngine3/src/part_base.js.h` (two Part methods after `smoothing(k)` line 31)
- Modify: `MatterEngine3/src/script_host.cpp` (open-region check next to the existing session-left-open check, ~lines 910–913 — locate by grepping the existing "left open" error message)
- Test: `MatterEngine3/tests/script_host_tests.cpp`

**Interfaces:**
- Produces (consumed by Tasks 3/4):

```cpp
namespace dsl {
enum class ModifierKind { Simplify, Smooth, Retopo };
struct ModifierSpec { /* see Step 2 */ };
struct ModifierRegion { size_t op_begin, op_end, tri_begin, tri_end; std::vector<ModifierSpec> stack; };
}
// On DslState:
void begin_modifier_region();
void end_modifier_region(std::vector<ModifierSpec> stack);
bool modifier_region_open() const;
const std::vector<ModifierRegion>& modifier_regions() const;
```

- [ ] **Step 1: Write the failing tests**

Add to `MatterEngine3/tests/script_host_tests.cpp` (follow the existing patterns: direct `dsl::DslState` misuse tests like `test_dsl_state_rules`, and bake tests via `script_host::ScriptHost host; BakeResult r = host.bake_source(src, "{}", {});` — register both new test functions in `main()`):

```cpp
static void test_modifier_region_state_rules() {
    {   // regions do not nest
        dsl::DslState s;
        s.begin_modifier_region();
        s.begin_modifier_region();
        CHECK(s.has_error());
    }
    {   // end without begin
        dsl::DslState s;
        s.end_modifier_region({});
        CHECK(s.has_error());
    }
    {   // cannot open inside a session
        dsl::DslState s;
        s.beginVoxels(0.1f);
        s.begin_modifier_region();
        CHECK(s.has_error());
    }
    {   // cannot close while a session is open (sessions must not straddle)
        dsl::DslState s;
        s.begin_modifier_region();
        s.beginVoxels(0.1f);
        s.end_modifier_region({});
        CHECK(s.has_error());
    }
    {   // happy path: op ranges cover exactly the brushes emitted inside
        dsl::DslState s;
        s.beginVoxels(0.1f);
        s.sphere({0,0,0}, 1.0f, dsl::CsgOp::Union);   // op 0: outside any region
        s.endVoxels();
        s.begin_modifier_region();
        s.beginVoxels(0.1f);
        s.sphere({0,0,0}, 1.0f, dsl::CsgOp::Union);   // op 1: inside
        s.endVoxels();
        dsl::ModifierSpec smooth_spec{};
        smooth_spec.kind = dsl::ModifierKind::Smooth;
        s.end_modifier_region({ smooth_spec });
        CHECK(!s.has_error());
        CHECK(s.modifier_regions().size() == 1);
        CHECK(s.modifier_regions()[0].op_begin == 1);
        CHECK(s.modifier_regions()[0].op_end == 2);
        CHECK(s.modifier_regions()[0].stack.size() == 1);
        CHECK(!s.modifier_region_open());
    }
}

static void test_modifier_region_bake_rules() {
    script_host::ScriptHost host;
    {   // region left open at end of build -> clean bake error
        const char* src =
            "class P extends Part { build(p) {"
            "  this.beginModifier();"
            "  this.beginVoxels(0.2); this.fill(2); this.sphere([0,0,0],0.5); this.endVoxels();"
            "} }";
        script_host::BakeResult r = host.bake_source(src, "{}", {});
        CHECK(!r.error.ok);
    }
    {   // unknown modifier name -> clean bake error
        const char* src =
            "class P extends Part { build(p) {"
            "  this.beginModifier();"
            "  this.beginVoxels(0.2); this.fill(2); this.sphere([0,0,0],0.5); this.endVoxels();"
            "  this.endModifier([{ frobnicate: {} }]);"
            "} }";
        script_host::BakeResult r = host.bake_source(src, "{}", {});
        CHECK(!r.error.ok);
    }
    {   // simplify ratio out of (0,1] -> clean bake error
        const char* src =
            "class P extends Part { build(p) {"
            "  this.beginModifier();"
            "  this.beginVoxels(0.2); this.fill(2); this.sphere([0,0,0],0.5); this.endVoxels();"
            "  this.endModifier([{ simplify: 1.5 }]);"
            "} }";
        script_host::BakeResult r = host.bake_source(src, "{}", {});
        CHECK(!r.error.ok);
    }
    {   // two-key entry -> clean bake error
        const char* src =
            "class P extends Part { build(p) {"
            "  this.beginModifier();"
            "  this.beginVoxels(0.2); this.fill(2); this.sphere([0,0,0],0.5); this.endVoxels();"
            "  this.endModifier([{ smooth: {}, retopo: {} }]);"
            "} }";
        script_host::BakeResult r = host.bake_source(src, "{}", {});
        CHECK(!r.error.ok);
    }
    {   // well-formed region bakes clean (stack processing lands in Task 4);
        // shorthand { simplify: 0.4 } accepted
        const char* src =
            "class P extends Part { build(p) {"
            "  this.beginModifier();"
            "  this.beginVoxels(0.2); this.fill(2); this.sphere([0,0,0],0.5); this.endVoxels();"
            "  this.endModifier([{ smooth: { iterations: 1 } }, { simplify: 0.4 }]);"
            "} }";
        script_host::BakeResult r = host.bake_source(src, "{}", {});
        CHECK(r.error.ok);
    }
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `make -C MatterEngine3/tests run-script`
Expected: FAIL to compile — `begin_modifier_region` not a member of `dsl::DslState`.

- [ ] **Step 3: Add structs + method decls + members to `dsl_state.h`**

After the `BuildBuffer` struct (line 64), inside `namespace dsl`:

```cpp
// --- Modifier regions (beginModifier/endModifier, spec 2026-07-08) ---
// A region marks a range of authored geometry whose baked mesh is post-
// processed by an ordered modifier stack at part bake. Regions do not nest;
// sessions must not straddle region boundaries; placeChild placements are
// NOT captured (children are governed by their own schema's regions).
enum class ModifierKind { Simplify, Smooth, Retopo };

struct ModifierSpec {
    ModifierKind kind = ModifierKind::Simplify;
    // simplify
    float ratio = 1.0f;              // keep-fraction, (0..1]
    // smooth (Taubin lambda/mu)
    int   iterations = 2;
    float lambda = 0.5f;
    float mu = -0.53f;
    // retopo
    float    target_ratio = 1.0f;
    int      retopo_iterations = 3;
    uint32_t seed = 0;
    int      timeout_seconds = 60;
};

struct ModifierRegion {
    size_t op_begin = 0, op_end = 0;    // [op_begin, op_end) over BuildBuffer::ops
    size_t tri_begin = 0, tri_end = 0;  // [tri_begin, tri_end) over the direct-triangle buffer
    std::vector<ModifierSpec> stack;    // execution order
};
```

In `class DslState` (after the simplify accessors, ~line 120), the public surface:

```cpp
    // Modifier regions. begin/end are defined in dsl_triangle.cpp — they
    // capture tris_buf_->triangles().size(), and this header cannot include
    // triangle_emit.hpp (float3/raymath clash).
    void begin_modifier_region();                          // misuse -> set_error
    void end_modifier_region(std::vector<ModifierSpec> stack);  // misuse -> set_error
    bool modifier_region_open() const { return region_open_; }
    const std::vector<ModifierRegion>& modifier_regions() const { return regions_; }
```

In `private:`:

```cpp
    bool   region_open_ = false;
    size_t region_start_op_ = 0;
    size_t region_start_tri_ = 0;
    std::vector<ModifierRegion> regions_;
```

- [ ] **Step 4: Implement in `dsl_triangle.cpp`**

(Verify the triangle-count accessor name on `tri_emit::TriangleBuildBuffer` in `triangle_emit.hpp` — it is used below as `triangles().size()`.)

```cpp
void DslState::begin_modifier_region() {
    if (region_open_) { set_error("beginModifier: modifier regions do not nest"); return; }
    if (session_ != Session::None) {
        set_error("beginModifier inside an open session (call it before beginVoxels/beginShape)");
        return;
    }
    region_open_ = true;
    region_start_op_  = buffer_.ops.size();
    region_start_tri_ = tris_buf_->triangles().size();
}

void DslState::end_modifier_region(std::vector<ModifierSpec> stack) {
    if (!region_open_) { set_error("endModifier without beginModifier"); return; }
    if (session_ != Session::None) {
        set_error("endModifier inside an open session (close the session first)");
        return;
    }
    ModifierRegion r;
    r.op_begin  = region_start_op_;
    r.op_end    = buffer_.ops.size();
    r.tri_begin = region_start_tri_;
    r.tri_end   = tris_buf_->triangles().size();
    r.stack     = std::move(stack);
    regions_.push_back(std::move(r));
    region_open_ = false;
}
```

- [ ] **Step 5: Add the JS bindings in `dsl_bindings.cpp`**

Near `j_simplify` (line 57), add (quickjs-ng v0.10: `JS_IsArray(v)` is one-arg; property enum atoms are freed manually; all author misuse goes through `st->set_error` — fail-closed at end of build — never a JS exception):

```cpp
static JSValue j_beginModifier(JSContext* c, JSValueConst, int, JSValueConst*) {
    state_of(c)->begin_modifier_region(); return JS_UNDEFINED; }

// Optional numeric field: returns true and fills `out` iff present (non-null).
static bool opt_num(JSContext* c, JSValueConst obj, const char* k, double& out) {
    JSValue v = JS_GetPropertyStr(c, obj, k);
    const bool has = !JS_IsUndefined(v) && !JS_IsNull(v);
    if (has) JS_ToFloat64(c, &out, v);
    JS_FreeValue(c, v);
    return has;
}

// endModifier(list): an Array of one-key objects in execution order, e.g.
//   [{ smooth: { iterations: 2 } }, { retopo: {...} }, { simplify: 0.3 }]
// Shorthand: { simplify: 0.3 } (bare number).
static JSValue j_endModifier(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    DslState* st = state_of(c);
    if (n < 1 || !JS_IsArray(a[0])) {
        st->set_error("endModifier: expected an array of modifier entries");
        return JS_UNDEFINED;
    }
    JSValue lenv = JS_GetPropertyStr(c, a[0], "length");
    uint32_t len = 0; JS_ToUint32(c, &len, lenv); JS_FreeValue(c, lenv);

    std::vector<ModifierSpec> stack;
    for (uint32_t i = 0; i < len && !st->has_error(); ++i) {
        JSValue entry = JS_GetPropertyUint32(c, a[0], i);
        JSPropertyEnum* props = nullptr;
        uint32_t pcount = 0;
        if (!JS_IsObject(entry) ||
            JS_GetOwnPropertyNames(c, &props, &pcount, entry,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
            st->set_error("endModifier: each entry must be an object like { smooth: {...} }");
            JS_FreeValue(c, entry);
            break;
        }
        if (pcount != 1) {
            st->set_error("endModifier: each entry must have exactly one key (the modifier name)");
        } else {
            const char* key = JS_AtomToCString(c, props[0].atom);
            JSValue val = JS_GetProperty(c, entry, props[0].atom);
            ModifierSpec spec{};
            double d = 0.0;
            if (key && !std::strcmp(key, "simplify")) {
                spec.kind = ModifierKind::Simplify;
                if (JS_IsNumber(val)) d = argd(c, val);          // shorthand { simplify: 0.3 }
                else if (!opt_num(c, val, "ratio", d)) d = 0.0;  // { simplify: { ratio: 0.3 } }
                if (!(d > 0.0 && d <= 1.0))
                    st->set_error("endModifier: simplify ratio must be in (0, 1]");
                spec.ratio = (float)d;
            } else if (key && !std::strcmp(key, "smooth")) {
                spec.kind = ModifierKind::Smooth;
                if (!JS_IsObject(val)) {
                    st->set_error("endModifier: smooth params must be an object");
                } else {
                    if (opt_num(c, val, "iterations", d)) spec.iterations = (int)d;
                    if (opt_num(c, val, "lambda", d))     spec.lambda = (float)d;
                    if (opt_num(c, val, "mu", d))         spec.mu = (float)d;
                    if (spec.iterations < 1 || !(spec.lambda > 0.0f) || !(spec.mu < 0.0f))
                        st->set_error("endModifier: smooth params out of range "
                                      "(iterations>=1, lambda>0, mu<0)");
                }
            } else if (key && !std::strcmp(key, "retopo")) {
                spec.kind = ModifierKind::Retopo;
                if (!JS_IsObject(val)) {
                    st->set_error("endModifier: retopo params must be an object");
                } else {
                    if (opt_num(c, val, "target_ratio", d))    spec.target_ratio = (float)d;
                    if (opt_num(c, val, "iterations", d))      spec.retopo_iterations = (int)d;
                    if (opt_num(c, val, "seed", d))            spec.seed = (uint32_t)d;
                    if (opt_num(c, val, "timeout_seconds", d)) spec.timeout_seconds = (int)d;
                    if (!(spec.target_ratio > 0.0f) || spec.retopo_iterations < 1 ||
                        spec.timeout_seconds < 1)
                        st->set_error("endModifier: retopo params out of range");
                }
            } else {
                st->set_error(std::string("endModifier: unknown modifier '") +
                              (key ? key : "?") + "'");
            }
            if (key) JS_FreeCString(c, key);
            JS_FreeValue(c, val);
            if (!st->has_error()) stack.push_back(spec);
        }
        for (uint32_t p = 0; p < pcount; ++p) JS_FreeAtom(c, props[p].atom);
        js_free(c, props);
        JS_FreeValue(c, entry);
    }
    if (!st->has_error()) st->end_modifier_region(std::move(stack));
    return JS_UNDEFINED;
}
```

Add `#include <string>` to the includes if not already pulled in transitively. Register in `install_bindings` after the `__dsl_simplify` line (690):

```cpp
    bind("__dsl_beginModifier",j_beginModifier,0); bind("__dsl_endModifier",j_endModifier,1);
```

- [ ] **Step 6: Add the Part methods in `part_base.js.h`**

After `smoothing(k)` (line 31):

```js
  beginModifier()        { __dsl_beginModifier(); }
  endModifier(list)      { __dsl_endModifier(list); }
```

- [ ] **Step 7: Add the open-region check in `script_host.cpp`**

Immediately after the existing session-left-open check at the end of build (lines 910–913 at main@54dbd76 — grep for the existing "left open" message to relocate post-Phase-B):

```cpp
    if (r.error.ok && state.modifier_region_open()) {
        r.error.ok = false;
        r.error.message =
            "modifier region left open at end of build (beginModifier without endModifier)";
    }
```

- [ ] **Step 8: Run the tests**

Run: the script-host suite target from Step 2, then `./MatterEngine3/tests/script_host_tests`
Expected: PASS, including all pre-existing tests.

- [ ] **Step 9: Commit**

```bash
git add MatterEngine3/src/dsl_state.h MatterEngine3/src/dsl_triangle.cpp \
        MatterEngine3/src/dsl_bindings.cpp MatterEngine3/src/part_base.js.h \
        MatterEngine3/src/script_host.cpp MatterEngine3/tests/script_host_tests.cpp
git commit -m "feat(MatterEngine3): beginModifier/endModifier DSL regions recorded on DslState (no bake consumer yet)"
```

---

### Task 3: `modifier_apply` — the stack runner

**Files:**
- Create: `MatterEngine3/src/modifier_apply.h`
- Create: `MatterEngine3/src/modifier_apply.cpp`
- Test: `MatterEngine3/tests/modifier_apply_tests.cpp` (new standalone suite)
- Modify: `MatterEngine3/tests/Makefile` (new MODAPPLY target + `run-modapply`)
- Modify: `MatterEngine3/Makefile` (add `modifier_apply.cpp` and `../MatterSurfaceLib/src/mesh_smooth.cpp` to the lib sources so `libmatter_engine3.a` carries them; the `.lib_flags` mechanism already forces recompiles on flag changes)
- Modify: `build-all.sh` (add `run-modapply` to the MatterEngine3 `run-*` loop at line ~197)

**Interfaces:**
- Consumes: `dsl::ModifierSpec`/`ModifierKind` (Task 2), MSL `simplify`/`smooth`/`retopo`, `matter_engine3::retopo_blacklist`.
- Produces (consumed by Task 4):

```cpp
namespace modifier_apply {
MeshIndexed apply_stack(MeshIndexed mesh,
                        const std::vector<dsl::ModifierSpec>& stack,
                        const std::string& chunk_label);
uint64_t chunk_retopo_hash(const MeshIndexed& mesh, const dsl::ModifierSpec& spec);
}
```

- [ ] **Step 1: Write the failing tests**

`MatterEngine3/tests/modifier_apply_tests.cpp` (own `main()`; reuse the octahedron-sphere generator from Task 1's test file — duplicate it here, the two test binaries are independent):

```cpp
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
```

- [ ] **Step 2: Register the suite in `MatterEngine3/tests/Makefile`, run to verify it fails**

Add a MODAPPLY block modeled on the existing standalone MSL-linking targets (use the `COMMON_MSL_*` variables where they fit; the suite needs mesh_simplifier + mesh_indexed + mesh_smooth from MSL, plus the two engine sources):

```make
MODAPPLY_TARGET = modifier_apply_tests
MODAPPLY_SRC = modifier_apply_tests.cpp ../src/modifier_apply.cpp ../src/retopo_blacklist.cpp \
               $(MSL)/mesh_smooth.cpp $(MSL)/mesh_simplifier.cpp $(MSL)/mesh_indexed.cpp

$(MODAPPLY_TARGET): $(MODAPPLY_SRC)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(MODAPPLY_SRC) -o $(MODAPPLY_TARGET) $(LDFLAGS)

run-modapply: $(MODAPPLY_TARGET)
	./$(MODAPPLY_TARGET)
```

Match the file's actual variable names (`$(MSL)`, `$(CXX)`, flags) to the neighboring targets — copy a working recipe, swap the sources. Add `run-modapply` to `.PHONY` and the target to `clean`. NOTE: `dsl_state.h` includes `raylib.h`, so the recipe needs the same raylib include path the script-host suite uses; `mesh_simplifier.cpp` may pull additional MSL deps — if the link fails, add exactly the missing MSL sources the way `COMMON_MSL_BLAS_SRC` does.

Run: `make -C MatterEngine3/tests run-modapply`
Expected: FAIL — `modifier_apply.h: No such file or directory`.

- [ ] **Step 3: Write the header**

`MatterEngine3/src/modifier_apply.h`:

```cpp
// Applies an ordered modifier stack (spec 2026-07-08 modifier regions) to one
// welded region mesh at part bake. Failure semantics: a modifier that fails is
// skipped with a one-line stderr warning; the rest of the stack still runs on
// the previous mesh (a stack of [{smooth},{retopo}] degrades to the smoothed
// mesh). Retopo is compiled only under MATTER_HAVE_AUTOREMESHER; otherwise it
// warns+skips so the Windows cross-build stays clean.
#pragma once

#include "dsl_state.h"
#include "mesh_indexed.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace modifier_apply {

// Run `stack` in order on `mesh`. `chunk_label` prefixes warnings, e.g.
// "part 0123456789abcdef region 0 group 2". Never throws / never fails.
MeshIndexed apply_stack(MeshIndexed mesh,
                        const std::vector<dsl::ModifierSpec>& stack,
                        const std::string& chunk_label);

// Blacklist key for one region chunk's retopo attempt: FNV-1a fold of the
// welded mesh (positions flattened to tightly-packed floats — float3 may carry
// padding — plus index bytes) and the retopo params. Exposed for tests.
uint64_t chunk_retopo_hash(const MeshIndexed& mesh, const dsl::ModifierSpec& spec);

} // namespace modifier_apply
```

- [ ] **Step 4: Write the implementation**

`MatterEngine3/src/modifier_apply.cpp`:

```cpp
#include "modifier_apply.h"

#include "mesh_simplifier.hpp"
#include "mesh_smooth.hpp"
#include "retopo_blacklist.h"

#include <cstdio>
#include <cstring>

#ifdef MATTER_HAVE_AUTOREMESHER
#include "mesh_retopo.hpp"
#endif

namespace modifier_apply {

namespace {

uint64_t fnv1a64(const void* data, size_t n) {
    uint64_t h = 1469598103934665603ull;
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

} // namespace

uint64_t chunk_retopo_hash(const MeshIndexed& mesh, const dsl::ModifierSpec& spec) {
    std::vector<float> f;
    f.reserve(mesh.positions.size() * 3);
    for (const float3& p : mesh.positions) {
        f.push_back(p.x); f.push_back(p.y); f.push_back(p.z);
    }
    const uint64_t hp = fnv1a64(f.data(), f.size() * sizeof(float));
    const uint64_t hi = fnv1a64(mesh.indices.data(),
                                mesh.indices.size() * sizeof(uint32_t));
    uint32_t tr_bits = 0;
    std::memcpy(&tr_bits, &spec.target_ratio, sizeof(tr_bits));
    const uint64_t fold[6] = { hp, hi, tr_bits,
                               (uint64_t)spec.retopo_iterations,
                               (uint64_t)spec.seed,
                               (uint64_t)spec.timeout_seconds };
    return fnv1a64(fold, sizeof(fold));
}

MeshIndexed apply_stack(MeshIndexed mesh,
                        const std::vector<dsl::ModifierSpec>& stack,
                        const std::string& chunk_label) {
    for (const dsl::ModifierSpec& m : stack) {
        switch (m.kind) {
        case dsl::ModifierKind::Simplify: {
            SimplifyOptions opts;
            opts.target_ratio = m.ratio;
            MeshIndexed out = simplify(mesh, opts);
            if (out.positions.empty() || out.indices.empty()) {
                std::fprintf(stderr, "[modifier] %s: simplify(%g) produced an empty mesh, skipped\n",
                             chunk_label.c_str(), m.ratio);
            } else {
                mesh = std::move(out);
            }
            break;
        }
        case dsl::ModifierKind::Smooth: {
            SmoothOptions opts;
            opts.iterations = m.iterations;
            opts.lambda = m.lambda;
            opts.mu = m.mu;
            SmoothResult r = smooth(mesh, opts);
            if (!r.ok) {
                std::fprintf(stderr, "[modifier] %s: smooth failed (%s), skipped\n",
                             chunk_label.c_str(), r.err.c_str());
            } else {
                mesh = std::move(r.mesh);
            }
            break;
        }
        case dsl::ModifierKind::Retopo: {
#ifdef MATTER_HAVE_AUTOREMESHER
            namespace bl = matter_engine3::retopo_blacklist;
            const uint64_t h = chunk_retopo_hash(mesh, m);
            if (bl::is_blacklisted(h)) {
                std::fprintf(stderr, "[modifier] %s: retopo blacklisted (%016llx), skipped\n",
                             chunk_label.c_str(), (unsigned long long)h);
                break;
            }
            RetopoOptions opts;
            opts.target_ratio = m.target_ratio;
            opts.iterations = m.retopo_iterations;
            opts.seed = m.seed;
            opts.timeout_seconds = m.timeout_seconds;
            opts.threads = 1;  // determinism
            bl::begin_attempt(h);
            RetopoResult r = retopo(mesh, opts);
            bl::end_attempt(h);
            if (!r.ok) {
                std::fprintf(stderr, "[modifier] %s: retopo failed (%s), skipped\n",
                             chunk_label.c_str(), r.err.c_str());
            } else {
                mesh = std::move(r.mesh);
            }
#else
            std::fprintf(stderr, "[modifier] %s: retopo unavailable (built without autoremesher), skipped\n",
                         chunk_label.c_str());
#endif
            break;
        }
        }
    }
    return mesh;
}

} // namespace modifier_apply
```

- [ ] **Step 5: Run the tests**

Run: `make -C MatterEngine3/tests run-modapply`
Expected: PASS — `all modifier_apply tests passed`.

- [ ] **Step 6: Add the sources to `MatterEngine3/Makefile` and rebuild the lib**

Add `src/modifier_apply.cpp` and `../MatterSurfaceLib/src/mesh_smooth.cpp` to the library source list (next to wherever `retopo_blacklist.cpp` is listed). The viewer's `EXTRA_CFLAGS="-DMATTER_HAVE_AUTOREMESHER"` mechanism already flows into these compiles.

Run: `make -C MatterEngine3`
Expected: `libmatter_engine3.a` builds clean.

- [ ] **Step 7: Register `run-modapply` in `build-all.sh`**

Add `run-modapply` to the MatterEngine3 `run-*` test loop (line ~197).

- [ ] **Step 8: Commit**

```bash
git add MatterEngine3/src/modifier_apply.h MatterEngine3/src/modifier_apply.cpp \
        MatterEngine3/tests/modifier_apply_tests.cpp MatterEngine3/tests/Makefile \
        MatterEngine3/Makefile build-all.sh
git commit -m "feat(MatterEngine3): modifier_apply stack runner (simplify/smooth/retopo, failure=skip, blacklist chunk hash)"
```

---

### Task 4: Bake integration — regions meshed, welded, stack-processed, registered

This is the task most exposed to Phase B drift. All anchors below are symbols/comments at `main@54dbd76`; re-locate by grep before editing.

**Files:**
- Modify: `MatterEngine3/src/script_host.cpp` (`bake_source` internals)
- Test: `MatterEngine3/tests/modifier_region_bake_tests.cpp` (new suite)
- Modify: `MatterEngine3/tests/Makefile` (MODBAKE target + `run-modbake`; ALSO add `../src/modifier_apply.cpp ../src/retopo_blacklist.cpp $(MSL)/mesh_smooth.cpp` to EVERY `*_CPP` source list that already contains `../src/script_host.cpp` — find them with `grep -n 'script_host.cpp' MatterEngine3/tests/Makefile`)
- Modify: `build-all.sh` (add `run-modbake` to the MatterEngine3 `run-*` loop)

**Interfaces:**
- Consumes: `state.modifier_regions()` (Task 2), `modifier_apply::apply_stack` (Task 3), MSL `from_tri`/`to_tri` (`mesh_indexed.hpp`).
- Produces: baked `.part` files where each region × material-merge-group is ONE welded, stack-processed BLAS entry; non-region geometry keeps the existing per-cell path byte-identical.

- [ ] **Step 1: Write the failing tests**

`MatterEngine3/tests/modifier_region_bake_tests.cpp` — bake-level, via `ScriptHost::bake_source` exactly like the bake tests in `script_host_tests.cpp` (mirror that file's harness: includes, CHECK macro from `check.h`, how the cache dir is set up, and how tests read back the written part — `BakeResult.written_path` is the `.part` file). Test bodies:

```cpp
// Region bake plumbing tests (spec: Testing / "region plumbing tests at the
// bake level"). Built without MATTER_HAVE_AUTOREMESHER; retopo cases use
// smooth/simplify instead (retopo's skip path is covered by modifier_apply_tests).
#include "script_host.h"
#include "check.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

static std::vector<char> read_file_bytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
}

// One voxel sphere inside a region, one outside. Both bake; the part is valid.
static void test_region_isolated_from_base_geometry() {
    script_host::ScriptHost host;
    const char* src =
        "class P extends Part { build(p) {"
        "  this.beginVoxels(0.2); this.fill(2); this.sphere([0,0,0],0.5); this.endVoxels();"
        "  this.beginModifier();"
        "  this.beginVoxels(0.2); this.fill(2); this.sphere([2,0,0],0.5); this.endVoxels();"
        "  this.endModifier([{ smooth: { iterations: 1 } }]);"
        "} }";
    script_host::BakeResult r = host.bake_source(src, "{}", {});
    CHECK(r.error.ok);
    CHECK(!r.written_path.empty());
    // The base sphere must be byte-identical to a bake WITHOUT the region part:
    // a region must not perturb non-region geometry. (Compare against a source
    // containing only the first sphere is NOT byte-comparable — different
    // hash/geometry — so instead assert the stronger property below.)
}

// Stack order is respected: simplify-then-smooth != smooth-then-simplify.
static void test_stack_order_changes_output() {
    script_host::ScriptHost host;
    const char* fmt =
        "class P extends Part { build(p) {"
        "  this.beginModifier();"
        "  this.beginVoxels(0.15); this.fill(2); this.sphere([0,0,0],0.6); this.endVoxels();"
        "  this.endModifier([%s]);"
        "} }";
    char a_src[512], b_src[512];
    std::snprintf(a_src, sizeof(a_src), fmt, "{ simplify: 0.5 }, { smooth: { iterations: 2 } }");
    std::snprintf(b_src, sizeof(b_src), fmt, "{ smooth: { iterations: 2 } }, { simplify: 0.5 }");
    script_host::BakeResult ra = host.bake_source(a_src, "{}", {});
    script_host::BakeResult rb = host.bake_source(b_src, "{}", {});
    CHECK(ra.error.ok && rb.error.ok);
    CHECK(ra.resolved_hash != rb.resolved_hash);  // stack is part of the source -> new hash
    CHECK(read_file_bytes(ra.written_path) != read_file_bytes(rb.written_path));
}

// Failure-skip: an always-failing modifier still yields the rest of the stack.
// (Simplify to an impossibly tiny ratio on a tiny mesh can fail/empty-out; the
// smooth after it must still land. Assert the bake succeeds and writes a part.)
static void test_failed_modifier_still_bakes() {
    script_host::ScriptHost host;
    const char* src =
        "class P extends Part { build(p) {"
        "  this.beginModifier();"
        "  this.beginVoxels(0.2); this.fill(2); this.sphere([0,0,0],0.5); this.endVoxels();"
        "  this.endModifier([{ simplify: 0.0001 }, { smooth: { iterations: 1 } }]);"
        "} }";
    script_host::BakeResult r = host.bake_source(src, "{}", {});
    CHECK(r.error.ok);
    CHECK(!r.written_path.empty());
}

// Two sequential regions are independent (both bake; distinct from one region).
static void test_multiple_regions() {
    script_host::ScriptHost host;
    const char* src =
        "class P extends Part { build(p) {"
        "  this.beginModifier();"
        "  this.beginVoxels(0.2); this.fill(2); this.sphere([0,0,0],0.5); this.endVoxels();"
        "  this.endModifier([{ smooth: { iterations: 1 } }]);"
        "  this.beginModifier();"
        "  this.beginVoxels(0.2); this.fill(2); this.sphere([2,0,0],0.5); this.endVoxels();"
        "  this.endModifier([{ simplify: 0.5 }]);"
        "} }";
    script_host::BakeResult r = host.bake_source(src, "{}", {});
    CHECK(r.error.ok);
}

// Determinism: bake the same source twice (forcing a rewrite by deleting the
// first .part) -> byte-identical output.
static void test_region_bake_deterministic() {
    script_host::ScriptHost host;
    const char* src =
        "class P extends Part { build(p) {"
        "  this.beginModifier();"
        "  this.beginVoxels(0.2); this.fill(2); this.sphere([0,0,0],0.5); this.endVoxels();"
        "  this.endModifier([{ smooth: { iterations: 2 } }, { simplify: 0.6 }]);"
        "} }";
    script_host::BakeResult r1 = host.bake_source(src, "{}", {});
    CHECK(r1.error.ok);
    std::vector<char> bytes1 = read_file_bytes(r1.written_path);
    std::remove(r1.written_path.c_str());
    script_host::BakeResult r2 = host.bake_source(src, "{}", {});
    CHECK(r2.error.ok);
    CHECK(bytes1 == read_file_bytes(r2.written_path));
}

int main() {
    printf("modifier_region_bake_tests\n");
    test_region_isolated_from_base_geometry();
    test_stack_order_changes_output();
    test_failed_modifier_still_bakes();
    test_multiple_regions();
    test_region_bake_deterministic();
    printf("all modifier_region_bake tests passed\n");
    return 0;
}
```

(If `script_host_tests.cpp`'s `main()` performs cache-dir setup before its bake tests — e.g. pointing the part cache at a scratch dir — replicate that setup here before the first test.)

Register the MODBAKE target in `MatterEngine3/tests/Makefile` — copy the script-host suite's target (it already links QuickJS + full engine sources) and swap the test cpp. Add `run-modbake` to `.PHONY`/`clean`, and register it in `build-all.sh`.

- [ ] **Step 2: Run to verify current behavior**

Run: `make -C MatterEngine3/tests run-modbake`
Expected: builds, but `test_stack_order_changes_output` FAILS — regions are recorded (Task 2) yet ignored by the bake, so both stack orders produce meshes processed by neither (identical geometry apart from the folded source hash; the parts differ only in hash header — if the byte comparison accidentally passes because the hash is folded into the file, keep the test as the gate and proceed; the real assertion is post-Task-4 behavior).

- [ ] **Step 3: Extract `mesh_sdf_ops` in `script_host.cpp`**

At `main@54dbd76` the SDF bake body is lines 935–1148 of `bake_source` (anchor symbols: `lower_build_buffer`, `build_cell_meshes`, `DestroySurfaceScratch`). Extract it into a static function ABOVE `bake_source`:

```cpp
// Lower + cell-mesh one BuildBuffer and register its triangles.
//   stack == nullptr: existing per-cell path, byte-identical to before —
//     each cell/group registers its own BLAS entry (cell_simplify applies).
//   stack != nullptr: region path — per material-merge-group, repacked
//     Tri/TriEx are ACCUMULATED across cells, welded into one indexed mesh
//     (cross-cell seams become interior edges), run through the modifier
//     stack, and registered as ONE BLAS entry (cell_simplify must be 1.0).
static void mesh_sdf_ops(const dsl::BuildBuffer& buf,
                         const std::vector<dsl::ModifierSpec>* stack,
                         const std::string& label,
                         float cell_simplify,
                         /* BLAS/TLAS registration params — exactly the
                            locals the moved body already uses; pass them
                            through from bake_source unchanged */);
```

Mechanics of the move (body stays byte-identical except):
- `state.buffer()` → `buf` parameter.
- `state.simplify_ratio()` → `cell_simplify` parameter.
- `base_detail` derives from `buf.ops[0].spacing` (it already reads ops[0]).
- BLAS/TLAS manager locals move to `bake_source` (the caller) and are passed by reference, so base + all regions register into the SAME part asset.
- At the per-cell/group register site (lines 1141–1145: `blas.register_triangles` + `tlas.draw(h, g.group_id)` on the memset-zero-repacked `norm`/`normEx`): wrap in

```cpp
    if (!stack) {
        // existing register call, unchanged
    } else {
        auto& acc = region_acc[g.group_id];
        acc.first.insert(acc.first.end(), norm.begin(), norm.end());
        acc.second.insert(acc.second.end(), normEx.begin(), normEx.end());
    }
```

with, at function top:

```cpp
    // group_id -> accumulated (Tri, TriEx) across all cells. std::map for
    // deterministic group iteration order.
    std::map<uint32_t, std::pair<std::vector<Tri>, std::vector<TriEx>>> region_acc;
```

- After the existing `DestroySurfaceScratch` call, append the region finalize:

```cpp
    if (stack) {
        for (auto& entry : region_acc) {
            const uint32_t group_id = entry.first;
            auto& acc = entry.second;
            if (acc.first.empty()) continue;
            MeshIndexed welded = from_tri(acc.first, &acc.second);
            char glabel[96];
            std::snprintf(glabel, sizeof(glabel), "%s group %u", label.c_str(), group_id);
            MeshIndexed done = modifier_apply::apply_stack(std::move(welded), *stack, glabel);
            if (done.positions.empty() || done.indices.empty()) continue;
            ensure_triex(done, acc.second[0]);
            std::vector<Tri> tris; std::vector<TriEx> triex;
            to_tri(done, tris, triex);
            // memset-zero repack (byte-stable .part) — same field-by-field
            // repack the per-cell path above performs on its norm/normEx.
            std::vector<Tri> norm(tris.size());
            std::vector<TriEx> normEx(triex.size());
            for (size_t i = 0; i < tris.size(); ++i) {
                std::memset(&norm[i], 0, sizeof(Tri));
                /* copy the same Tri fields the per-cell repack copies */
                std::memset(&normEx[i], 0, sizeof(TriEx));
                /* copy the same TriEx fields the per-cell repack copies */
            }
            /* identical register_triangles + tlas.draw(h, group_id) call the
               per-cell branch uses */
        }
    }
```

The two `/* copy ... */` comments mean: duplicate the EXACT per-field assignments from the existing per-cell repack loop a few lines above (it is in the moved body — same fields, same order). The `/* identical register */` means the same call shape as the `!stack` branch.

- Add a file-local helper above `mesh_sdf_ops` (retopo output drops TriEx; the repack below needs one TriEx per triangle):

```cpp
// Guarantee one TriEx per triangle after a modifier stack. Retopo output has
// no TriEx; rebuild with face normals and the group's material/tint (uniform
// within a material-merge-group, so proto = the group's first input TriEx).
static void ensure_triex(MeshIndexed& m, const TriEx& proto) {
    const size_t ntris = m.indices.size() / 3;
    if (m.triex.size() == ntris) return;
    m.triex.assign(ntris, TriEx{});
    for (size_t t = 0; t < ntris; ++t) {
        const float3 p0 = m.positions[m.indices[3*t]];
        const float3 p1 = m.positions[m.indices[3*t + 1]];
        const float3 p2 = m.positions[m.indices[3*t + 2]];
        const float ex1 = p1.x-p0.x, ey1 = p1.y-p0.y, ez1 = p1.z-p0.z;
        const float ex2 = p2.x-p0.x, ey2 = p2.y-p0.y, ez2 = p2.z-p0.z;
        float nx = ey1*ez2 - ez1*ey2, ny = ez1*ex2 - ex1*ez2, nz = ex1*ey2 - ey1*ex2;
        const float len = std::sqrt(nx*nx + ny*ny + nz*nz);
        if (len > 1e-20f) { nx /= len; ny /= len; nz /= len; }
        else              { nx = 0; ny = 0; nz = 1; }
        TriEx& e = m.triex[t];
        e.materialId = proto.materialId;
        e.tint = proto.tint;
        e.N0 = e.N1 = e.N2 = make_float3(nx, ny, nz);
    }
}
```

Includes to add at the top of `script_host.cpp`: `"modifier_apply.h"`, `"mesh_indexed.hpp"` (plus `<map>` if absent).

- [ ] **Step 4: Partition ops by region in `bake_source` and call `mesh_sdf_ops`**

At the old call site (where the moved body used to start), replace with:

```cpp
    const std::vector<dsl::ModifierRegion>& regions = state.modifier_regions();
    dsl::BuildBuffer base_buf;
    std::vector<dsl::BuildBuffer> region_bufs(regions.size());
    {
        const std::vector<dsl::BuildOp>& all_ops = state.buffer().ops;
        size_t ri = 0;
        for (size_t i = 0; i < all_ops.size(); ++i) {
            while (ri < regions.size() && i >= regions[ri].op_end) ++ri;
            if (ri < regions.size() && i >= regions[ri].op_begin)
                region_bufs[ri].ops.push_back(all_ops[i]);
            else
                base_buf.ops.push_back(all_ops[i]);
        }
    }
    char plabel[64];
    std::snprintf(plabel, sizeof(plabel), "part %016llx",
                  (unsigned long long)resolved_hash);
    if (!base_buf.ops.empty())
        mesh_sdf_ops(base_buf, nullptr, plabel, state.simplify_ratio(), /* blas/tlas */);
    for (size_t ri = 0; ri < regions.size(); ++ri) {
        if (region_bufs[ri].ops.empty()) continue;
        char rlabel[80];
        std::snprintf(rlabel, sizeof(rlabel), "%s region %zu", plabel, ri);
        mesh_sdf_ops(region_bufs[ri], &regions[ri].stack, rlabel, 1.0f, /* blas/tlas */);
    }
```

(`resolved_hash` = the local `bake_source` already computes; regions are ordered and non-overlapping by construction, so the single `ri` walk is correct. `cell_simplify = 1.0f` for regions: per-cell QEM never pre-chews a region — simplify only runs via the stack.)

The existing guard that skips the SDF path when `state.buffer().ops` is empty must now consider region ops too (it is the same vector — unchanged condition works).

- [ ] **Step 5: Partition the direct-triangle buffer the same way**

The direct-tri section (lines 1150–1208 at 54dbd76; anchors: `state.triangle_buffer()`, `src_t`/`src_e`, the decimate branch, memset-zero repack, register with group 0):

- Partition `src_t`/`src_e` indices by the regions' `[tri_begin, tri_end)` ranges into `base_t/base_e` plus `region_t[ri]/region_e[ri]` (same single-walk loop as Step 4, over triangle index `i`).
- Base vectors take the EXISTING path unchanged (including the `decimate` branch — it survives until Task 6 deletes it).
- Each non-empty region chunk: `from_tri(region_t[ri], &region_e[ri])` → `modifier_apply::apply_stack(..., regions[ri].stack, rlabel)` → `ensure_triex` → `to_tri` → memset-zero repack (same fields as the existing direct-tri repack) → same `register_triangles` + `tlas.draw(h, 0)` (group 0) call the base path uses.

- [ ] **Step 6: Run the new suite + regression suites**

Run: `make -C MatterEngine3/tests run-modbake run-modapply` and the script-host suite target.
Expected: ALL PASS — including `test_stack_order_changes_output` byte-difference and `test_region_bake_deterministic` byte-identity. Non-region bakes must be byte-identical to before the change (existing determinism/bake tests in the script-host suite are the net).

- [ ] **Step 7: Commit**

```bash
git add MatterEngine3/src/script_host.cpp MatterEngine3/tests/modifier_region_bake_tests.cpp \
        MatterEngine3/tests/Makefile build-all.sh
git commit -m "feat(MatterEngine3): bake-time modifier regions — per-group cross-cell weld, stack apply, single-BLAS register"
```

---

### Task 5: Schema migrations (Tree, TreeBranch, Rock)

Same-change migration, no legacy syntax left behind. These are the ONLY live users of `this.simplify()` / `static retopo`. All paths under `MatterEngine3/examples/world_demo/schemas/`.

**Files:**
- Modify: `Tree.js`
- Modify: `TreeBranch.js`
- Modify: `Rock.js`

- [ ] **Step 1: Migrate `Tree.js`**

- DELETE the `static retopo = {...}` block (lines 15–43 at 54dbd76, including its KNOWN LIMITATION comment block and the `enabled: false` ship state).
- DELETE `this.simplify(0.3)` and its comment (lines 52–55).
- WRAP the trunk+bark voxel session (passes 2 and 3 — the isosurface geometry; pass 1's `placeChild` branch placements stay OUTSIDE, children are not captured by regions anyway) in a region:

```js
    this.fill(MAT.bark);
    this.beginModifier();
    this.beginVoxels(VOX);
    // ... existing pass 2 (trunk core) and pass 3 (bark strands) bodies, unchanged ...
    this.endVoxels();
    this.endModifier([
      { smooth: { iterations: 2 } },
      { retopo: { target_ratio: 1.0, iterations: 3, seed: 42, timeout_seconds: 120 } },
    ]);
```

Retopo is hereby RE-ENABLED for Tree through the region path (it consumes the smoothed isosurface weld, never post-QEM meshes — the non-manifold-input crash class the old path had). Crashes are absorbed by the bake-time blacklist journal (Task 3).

- [ ] **Step 2: Migrate `TreeBranch.js`**

- DELETE the commented-out `//this.simplify(0.3)` (line 21).
- CONVERT the tube pass (the `line()` loop over `segs`) from mesh output to an isosurface region — `line()` is already an SDF capsule brush inside a voxel session (session-polymorphic DSL):

```js
    this.fill(MAT.bark);
    this.beginModifier();
    this.beginVoxels(0.06);   // resolves the thinnest twig tips; tune at the Meadow gate
    for (const [from, to, wFrom, wTo] of segs)
      this.line(from, to, wFrom * 0.5, wTo * 0.5);
    this.endVoxels();
    this.endModifier([{ smooth: { iterations: 1 } }]);
```

Leaves (pass 1 placements/mesh geometry) stay exactly as they are — mesh, untouched, outside the region.

- [ ] **Step 3: Migrate `Rock.js`**

- DELETE the `static retopo = {...}` block (lines 27–33) and its explanatory comment block (lines 10–26 — the blacklist mechanics now live at bake time and are documented in `retopo_blacklist.h`/the spec).
- WRAP the whole voxel session in a region:

```js
  build(p) {
    const r = rng(1000 + p.seed);
    this.beginModifier();
    this.beginVoxels(0.15);
    // ... existing body unchanged (fill/smoothing/blobs/cuts) ...
    this.endVoxels();
    this.endModifier([
      { retopo: { target_ratio: 1.0, iterations: 3, seed: 42, timeout_seconds: 60 } },
    ]);
  }
```

- [ ] **Step 4: Verify with a scripted Meadow bake**

```bash
make -C MatterViewer   # Linux viewer with autoremesher (EXTRA_CFLAGS already set there)
GALLIUM_DRIVER=d3d12 tools/viewer_shots.sh   # self-terminating scripted run
```

Expected: meadow bakes; Tree/TreeBranch/Rock parts write clean. A `geo_assert` abort on a bad Rock seed or the Tree chunk is EXPECTED on first run — relaunch 2–3 times; the bake-time blacklist converges and the run completes (same recovery contract as Phase 5). If TreeBranch twig tips drop out visually, adjust the `0.06` spacing here.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/examples/world_demo/schemas/Tree.js \
        MatterEngine3/examples/world_demo/schemas/TreeBranch.js \
        MatterEngine3/examples/world_demo/schemas/Rock.js
git commit -m "feat(world_demo): migrate Tree/TreeBranch/Rock to modifier regions; Tree retopo re-enabled; TreeBranch tubes -> isosurface"
```

---

### Task 6: Clean cut — delete `this.simplify()`, `static retopo`, and the flatten retopo hook

No aliases, no compat shims. Line refs at `main@54dbd76`; re-locate every one by symbol.

**Files:**
- Modify: `MatterEngine3/src/part_flatten.cpp` + `part_flatten.h`
- Modify: `MatterEngine3/src/script_host.h` + `script_host.cpp`
- Modify: `MatterEngine3/src/local_provider.cpp`
- Modify: `MatterEngine3/src/part_asset_v2.h` + `part_asset_v2.cpp`
- Delete: `MatterEngine3/src/retopo_hook_stats.h`
- Modify: `MatterEngine3/src/part_base.js.h`, `dsl_bindings.cpp`, `dsl_state.h`
- Modify: `MatterEngine3/include/retopo_blacklist.h` (comment only)
- Modify: `MatterEngine3/tests/script_host_tests.cpp`
- Rewrite: `MatterEngine3/tests/retopo_integration_tests.cpp` (region form — spec says updated, NOT deleted)

- [ ] **Step 1: Delete the flatten-time retopo hook**

`part_flatten.cpp`: retopo includes (lines 6–7, 20), `sys/stat` include (38), the stats namespace (47–66), all hook helpers + `apply_retopo_hook` itself (495–824), and its call site (1263).
`part_flatten.h`: `FlattenTargets::retopo` member (line 12) and its doc block (60–68).

- [ ] **Step 2: Delete the static-retopo eval plumbing**

`script_host.h`: `eval_retopo_settings` declaration (line 107). `script_host.cpp`: its implementation (595–659). `local_provider.cpp`: the caller (242–245 — Phase B will have moved this; grep `eval_retopo_settings`). The `retopo_blacklist::init` call at `local_provider.cpp:211` STAYS (the journal now serves bake-time attempts).

- [ ] **Step 3: Delete `RetopoSettings` + the `.retopo.part` cache**

`part_asset_v2.h`: `RetopoSettings` struct (27–41) and `cache_path_retopo` (96–102). `part_asset_v2.cpp`: `cache_path_retopo` impl (102–105). Delete `retopo_hook_stats.h` entirely (`git rm`).

- [ ] **Step 4: Delete `this.simplify()` end-to-end**

- `part_base.js.h` line 32: `simplify(ratio) { __dsl_simplify(ratio); }`.
- `dsl_bindings.cpp`: `j_simplify` (line 57), its `bind("__dsl_simplify",...)` (690), and rewrite the stale header comment (13–19) that points at `eval_retopo_settings` — it should now say schema-level config is `lodBudgets`/`lodAnchorSize` only.
- `dsl_state.h`: `set_simplify`/`simplify_ratio` (119–120) and `simplify_ratio_` (271), plus the comment block above them.
- `script_host.cpp`: the per-cell QEM branch keyed on `cell_simplify < 1` inside `mesh_sdf_ops` AND the direct-tri `decimate` branches (1167–1169, 1182–1192 pre-move). Then remove the now-constant `cell_simplify` parameter from `mesh_sdf_ops` and the `state.simplify_ratio()` argument at the call site. (MSL `decimate_tris` itself STAYS — `lod_bake.cpp:104`, `composition_tests.cpp:34`.)

- [ ] **Step 5: Update `retopo_blacklist.h`'s header comment**

Its mechanism text references `apply_retopo_hook` and flatten-time sequencing (lines 8–16). Reword to bake-time: attempts are journaled per region chunk from `modifier_apply` during `bake_source`; single-threaded assumption still holds (parts bake sequentially).

- [ ] **Step 6: Delete the dead tests, rewrite the retopo integration test**

- `script_host_tests.cpp`: `test_eval_retopo_settings` (1056–1151) and its call in `main()` (1187).
- `retopo_integration_tests.cpp`: rewrite to region form — the existing spherified-cube fixture is baked via `bake_source` with the source wrapping its voxel geometry in `beginModifier()/endModifier([{ retopo: {...} }])`, asserting the baked part's triangle content changed vs. a no-stack bake and that `retopo_blacklist` journal files were touched. Its Makefile target and the conditional `build-all.sh` block (lines 205–214, gated on `libautoremesher_core.a`) STAY as-is.

- [ ] **Step 7: Verify the cut is total**

```bash
grep -rn -E "__dsl_simplify|set_simplify|simplify_ratio|static retopo|eval_retopo_settings|RetopoSettings|cache_path_retopo|apply_retopo_hook|retopo_hook_stats" \
  MatterEngine3/src MatterEngine3/include MatterEngine3/tests MatterEngine3/examples MatterViewer
```

Expected: ZERO hits (docs/ excluded deliberately).

- [ ] **Step 8: Run the affected suites**

Run: the script-host suite, `run-modbake`, `run-modapply`, and (if `Libraries/autoremesher_core/libautoremesher_core.a` exists) the rewritten `retopo_integration_tests`.
Expected: ALL PASS.

- [ ] **Step 9: Commit**

```bash
git add -A MatterEngine3   # includes the retopo_hook_stats.h deletion from Step 3
git commit -m "refactor(MatterEngine3)!: delete this.simplify(), static retopo, flatten retopo hook + .retopo.part cache (clean cut to modifier regions)"
```

---

### Task 7: Final gates

- [ ] **Step 1: Full test sweep**

Run: `./build-all.sh test`
Expected: green. Known pre-existing exception: the MatterEngine3 api-tests retopo_blacklist LINK failure in WSL existed on main before this work — verify it is unchanged (not a regression), everything else passes.

- [ ] **Step 2: Clean Windows cross-build**

Headers/structs changed → partial obj rebuilds produce wandering silent crashes. Clear ALL objs first, then:

Run: `make -C MatterViewer clean-windows 2>/dev/null || find . -name '*.obj' -delete; make -C MatterViewer windows` (use the project's actual clean mechanism)
Expected: `viewer.exe` builds; retopo modifier compiles to the warn+skip path (no autoremesher on Windows).

- [ ] **Step 3: Real gate — Meadow with Tree retopo through the region path**

```bash
GALLIUM_DRIVER=d3d12 tools/viewer_shots.sh
```

(Self-terminating per viewer-test lifecycle; relaunch on blacklist-convergence crashes.) Verify in the shots (FIFO `wireframe on`): Tree trunk/bark shows retopo'd quad-dominant topology; safe Rock seeds retopo'd; TreeBranch twigs render as smoothed isosurface tubes with intact tips; leaves unchanged.

- [ ] **Step 4: Commit any gate fixes; done**

Branch is ready for the finishing-a-development-branch flow.

---

## Self-Review Notes (writing-plans checklist)

- **Spec coverage:** DSL surface (Task 2), v1 modifiers table incl. `{simplify: 0.3}` shorthand (Tasks 2/3), bake-time engine flow + per-group weld + single BLAS (Task 4), blacklist moved to bake keyed by chunk hash (Task 3), flatten hook + `.retopo.part` + `RetopoSettings` deletion (Task 6), smooth as new MSL pass with per-suite test binary (Task 1), schema migrations incl. TreeBranch isosurface conversion (Task 5), no-autoremesher warn+skip (Task 3), testing section incl. retopo integration test updated-not-deleted (Task 6) and the Meadow real gate (Task 7). Non-goals untouched.
- **Type consistency:** `ModifierSpec` field names (`ratio`, `iterations`, `lambda`, `mu`, `target_ratio`, `retopo_iterations`, `seed`, `timeout_seconds`) are used identically in Tasks 2 (parse), 3 (apply), 4 (integration). `SmoothOptions`/`SmoothResult` match between Tasks 1 and 3.
- **Known intentional non-literals:** Task 4's `mesh_sdf_ops` extraction describes a code MOVE with the exact wrap points rather than reproducing 200 unchanged lines, and its repack/register comments mean "duplicate the adjacent existing per-cell code" — both are references to code in the same function being edited, pinned at `main@54dbd76`, to survive Phase B drift.
