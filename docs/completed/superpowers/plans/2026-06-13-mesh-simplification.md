# Mesh Simplification Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a CPU QEM edge-collapse mesh simplifier to MatterSurfaceLib that decimates marching-cubes cell meshes while hard-locking cell-boundary vertices so seams between same-level neighbor cells stay watertight.

**Architecture:** A standalone, GL-free module (`mesh_simplifier.hpp`/`.cpp`) consumes a raylib `Mesh` and returns a new indexed `Mesh`. It is unit-tested headless. `Cell` calls it after marching-cubes generation when the cluster's simplification ratio is below 1.0; a UI slider drives the ratio. The simplifier is pure geometry — no normal/detail baking.

**Tech Stack:** C++14 (matches MatterSurfaceLib Makefile), raylib `Mesh`/`MemAlloc`, plain `assert`/`printf` tests (matches existing `tests/` harness).

**Spec:** `docs/superpowers/specs/2026-06-13-mesh-simplification-design.md`

---

## File Structure

- **Create** `MatterSurfaceLib/include/mesh_simplifier.hpp` — public API: `SimplifyOptions`, `CellBounds`, `simplify_mesh()`.
- **Create** `MatterSurfaceLib/src/mesh_simplifier.cpp` — implementation (topology build/weld, QEM decimation, mesh rebuild, normal recompute). All internals in an anonymous namespace.
- **Create** `MatterSurfaceLib/tests/mesh_simplifier_tests.cpp` — headless unit tests (no GL/InitWindow).
- **Modify** `MatterSurfaceLib/tests/Makefile` — add `mesh_simplifier_tests` target.
- **Modify** `MatterSurfaceLib/Makefile` — add `src/mesh_simplifier.cpp` to `SRC`/`OBJ` and a build rule (so the main app links it).
- **Modify** `MatterSurfaceLib/include/cell.h` — add `simplification_ratio` param to `rebuild_meshes` / `generate_mesh_for_material`.
- **Modify** `MatterSurfaceLib/src/cell.cpp` — call `simplify_mesh` after `GenerateMesh`.
- **Modify** `MatterSurfaceLib/include/cluster.h` — add `simplification_ratio_` member + setter/getter.
- **Modify** `MatterSurfaceLib/src/cluster.cpp` — thread ratio into the `rebuild_meshes` call.
- **Modify** `MatterSurfaceLib/main.cpp` — add a "Simplification" slider in `render_ui()` + a `simplification_ratio_` member.
- **Modify** `build-all.sh` — build and run the new test in the `test` mode loop.

**Note on the tests Makefile paths:** From `MatterSurfaceLib/tests/`, `../include` is the module headers, `../../Libraries/raylib/src` is raylib headers, and `../../Libraries/raylib/src/libraylib.a` is the static lib. The existing `INCLUDE_PATHS`/`LDLIBS` already encode these — reuse them verbatim.

---

## Task 1: Module scaffold — topology, mesh rebuild, trivial cases, test harness

This task creates the module with everything **except** the decimation engine. `simplify_mesh` builds working topology (welding non-indexed input), runs a no-op `decimate` stub, then rebuilds an indexed `Mesh` with recomputed normals. At ratio 1.0 (and with the stub) it returns a geometrically equivalent mesh; empty/degenerate inputs are handled. Task 2 fills in the stub.

**Files:**
- Create: `MatterSurfaceLib/include/mesh_simplifier.hpp`
- Create: `MatterSurfaceLib/src/mesh_simplifier.cpp`
- Create: `MatterSurfaceLib/tests/mesh_simplifier_tests.cpp`
- Modify: `MatterSurfaceLib/tests/Makefile`

- [ ] **Step 1: Create the public header**

Create `MatterSurfaceLib/include/mesh_simplifier.hpp`:

```cpp
#ifndef MESH_SIMPLIFIER_HPP
#define MESH_SIMPLIFIER_HPP

#include "raylib.h"
#include <cfloat>

// Options controlling QEM edge-collapse decimation.
struct SimplifyOptions {
    float target_ratio  = 0.5f;     // fraction of triangles to keep, (0..1]
    float max_error     = FLT_MAX;  // stop once min collapse cost exceeds this
    bool  lock_boundary = true;     // freeze vertices lying on a cell face plane
};

// Axis-aligned cell extent in cluster-local space. When supplied to
// simplify_mesh and lock_boundary is true, vertices on any of the 6 face
// planes are never moved or removed (guarantees watertight same-level seams).
struct CellBounds {
    Vector3 min_bound;
    Vector3 max_bound;
};

// Returns a NEW indexed Mesh allocated with raylib's allocator (MemAlloc),
// safe to pass to UploadMesh/UnloadMesh. Does NOT mutate or free `input`.
// On empty/degenerate input returns a zeroed Mesh (vertexCount == 0).
Mesh simplify_mesh(const Mesh& input,
                   const SimplifyOptions& opts,
                   const CellBounds* bounds = nullptr);

#endif // MESH_SIMPLIFIER_HPP
```

- [ ] **Step 2: Create the implementation with topology + rebuild + a decimate stub**

Create `MatterSurfaceLib/src/mesh_simplifier.cpp`:

```cpp
#include "mesh_simplifier.hpp"

#include <vector>
#include <map>
#include <array>
#include <cmath>
#include <cstdint>

namespace {

// --- minimal double-precision vector helpers (avoids raymath coupling) ---
struct V3 { double x, y, z; };
static inline V3 sub(V3 a, V3 b)   { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static inline V3 cross(V3 a, V3 b) { return {a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x}; }
static inline double dot(V3 a, V3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline double len(V3 a)       { return std::sqrt(dot(a, a)); }

// --- accumulated quadric (Garland-Heckbert), used by the Task 2 engine ---
struct Quadric {
    double a00=0,a01=0,a02=0,a11=0,a12=0,a22=0; // symmetric 3x3 A
    double b0=0,b1=0,b2=0;                       // vector b
    double c=0;                                  // scalar c
    void addPlane(V3 n, double d) {
        a00+=n.x*n.x; a01+=n.x*n.y; a02+=n.x*n.z;
        a11+=n.y*n.y; a12+=n.y*n.z; a22+=n.z*n.z;
        b0+=d*n.x; b1+=d*n.y; b2+=d*n.z; c+=d*d;
    }
    void add(const Quadric& q) {
        a00+=q.a00; a01+=q.a01; a02+=q.a02; a11+=q.a11; a12+=q.a12; a22+=q.a22;
        b0+=q.b0; b1+=q.b1; b2+=q.b2; c+=q.c;
    }
    double error(V3 v) const {
        double e = a00*v.x*v.x + 2*a01*v.x*v.y + 2*a02*v.x*v.z
                 + a11*v.y*v.y + 2*a12*v.y*v.z + a22*v.z*v.z
                 + 2*(b0*v.x + b1*v.y + b2*v.z) + c;
        return e < 0 ? 0 : e;
    }
    bool optimal(V3& out) const {
        double m00 = a11*a22 - a12*a12;
        double m01 = a02*a12 - a01*a22;
        double m02 = a01*a12 - a02*a11;
        double det = a00*m00 + a01*m01 + a02*m02;
        if (std::fabs(det) < 1e-12) return false;
        double m11 = a00*a22 - a02*a02;
        double m12 = a01*a02 - a00*a12;
        double m22 = a00*a11 - a01*a01;
        double inv = 1.0 / det;
        out.x = -inv*(m00*b0 + m01*b1 + m02*b2);
        out.y = -inv*(m01*b0 + m11*b1 + m12*b2);
        out.z = -inv*(m02*b0 + m12*b1 + m22*b2);
        return true;
    }
};

struct WVert {
    V3 pos {0,0,0};
    Quadric q;
    bool locked = false;
    bool removed = false;
    int  version = 0;
};
struct WTri {
    int v[3] = {0,0,0};
    bool removed = false;
};

// Build working topology from the input mesh. Indexed input is read directly;
// non-indexed input is welded by exact-quantized position so edge-collapse has
// real connectivity.
static void buildTopology(const Mesh& m, std::vector<WVert>& verts, std::vector<WTri>& tris) {
    if (m.indices) {
        verts.resize(m.vertexCount);
        for (int i = 0; i < m.vertexCount; ++i) {
            verts[i].pos = {m.vertices[i*3+0], m.vertices[i*3+1], m.vertices[i*3+2]};
        }
        tris.resize(m.triangleCount);
        for (int t = 0; t < m.triangleCount; ++t) {
            tris[t].v[0] = m.indices[t*3+0];
            tris[t].v[1] = m.indices[t*3+1];
            tris[t].v[2] = m.indices[t*3+2];
        }
    } else {
        std::map<std::array<long long,3>, int> weld;
        tris.resize(m.triangleCount);
        for (int t = 0; t < m.triangleCount; ++t) {
            for (int k = 0; k < 3; ++k) {
                int src = t*3 + k;
                float x = m.vertices[src*3+0], y = m.vertices[src*3+1], z = m.vertices[src*3+2];
                std::array<long long,3> key = {
                    (long long)std::llround((double)x * 100000.0),
                    (long long)std::llround((double)y * 100000.0),
                    (long long)std::llround((double)z * 100000.0)
                };
                auto it = weld.find(key);
                int vi;
                if (it == weld.end()) {
                    vi = (int)verts.size();
                    WVert w; w.pos = {x, y, z};
                    verts.push_back(w);
                    weld[key] = vi;
                } else {
                    vi = it->second;
                }
                tris[t].v[k] = vi;
            }
        }
    }
}

// Compact surviving verts/tris into a new indexed Mesh with smooth
// area-weighted vertex normals (matches the marching-cubes convention).
static Mesh buildMesh(const std::vector<WVert>& verts, const std::vector<WTri>& tris) {
    Mesh out = {0};
    std::vector<int> remap(verts.size(), -1);
    int nv = 0;
    for (size_t i = 0; i < verts.size(); ++i)
        if (!verts[i].removed) remap[i] = nv++;

    std::vector<unsigned short> idx;
    for (const auto& tr : tris) {
        if (tr.removed) continue;
        idx.push_back((unsigned short)remap[tr.v[0]]);
        idx.push_back((unsigned short)remap[tr.v[1]]);
        idx.push_back((unsigned short)remap[tr.v[2]]);
    }
    int nt = (int)idx.size() / 3;
    if (nv == 0 || nt == 0) return out; // zeroed -> empty mesh

    out.vertexCount = nv;
    out.triangleCount = nt;
    out.vertices = (float*)MemAlloc(sizeof(float) * 3 * nv);
    out.normals  = (float*)MemAlloc(sizeof(float) * 3 * nv);
    out.indices  = (unsigned short*)MemAlloc(sizeof(unsigned short) * idx.size());

    for (size_t i = 0; i < verts.size(); ++i) {
        if (verts[i].removed) continue;
        int j = remap[i];
        out.vertices[j*3+0] = (float)verts[i].pos.x;
        out.vertices[j*3+1] = (float)verts[i].pos.y;
        out.vertices[j*3+2] = (float)verts[i].pos.z;
    }
    for (size_t i = 0; i < idx.size(); ++i) out.indices[i] = idx[i];

    for (int i = 0; i < nv*3; ++i) out.normals[i] = 0.0f;
    for (int t = 0; t < nt; ++t) {
        int a = idx[t*3+0], b = idx[t*3+1], c = idx[t*3+2];
        V3 va = {out.vertices[a*3+0], out.vertices[a*3+1], out.vertices[a*3+2]};
        V3 vb = {out.vertices[b*3+0], out.vertices[b*3+1], out.vertices[b*3+2]};
        V3 vc = {out.vertices[c*3+0], out.vertices[c*3+1], out.vertices[c*3+2]};
        V3 n = cross(sub(vb, va), sub(vc, va)); // area-weighted (twice area)
        int ix[3] = {a, b, c};
        for (int k = 0; k < 3; ++k) {
            out.normals[ix[k]*3+0] += (float)n.x;
            out.normals[ix[k]*3+1] += (float)n.y;
            out.normals[ix[k]*3+2] += (float)n.z;
        }
    }
    for (int i = 0; i < nv; ++i) {
        V3 nn = {out.normals[i*3+0], out.normals[i*3+1], out.normals[i*3+2]};
        double l = len(nn);
        if (l > 1e-12) {
            out.normals[i*3+0] /= (float)l;
            out.normals[i*3+1] /= (float)l;
            out.normals[i*3+2] /= (float)l;
        } else {
            out.normals[i*3+0] = 0; out.normals[i*3+1] = 1; out.normals[i*3+2] = 0;
        }
    }
    return out;
}

// Decimation engine. STUB in Task 1 (does nothing); implemented in Task 2.
static void decimate(std::vector<WVert>& verts, std::vector<WTri>& tris,
                     const SimplifyOptions& opts, const CellBounds* bounds, int inputTri) {
    (void)verts; (void)tris; (void)opts; (void)bounds; (void)inputTri;
}

} // anonymous namespace

Mesh simplify_mesh(const Mesh& input, const SimplifyOptions& opts, const CellBounds* bounds) {
    if (input.vertexCount == 0 || input.triangleCount == 0 || !input.vertices) {
        Mesh empty = {0};
        return empty;
    }
    std::vector<WVert> verts;
    std::vector<WTri> tris;
    buildTopology(input, verts, tris);
    int inputTri = (int)tris.size();
    decimate(verts, tris, opts, bounds, inputTri);
    return buildMesh(verts, tris);
}
```

- [ ] **Step 3: Write the failing test file**

Create `MatterSurfaceLib/tests/mesh_simplifier_tests.cpp`. (Tests for reduction/flip/boundary/watertight are added in Task 2; this set covers scaffold behavior.)

```cpp
#include <cstdio>
#include <cassert>
#include <cmath>
#include <vector>

#include "raylib.h"
#include "mesh_simplifier.hpp"

// Build an indexed Mesh from raw vertex + index arrays (CPU-only, no GL upload).
static Mesh makeMesh(const std::vector<float>& v, const std::vector<unsigned short>& idx) {
    Mesh m = {0};
    m.vertexCount = (int)(v.size() / 3);
    m.triangleCount = (int)(idx.size() / 3);
    m.vertices = (float*)MemAlloc(sizeof(float) * v.size());
    for (size_t i = 0; i < v.size(); ++i) m.vertices[i] = v[i];
    m.indices = (unsigned short*)MemAlloc(sizeof(unsigned short) * idx.size());
    for (size_t i = 0; i < idx.size(); ++i) m.indices[i] = idx[i];
    return m;
}

// A flat 2x2 grid of quads on the z=0 plane spanning [0,2]x[0,2] -> 8 triangles.
static Mesh makeGrid(int n /*cells per side*/, float span) {
    std::vector<float> v;
    std::vector<unsigned short> idx;
    int side = n + 1;
    for (int j = 0; j < side; ++j)
        for (int i = 0; i < side; ++i) {
            v.push_back(span * (float)i / (float)n);
            v.push_back(span * (float)j / (float)n);
            v.push_back(0.0f);
        }
    auto vid = [&](int i, int j) { return (unsigned short)(j*side + i); };
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            idx.push_back(vid(i, j));   idx.push_back(vid(i+1, j));   idx.push_back(vid(i+1, j+1));
            idx.push_back(vid(i, j));   idx.push_back(vid(i+1, j+1)); idx.push_back(vid(i, j+1));
        }
    return makeMesh(v, idx);
}

static void test_empty_input() {
    printf("=== test_empty_input ===\n");
    Mesh empty = {0};
    Mesh out = simplify_mesh(empty, SimplifyOptions{});
    assert(out.vertexCount == 0 && out.triangleCount == 0);
    printf("PASSED\n");
}

static void test_single_triangle() {
    printf("=== test_single_triangle ===\n");
    std::vector<float> v = {0,0,0, 1,0,0, 0,1,0};
    std::vector<unsigned short> idx = {0,1,2};
    Mesh in = makeMesh(v, idx);
    SimplifyOptions o; o.target_ratio = 0.1f; // ask for fewer, but 1 tri is the floor
    Mesh out = simplify_mesh(in, o);
    assert(out.triangleCount == 1);
    UnloadMesh(in); UnloadMesh(out);
    printf("PASSED\n");
}

static void test_identity_ratio_one() {
    printf("=== test_identity_ratio_one ===\n");
    Mesh in = makeGrid(2, 2.0f); // 9 verts, 8 tris
    SimplifyOptions o; o.target_ratio = 1.0f;
    Mesh out = simplify_mesh(in, o);
    assert(out.triangleCount == in.triangleCount);
    assert(out.vertexCount == in.vertexCount);
    UnloadMesh(in); UnloadMesh(out);
    printf("PASSED\n");
}

static void test_weld_non_indexed() {
    printf("=== test_weld_non_indexed ===\n");
    // Two triangles sharing an edge, expressed WITHOUT indices (6 verts, 2 shared).
    std::vector<float> v = {
        0,0,0, 1,0,0, 1,1,0,   // tri 0
        0,0,0, 1,1,0, 0,1,0    // tri 1 (shares 0,0,0 and 1,1,0)
    };
    Mesh in = {0};
    in.vertexCount = 6;
    in.triangleCount = 2;
    in.vertices = (float*)MemAlloc(sizeof(float) * v.size());
    for (size_t i = 0; i < v.size(); ++i) in.vertices[i] = v[i];
    // indices == NULL -> simplifier must weld
    SimplifyOptions o; o.target_ratio = 1.0f;
    Mesh out = simplify_mesh(in, o);
    assert(out.vertexCount == 4);   // welded down to 4 unique corners
    assert(out.triangleCount == 2);
    UnloadMesh(in); UnloadMesh(out);
    printf("PASSED\n");
}

static void test_normals_unit_length() {
    printf("=== test_normals_unit_length ===\n");
    Mesh in = makeGrid(2, 2.0f);
    Mesh out = simplify_mesh(in, SimplifyOptions{}); // ratio 0.5; with Task1 stub still valid mesh
    assert(out.normals != nullptr);
    for (int i = 0; i < out.vertexCount; ++i) {
        float nx = out.normals[i*3+0], ny = out.normals[i*3+1], nz = out.normals[i*3+2];
        float l = sqrtf(nx*nx + ny*ny + nz*nz);
        assert(fabsf(l - 1.0f) < 1e-3f);
    }
    UnloadMesh(in); UnloadMesh(out);
    printf("PASSED\n");
}

int main() {
    printf("=== Mesh Simplifier Tests ===\n");
    test_empty_input();
    test_single_triangle();
    test_identity_ratio_one();
    test_weld_non_indexed();
    test_normals_unit_length();
    printf("\nAll mesh simplifier scaffold tests PASSED\n");
    return 0;
}
```

- [ ] **Step 4: Add the test target to the tests Makefile**

In `MatterSurfaceLib/tests/Makefile`, after the existing `TARGET = minimal_cell_test` block (around line 53) and before the build rules, add a second target. Append these lines to the file (after the `debug:` rule):

```makefile

# Mesh simplifier unit tests (headless, no GL window required)
SIMP_TARGET = mesh_simplifier_tests
SIMP_SOURCES = mesh_simplifier_tests.cpp ../src/mesh_simplifier.cpp

$(SIMP_TARGET): $(SIMP_SOURCES)
	$(CC) $(SIMP_SOURCES) -o $(SIMP_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)

run-simp: $(SIMP_TARGET)
	./$(SIMP_TARGET)
```

Also extend the `clean` rule to remove it. Change:

```makefile
clean:
	rm -f $(TARGET)
```
to:
```makefile
clean:
	rm -f $(TARGET) $(SIMP_TARGET)
```

And add `run-simp` to `.PHONY`:
```makefile
.PHONY: clean run run-simp
```

- [ ] **Step 5: Build and run — verify the scaffold tests pass**

Run: `make -C MatterSurfaceLib/tests mesh_simplifier_tests && ./MatterSurfaceLib/tests/mesh_simplifier_tests`
Expected: compiles cleanly; output ends with `All mesh simplifier scaffold tests PASSED`.

- [ ] **Step 6: Commit**

```bash
git add MatterSurfaceLib/include/mesh_simplifier.hpp \
        MatterSurfaceLib/src/mesh_simplifier.cpp \
        MatterSurfaceLib/tests/mesh_simplifier_tests.cpp \
        MatterSurfaceLib/tests/Makefile
git commit -m "feat: mesh simplifier scaffold (topology, rebuild, tests)"
```

---

## Task 2: QEM decimation engine

Replace the `decimate` stub with the real Garland-Heckbert edge-collapse engine: per-vertex quadrics, a min-heap of edges keyed by collapse cost, greedy collapse with triangle-flip/degeneracy rejection, and hard boundary-vertex locking. Then add the tests that exercise reduction, determinism, flip-prevention, boundary preservation, and watertight seams.

**Files:**
- Modify: `MatterSurfaceLib/src/mesh_simplifier.cpp` (replace the `decimate` stub and add `HeapEdge`/`buildEdge` above it)
- Modify: `MatterSurfaceLib/tests/mesh_simplifier_tests.cpp` (add 5 tests + helpers)

- [ ] **Step 1: Write the new failing tests first**

In `MatterSurfaceLib/tests/mesh_simplifier_tests.cpp`, add these helpers and tests **above** `main()`:

```cpp
// Count distinct vertex positions in a mesh that lie on x == plane_x (within eps).
static std::vector<std::array<float,3>> boundaryVertsOnX(const Mesh& m, float plane_x, float eps) {
    std::vector<std::array<float,3>> out;
    for (int i = 0; i < m.vertexCount; ++i) {
        float x = m.vertices[i*3+0], y = m.vertices[i*3+1], z = m.vertices[i*3+2];
        if (fabsf(x - plane_x) < eps) out.push_back({x, y, z});
    }
    return out;
}

static bool containsPos(const std::vector<std::array<float,3>>& s, float x, float y, float z, float eps) {
    for (const auto& p : s)
        if (fabsf(p[0]-x) < eps && fabsf(p[1]-y) < eps && fabsf(p[2]-z) < eps) return true;
    return false;
}

static void test_triangle_reduction() {
    printf("=== test_triangle_reduction ===\n");
    Mesh in = makeGrid(8, 4.0f); // 81 verts, 128 tris
    SimplifyOptions o; o.target_ratio = 0.5f; o.lock_boundary = false;
    Mesh out = simplify_mesh(in, o);
    printf("  in tris=%d out tris=%d\n", in.triangleCount, out.triangleCount);
    assert(out.triangleCount <= (int)(0.5f * in.triangleCount));
    assert(out.triangleCount > 0);
    UnloadMesh(in); UnloadMesh(out);
    printf("PASSED\n");
}

static void test_determinism() {
    printf("=== test_determinism ===\n");
    Mesh in = makeGrid(8, 4.0f);
    SimplifyOptions o; o.target_ratio = 0.4f; o.lock_boundary = false;
    Mesh a = simplify_mesh(in, o);
    Mesh b = simplify_mesh(in, o);
    assert(a.vertexCount == b.vertexCount);
    assert(a.triangleCount == b.triangleCount);
    for (int i = 0; i < a.vertexCount*3; ++i) assert(a.vertices[i] == b.vertices[i]);
    for (int i = 0; i < a.triangleCount*3; ++i) assert(a.indices[i] == b.indices[i]);
    UnloadMesh(in); UnloadMesh(a); UnloadMesh(b);
    printf("PASSED\n");
}

static void test_no_degenerate_triangles() {
    printf("=== test_no_degenerate_triangles ===\n");
    Mesh in = makeGrid(8, 4.0f);
    SimplifyOptions o; o.target_ratio = 0.3f; o.lock_boundary = false;
    Mesh out = simplify_mesh(in, o);
    // Every output triangle must have non-trivial area, and (this is a flat
    // z=0 grid) its normal must stay aligned with +z -- no flips.
    for (int t = 0; t < out.triangleCount; ++t) {
        int a = out.indices[t*3+0], b = out.indices[t*3+1], c = out.indices[t*3+2];
        float ax=out.vertices[a*3+0], ay=out.vertices[a*3+1];
        float bx=out.vertices[b*3+0], by=out.vertices[b*3+1];
        float cx=out.vertices[c*3+0], cy=out.vertices[c*3+1];
        float area2 = (bx-ax)*(cy-ay) - (by-ay)*(cx-ax); // 2*signed area, z component
        assert(fabsf(area2) > 1e-5f);   // non-degenerate
        assert(area2 > 0.0f);           // winding preserved (no flip)
    }
    UnloadMesh(in); UnloadMesh(out);
    printf("PASSED\n");
}

static void test_boundary_preserved() {
    printf("=== test_boundary_preserved ===\n");
    Mesh in = makeGrid(8, 4.0f); // grid spans [0,4]x[0,4] on z=0
    CellBounds cb; cb.min_bound = {0,0,-1}; cb.max_bound = {4,4,1};
    SimplifyOptions o; o.target_ratio = 0.3f; o.lock_boundary = true;
    Mesh out = simplify_mesh(in, o, &cb);
    // The x==0 edge of the grid has 9 boundary verts at y = 0,0.5,...,4.
    auto bv = boundaryVertsOnX(out, 0.0f, 1e-4f);
    for (int j = 0; j <= 8; ++j) {
        float y = 4.0f * (float)j / 8.0f;
        assert(containsPos(bv, 0.0f, y, 0.0f, 1e-3f));
    }
    UnloadMesh(in); UnloadMesh(out);
    printf("PASSED\n");
}

static void test_watertight_seam() {
    printf("=== test_watertight_seam ===\n");
    // Two grids sharing the plane x == 0. Cell A spans x in [-2,0], cell B x in [0,2].
    // Both use the SAME y samples on the shared face, so identical boundary verts.
    auto makeShiftedGrid = [](float x0, float x1) {
        std::vector<float> v; std::vector<unsigned short> idx;
        int n = 8, side = n + 1;
        for (int j = 0; j < side; ++j)
            for (int i = 0; i < side; ++i) {
                v.push_back(x0 + (x1 - x0) * (float)i / (float)n);
                v.push_back(4.0f * (float)j / (float)n);
                v.push_back(0.0f);
            }
        auto vid = [&](int i, int j) { return (unsigned short)(j*side + i); };
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                idx.push_back(vid(i,j));   idx.push_back(vid(i+1,j));   idx.push_back(vid(i+1,j+1));
                idx.push_back(vid(i,j));   idx.push_back(vid(i+1,j+1)); idx.push_back(vid(i,j+1));
            }
        return makeMesh(v, idx);
    };
    Mesh A = makeShiftedGrid(-2.0f, 0.0f); // shared face is its x==0 (right) edge
    Mesh B = makeShiftedGrid(0.0f, 2.0f);  // shared face is its x==0 (left) edge
    CellBounds cbA; cbA.min_bound = {-2,0,-1}; cbA.max_bound = {0,4,1};
    CellBounds cbB; cbB.min_bound = {0,0,-1};  cbB.max_bound = {2,4,1};
    SimplifyOptions o; o.target_ratio = 0.3f; o.lock_boundary = true;
    Mesh sA = simplify_mesh(A, o, &cbA);
    Mesh sB = simplify_mesh(B, o, &cbB);
    auto bvA = boundaryVertsOnX(sA, 0.0f, 1e-4f);
    auto bvB = boundaryVertsOnX(sB, 0.0f, 1e-4f);
    // Every shared-face vertex must appear in BOTH simplified meshes at the
    // same position -> no crack along the seam.
    for (int j = 0; j <= 8; ++j) {
        float y = 4.0f * (float)j / 8.0f;
        assert(containsPos(bvA, 0.0f, y, 0.0f, 1e-3f));
        assert(containsPos(bvB, 0.0f, y, 0.0f, 1e-3f));
    }
    UnloadMesh(A); UnloadMesh(B); UnloadMesh(sA); UnloadMesh(sB);
    printf("PASSED\n");
}
```

Add the includes `#include <array>` near the top of the test file (after `#include <vector>`).

Then add the new calls into `main()` before the final success print:

```cpp
    test_triangle_reduction();
    test_determinism();
    test_no_degenerate_triangles();
    test_boundary_preserved();
    test_watertight_seam();
```

- [ ] **Step 2: Run the tests to confirm they fail**

Run: `make -C MatterSurfaceLib/tests mesh_simplifier_tests && ./MatterSurfaceLib/tests/mesh_simplifier_tests`
Expected: FAIL at `test_triangle_reduction` (assertion on `out.triangleCount <= ...`), because the `decimate` stub returns the mesh unchanged.

- [ ] **Step 3: Add `HeapEdge` + `buildEdge` above `decimate`**

In `MatterSurfaceLib/src/mesh_simplifier.cpp`, add `#include <queue>` to the includes. Then, inside the anonymous namespace, **immediately before** the `decimate` function, insert:

```cpp
// A candidate edge collapse. `vi` is the survivor (kept) vertex, `vj` is
// merged into it at `target`. Version stamps detect stale heap entries.
struct HeapEdge {
    double cost;
    int vi, vj;
    V3 target;
    int veri, verj;
    // std::priority_queue is a max-heap; invert so lowest cost pops first.
    // Full ordering on (cost, vi, vj) makes results deterministic.
    bool operator<(const HeapEdge& o) const {
        if (cost != o.cost) return cost > o.cost;
        if (vi != o.vi)     return vi > o.vi;
        return vj > o.vj;
    }
};

// Build a collapse candidate for edge {p,q}. Returns false if the edge must
// not collapse (both endpoints boundary-locked). When exactly one endpoint is
// locked, that endpoint is the survivor and the target is its (frozen) position.
static bool buildEdge(int p, int q, const std::vector<WVert>& verts, HeapEdge& e) {
    const WVert& vp = verts[p];
    const WVert& vq = verts[q];
    if (vp.locked && vq.locked) return false;

    Quadric Q = vp.q; Q.add(vq.q);
    int survivor, removed;
    V3 target;
    if (vp.locked) {
        survivor = p; removed = q; target = vp.pos;
    } else if (vq.locked) {
        survivor = q; removed = p; target = vq.pos;
    } else {
        V3 opt;
        if (!Q.optimal(opt)) {
            V3 mid = {(vp.pos.x+vq.pos.x)*0.5, (vp.pos.y+vq.pos.y)*0.5, (vp.pos.z+vq.pos.z)*0.5};
            double ep = Q.error(vp.pos), eq = Q.error(vq.pos), em = Q.error(mid);
            opt = (ep <= eq && ep <= em) ? vp.pos : ((eq <= em) ? vq.pos : mid);
        }
        target = opt;
        survivor = (p < q) ? p : q;
        removed  = (p < q) ? q : p;
    }
    e.cost   = Q.error(target);
    e.vi     = survivor;
    e.vj     = removed;
    e.target = target;
    e.veri   = verts[survivor].version;
    e.verj   = verts[removed].version;
    return true;
}
```

- [ ] **Step 4: Replace the `decimate` stub with the engine**

Replace the entire `decimate` stub function with:

```cpp
static void decimate(std::vector<WVert>& verts, std::vector<WTri>& tris,
                     const SimplifyOptions& opts, const CellBounds* bounds, int inputTri) {
    int targetTri = (int)std::floor((double)opts.target_ratio * (double)inputTri);
    if (targetTri < 1) targetTri = 1;

    int curTri = 0;
    for (const auto& t : tris) if (!t.removed) ++curTri;
    if (curTri <= targetTri) return;

    // 1. Hard-lock vertices on any of the 6 cell face planes.
    if (bounds && opts.lock_boundary) {
        const double eps = 1e-4;
        V3 mn = {bounds->min_bound.x, bounds->min_bound.y, bounds->min_bound.z};
        V3 mx = {bounds->max_bound.x, bounds->max_bound.y, bounds->max_bound.z};
        for (auto& v : verts) {
            if (std::fabs(v.pos.x - mn.x) < eps || std::fabs(v.pos.x - mx.x) < eps ||
                std::fabs(v.pos.y - mn.y) < eps || std::fabs(v.pos.y - mx.y) < eps ||
                std::fabs(v.pos.z - mn.z) < eps || std::fabs(v.pos.z - mx.z) < eps)
                v.locked = true;
        }
    }

    // 2. Per-vertex quadrics from incident triangle planes.
    for (auto& v : verts) v.q = Quadric();
    for (const auto& t : tris) {
        if (t.removed) continue;
        V3 a = verts[t.v[0]].pos, b = verts[t.v[1]].pos, c = verts[t.v[2]].pos;
        V3 n = cross(sub(b, a), sub(c, a));
        double l = len(n);
        if (l < 1e-12) continue;
        n.x /= l; n.y /= l; n.z /= l;
        double d = -(n.x*a.x + n.y*a.y + n.z*a.z);
        Quadric q; q.addPlane(n, d);
        verts[t.v[0]].q.add(q);
        verts[t.v[1]].q.add(q);
        verts[t.v[2]].q.add(q);
    }

    // 3. Vertex -> incident triangles adjacency.
    std::vector<std::vector<int>> vtris(verts.size());
    for (int t = 0; t < (int)tris.size(); ++t) {
        if (tris[t].removed) continue;
        for (int k = 0; k < 3; ++k) vtris[tris[t].v[k]].push_back(t);
    }

    // 4. Seed the edge heap.
    std::priority_queue<HeapEdge> heap;
    auto pushTri = [&](int t) {
        int a = tris[t].v[0], b = tris[t].v[1], c = tris[t].v[2];
        int pr[3][2] = {{a,b}, {b,c}, {c,a}};
        for (auto& pe : pr) {
            HeapEdge he;
            if (buildEdge(pe[0], pe[1], verts, he)) heap.push(he);
        }
    };
    for (int t = 0; t < (int)tris.size(); ++t)
        if (!tris[t].removed) pushTri(t);

    // Reject collapses that would flip or degenerate any affected triangle.
    auto wouldFlip = [&](int s, int r, V3 tgt) -> bool {
        auto check = [&](int vv) -> bool {
            for (int t : vtris[vv]) {
                if (tris[t].removed) continue;
                int id[3] = {tris[t].v[0], tris[t].v[1], tris[t].v[2]};
                bool hasS = false, hasR = false;
                for (int k = 0; k < 3; ++k) { if (id[k]==s) hasS = true; if (id[k]==r) hasR = true; }
                if (hasS && hasR) continue; // triangle collapses away
                V3 oldp[3], newp[3];
                for (int k = 0; k < 3; ++k) {
                    oldp[k] = verts[id[k]].pos;
                    newp[k] = (id[k]==r || id[k]==s) ? tgt : verts[id[k]].pos;
                }
                V3 no = cross(sub(oldp[1], oldp[0]), sub(oldp[2], oldp[0]));
                V3 nn = cross(sub(newp[1], newp[0]), sub(newp[2], newp[0]));
                if (len(nn) < 1e-12) return true;     // degenerate
                if (dot(no, nn) < 0)  return true;     // flipped
            }
            return false;
        };
        return check(s) || check(r);
    };

    // 5. Greedy collapse loop.
    while (curTri > targetTri && !heap.empty()) {
        HeapEdge e = heap.top(); heap.pop();
        if (verts[e.vi].removed || verts[e.vj].removed) continue;
        if (verts[e.vi].version != e.veri || verts[e.vj].version != e.verj) continue; // stale
        if (e.cost > opts.max_error) break;
        if (wouldFlip(e.vi, e.vj, e.target)) continue;

        if (!verts[e.vi].locked) verts[e.vi].pos = e.target;
        verts[e.vi].q.add(verts[e.vj].q);
        verts[e.vj].removed = true;

        for (int t : vtris[e.vj]) {
            if (tris[t].removed) continue;
            for (int k = 0; k < 3; ++k) if (tris[t].v[k] == e.vj) tris[t].v[k] = e.vi;
            if (tris[t].v[0] == tris[t].v[1] || tris[t].v[1] == tris[t].v[2] || tris[t].v[0] == tris[t].v[2]) {
                tris[t].removed = true;
                --curTri;
            } else {
                vtris[e.vi].push_back(t);
            }
        }
        verts[e.vi].version++;
        verts[e.vj].version++;

        for (int t : vtris[e.vi])
            if (!tris[t].removed) pushTri(t);
    }
}
```

- [ ] **Step 5: Run the tests — verify all pass**

Run: `make -C MatterSurfaceLib/tests mesh_simplifier_tests && ./MatterSurfaceLib/tests/mesh_simplifier_tests`
Expected: PASS — output ends with `All mesh simplifier scaffold tests PASSED` and every `=== test_* ===` block prints `PASSED`, including reduction, determinism, no-degenerate, boundary-preserved, and watertight-seam.

- [ ] **Step 6: Commit**

```bash
git add MatterSurfaceLib/src/mesh_simplifier.cpp \
        MatterSurfaceLib/tests/mesh_simplifier_tests.cpp
git commit -m "feat: QEM edge-collapse decimation with boundary locking"
```

---

## Task 3: Integrate into Cell/Cluster, add UI slider, wire into builds

Thread a simplification ratio from `Cluster` into `Cell::generate_mesh_for_material`, apply `simplify_mesh` there, expose a UI slider, add the new source to the app build, and run the new test from `build-all.sh test`.

**Files:**
- Modify: `MatterSurfaceLib/include/cell.h`
- Modify: `MatterSurfaceLib/src/cell.cpp`
- Modify: `MatterSurfaceLib/include/cluster.h`
- Modify: `MatterSurfaceLib/src/cluster.cpp`
- Modify: `MatterSurfaceLib/main.cpp`
- Modify: `MatterSurfaceLib/Makefile`
- Modify: `build-all.sh`

- [ ] **Step 1: Add the ratio parameter to Cell declarations**

In `MatterSurfaceLib/include/cell.h`, change the `rebuild_meshes` declaration (line 40) to:

```cpp
    void rebuild_meshes(const std::vector<StaticParticle>& cluster_particles, BLASManager& blas_manager, float simplification_ratio = 1.0f);
```

and the private `generate_mesh_for_material` declaration (line 63) to:

```cpp
    void generate_mesh_for_material(uint32_t material_id, const std::vector<StaticParticle>& cluster_particles, BLASManager& blas_manager, float simplification_ratio);
```

- [ ] **Step 2: Apply simplification in cell.cpp**

In `MatterSurfaceLib/src/cell.cpp`, add the include near the top (with the other project includes):

```cpp
#include "mesh_simplifier.hpp"
```

Change `rebuild_meshes` (line 124) signature and its call to the per-material helper:

```cpp
void Cell::rebuild_meshes(const std::vector<StaticParticle>& cluster_particles, BLASManager& blas_manager, float simplification_ratio) {
    clear_meshes();

    if (material_particle_indices.empty()) {
        return;
    }

    // Generate a mesh for each material
    for (const auto& material_entry : material_particle_indices) {
        uint32_t material_id = material_entry.first;
        generate_mesh_for_material(material_id, cluster_particles, blas_manager, simplification_ratio);
    }

    has_meshes = !material_meshes.empty();
}
```

Change the `generate_mesh_for_material` definition signature (line 234) to add the parameter:

```cpp
void Cell::generate_mesh_for_material(uint32_t material_id, const std::vector<StaticParticle>& cluster_particles, BLASManager& blas_manager, float simplification_ratio) {
```

Then, immediately after the `Mesh mesh = GenerateMesh(...)` call and its `printf` (right after line 283, before the `if (mesh.vertexCount > 0)` block at line 285), insert:

```cpp
    // Decimate to a low-poly proxy when the cluster requests it. Boundary
    // vertices on this cell's face planes are locked so seams with same-level
    // neighbor cells stay watertight.
    if (simplification_ratio < 1.0f && mesh.vertexCount > 0 && mesh.triangleCount > 0) {
        CellBounds cb;
        cb.min_bound = min_bound;
        cb.max_bound = max_bound;
        SimplifyOptions so;
        so.target_ratio = simplification_ratio;
        so.lock_boundary = true;
        Mesh simplified = simplify_mesh(mesh, so, &cb);
        if (simplified.vertexCount > 0 && simplified.triangleCount > 0) {
            UnloadMesh(mesh);   // free the pre-upload CPU arrays of the dense mesh
            mesh = simplified;
            printf("    Simplified mesh: %d vertices, %d triangles (ratio %.2f)\n",
                   mesh.vertexCount, mesh.triangleCount, simplification_ratio);
        } else {
            UnloadMesh(simplified); // simplification produced nothing usable; keep dense mesh
        }
    }
```

- [ ] **Step 3: Add the ratio to Cluster**

In `MatterSurfaceLib/include/cluster.h`, in the `public:` section after the LOD block (after line 75, the `force_rebuild_all_cells();` line), add:

```cpp
    // Mesh simplification (uniform across cells; per-cell distance LOD can drive
    // this later without changing the simplifier).
    void set_simplification_ratio(float ratio) {
        if (ratio < 0.05f) ratio = 0.05f;
        if (ratio > 1.0f)  ratio = 1.0f;
        simplification_ratio_ = ratio;
    }
    float get_simplification_ratio() const { return simplification_ratio_; }
```

In the `private:` members, after `int current_lod_level_;` (line 97), add:

```cpp
    float simplification_ratio_ = 1.0f; // 1.0 = no simplification
```

- [ ] **Step 4: Pass the ratio through in cluster.cpp**

In `MatterSurfaceLib/src/cluster.cpp`, change the `rebuild_meshes` call in `update_cell_meshes` (line 256) to:

```cpp
        cell->rebuild_meshes(particles_, blas_manager_, simplification_ratio_);
```

- [ ] **Step 5: Build the app to confirm integration compiles**

First add the new source to the app build. In `MatterSurfaceLib/Makefile`:

In the `SRC =` line (line 129), add `src/mesh_simplifier.cpp` right after `src/cell.cpp`:

```makefile
SRC = main.cpp src/bvh.cpp src/object_allocator.c src/blas_manager.cpp src/tlas_manager.cpp src/bvh_visualizer.cpp src/bvh_analyzer.cpp src/open_particle_surface.c src/surface.c src/spatial_hash.c src/cluster.cpp src/cell.cpp src/mesh_simplifier.cpp $(IMGUI_PATH)/imgui.cpp $(IMGUI_PATH)/imgui_demo.cpp $(IMGUI_PATH)/imgui_draw.cpp $(IMGUI_PATH)/imgui_tables.cpp $(IMGUI_PATH)/imgui_widgets.cpp $(IMGUI_PATH)/backends/imgui_impl_opengl3.cpp $(IMGUI_PATH)/backends/imgui_impl_glfw.cpp
```

In the `OBJ =` line (line 130), add `$(OBJ_DIR)/mesh_simplifier.o` right after `$(OBJ_DIR)/cell.o`:

```makefile
OBJ = $(OBJ_DIR)/main.o $(OBJ_DIR)/bvh.o $(OBJ_DIR)/object_allocator.o $(OBJ_DIR)/blas_manager.o $(OBJ_DIR)/tlas_manager.o $(OBJ_DIR)/bvh_visualizer.o $(OBJ_DIR)/bvh_analyzer.o $(OBJ_DIR)/open_particle_surface.o $(OBJ_DIR)/surface.o $(OBJ_DIR)/spatial_hash.o $(OBJ_DIR)/cluster.o $(OBJ_DIR)/cell.o $(OBJ_DIR)/mesh_simplifier.o $(OBJ_DIR)/imgui.o $(OBJ_DIR)/imgui_demo.o $(OBJ_DIR)/imgui_draw.o $(OBJ_DIR)/imgui_tables.o $(OBJ_DIR)/imgui_widgets.o $(OBJ_DIR)/imgui_impl_opengl3.o $(OBJ_DIR)/imgui_impl_glfw.o
```

Add a compile rule after the `cell.o` rule (after line 256):

```makefile
$(OBJ_DIR)/mesh_simplifier.o: src/mesh_simplifier.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@
```

Run: `make -C MatterSurfaceLib WSL_LINUX=1`
Expected: links successfully, prints `✓ Copied to ./matter_surface_lib`.

- [ ] **Step 6: Add the UI slider**

In `MatterSurfaceLib/main.cpp`, in `render_ui()` right after the Render Mode `ImGui::Separator();` (line 736, before the `// Camera controls` block at line 738), insert:

```cpp
        // Mesh simplification ratio: 1.0 = full detail, lower = cheaper proxy.
        // Rebuilds all cells through the simplifier when changed.
        {
            float ratio = test_cluster_->get_simplification_ratio();
            if (ImGui::SliderFloat("Simplification", &ratio, 0.05f, 1.0f, "%.2f")) {
                test_cluster_->set_simplification_ratio(ratio);
                test_cluster_->force_rebuild_all_cells();
                test_cluster_->rebuild_dirty_cells();
            }
        }

        ImGui::Separator();
```

- [ ] **Step 7: Rebuild the app**

Run: `make -C MatterSurfaceLib WSL_LINUX=1`
Expected: compiles and links cleanly; `✓ Copied to ./matter_surface_lib`.

- [ ] **Step 8: Wire the unit test into build-all.sh**

In `build-all.sh`, inside the `if [ "$MODE" = "test" ]; then` block, after the existing `for proj in ObjectAllocatorLib SpatialQueryLib; do ... done` loop (after line 111, before the closing `fi` on line 112), add:

```bash
    # MatterSurfaceLib mesh simplifier unit tests (headless, no GL window)
    if make -C MatterSurfaceLib/tests mesh_simplifier_tests >/dev/null 2>&1; then
        echo
        echo "--- MatterSurfaceLib (mesh_simplifier) ---"
        MatterSurfaceLib/tests/mesh_simplifier_tests || RESULT[MatterSurfaceLib]="FAIL (tests)"
    else
        RESULT[MatterSurfaceLib]="FAIL (test build)"
    fi
```

- [ ] **Step 9: Full regression — build everything and run tests**

Run: `./build-all.sh test`
Expected: all projects report `OK` in the Summary, and the `--- MatterSurfaceLib (mesh_simplifier) ---` section prints all tests `PASSED`.

- [ ] **Step 10: Commit**

```bash
git add MatterSurfaceLib/include/cell.h MatterSurfaceLib/src/cell.cpp \
        MatterSurfaceLib/include/cluster.h MatterSurfaceLib/src/cluster.cpp \
        MatterSurfaceLib/main.cpp MatterSurfaceLib/Makefile build-all.sh
git commit -m "feat: wire mesh simplification into Cell/Cluster + UI + builds"
```

---

## Self-Review Notes

**Spec coverage:**
- QEM edge-collapse engine → Task 2 (quadrics, heap, greedy collapse).
- Boundary locking + watertight guarantee → Task 2 Step 4 (lock pass) + `test_boundary_preserved`/`test_watertight_seam`.
- Geometric-only, recomputed smooth normals → Task 1 `buildMesh`.
- Triangle-flip prevention → Task 2 `wouldFlip`.
- Integration hook (uniform ratio + UI slider, distance-LOD-ready) → Task 3.
- Error/edge cases (empty, single triangle, non-indexed weld, singular solve) → Task 1 trivial handling + `buildTopology` weld + `Quadric::optimal` fallback; tested in Task 1.
- All 7 spec test cases present (reduction, identity, boundary preservation, no flips/degenerates, determinism, degenerate input, watertight seam).
- Wired into `tests/Makefile` and `build-all.sh test` → Tasks 1 & 3.

**Type consistency:** `simplify_mesh(const Mesh&, const SimplifyOptions&, const CellBounds*)`, `SimplifyOptions{target_ratio,max_error,lock_boundary}`, `CellBounds{min_bound,max_bound}`, and `decimate(verts,tris,opts,bounds,inputTri)` are used identically across the header, the .cpp, and the integration call sites. `Cell::rebuild_meshes(..., float)` and `generate_mesh_for_material(..., float)` match between `cell.h` and `cell.cpp`. `Cluster::set_simplification_ratio`/`get_simplification_ratio` match the `main.cpp` calls.

**No placeholders:** every code step contains complete, compilable code.
