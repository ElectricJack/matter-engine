# Voxel Box Imposter Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the chart-based relief-cage imposter with a dense per-type voxel volume rendered by a 3D-DDA march, so non-convex/porous parts (trees, shrubs) render correctly while preserving instancing.

**Architecture:** Each part type bakes once (CPU, GL-free) into a dense `Nx×Ny×Nz` voxel grid storing coverage + RGB albedo + octahedral normal, sized to the part's local AABB. At runtime the proxy is a shared unit-cube BLAS; each instance is a TLAS entry whose transform maps `[0,1]³` to the part's world OBB. The traversal shader transforms the ray into local box space, runs a 3D-DDA voxel walk, and on the first covered voxel emits a standard `HitResult` that flows through the existing lighting path.

**Tech Stack:** C++17, raylib/rlgl + OpenGL 3.3 (raw `glTexImage3D` for 3D textures), GLSL fragment-shader ray tracer (`bvh_tlas_common.glsl`), plain-`main()` CHECK-macro unit tests under `tests/`.

**Reference spec:** `docs/superpowers/specs/2026-06-22-voxel-box-imposter-design.md`

---

## Pre-flight: revert experimental chart-cage edits, branch fresh

The working tree has uncommitted experimental relief-march edits (now dead-ended by the pivot). Discard them and branch before starting.

- [ ] **Step 1: Confirm what is dirty**

Run: `git status --short && git stash list`
Expected: modified `MatterSurfaceLib/main.cpp`, `shaders/bvh_tlas_common.glsl`, `shaders/raytrace_tlas_blas.fs`, `shaders/raytrace_tlas_blas_processed.fs`; untracked PNGs + `.claude/`.

- [ ] **Step 2: Discard the experimental tracked edits** (they are superseded; the seed-orientation fix is in already-committed history and will be deleted wholesale in Task 15 anyway)

Run:
```bash
git restore MatterSurfaceLib/main.cpp \
  MatterSurfaceLib/shaders/bvh_tlas_common.glsl \
  MatterSurfaceLib/shaders/raytrace_tlas_blas.fs \
  MatterSurfaceLib/shaders/raytrace_tlas_blas_processed.fs
```
Expected: `git status --short` shows those four no longer modified.

- [ ] **Step 3: Create the feature branch**

Run: `git checkout -b feat/voxel-box-imposter`
Expected: `Switched to a new branch 'feat/voxel-box-imposter'`

- [ ] **Step 4: Baseline build sanity** (Linux headless target builds before any change)

Run: `cd MatterSurfaceLib && make WSL_LINUX=1 shaders && make WSL_LINUX=1 -j4 2>&1 | tail -5`
Expected: links `matter_surface_lib` with no errors.

---

## File Structure

**New sibling library (salvage):**
- `MeshChartingLib/include/mesh_charting.h` — `build_adjacency`, `segment_charts`, `plane_basis`, `pack_charts` + their structs, in namespace `mesh_charting`.
- `MeshChartingLib/src/mesh_charting.cpp` — implementations (copied from `imposter_asset.cpp`).
- `MeshChartingLib/tests/mesh_charting_tests.cpp` + `MeshChartingLib/tests/Makefile`.
- `MeshChartingLib/Makefile`, `MeshChartingLib/README.md`.

**New voxel imposter (MatterSurfaceLib):**
- `MatterSurfaceLib/include/voxel_imposter.h` — `VoxGenParams`, `VoxelImposter`, bake/serialize/DDA/octahedral declarations.
- `MatterSurfaceLib/src/voxel_imposter.cpp` — all GL-free CPU logic.
- `MatterSurfaceLib/tests/voxel_imposter_tests.cpp` — unit tests.

**Modified:**
- `MatterSurfaceLib/include/blas_manager.hpp` / `src/blas_manager.cpp` — retain `TriEx` per BLAS entry.
- `MatterSurfaceLib/src/imposter_bake.cpp` (`flatten_part_triangles`) — material-aware flatten variant (then deleted in T15 only if fully replaced; we keep this file until T15).
- `MatterSurfaceLib/main.cpp` — unit-cube BLAS, 3D texture upload + binding, demo wiring.
- `MatterSurfaceLib/shaders/bvh_tlas_common.glsl` — 3D-DDA march replacing relief march.
- `MatterSurfaceLib/Makefile`, `MatterSurfaceLib/tests/Makefile`, `build-all.sh`.

**Deleted (T15):** chart-cage code in `imposter_asset.{h,cpp}`, `imposter_bake.cpp` GPU radiance bake, relief-march shader code + 2D imposter samplers.

---

## Task 1: Create MeshChartingLib (salvage extraction)

**Files:**
- Create: `MeshChartingLib/include/mesh_charting.h`
- Create: `MeshChartingLib/src/mesh_charting.cpp`
- Create: `MeshChartingLib/tests/mesh_charting_tests.cpp`
- Create: `MeshChartingLib/tests/Makefile`
- Create: `MeshChartingLib/Makefile`
- Modify: `build-all.sh` (add the new test suite)

Extraction is a **copy** into a new `mesh_charting` namespace; the originals in `imposter_asset` stay compiling until Task 15 deletes them with the rest of the chart-cage path. Different namespaces → no ODR clash.

- [ ] **Step 1: Write the library header**

Create `MeshChartingLib/include/mesh_charting.h`:
```cpp
#pragma once
#include <vector>

// Reusable mesh-charting / UV-atlas-packing utilities, salvaged from the
// chart-based imposter cage. GL-free and unit-tested. See
// docs/superpowers/specs/2026-06-22-voxel-box-imposter-design.md
namespace mesh_charting {

// Per-triangle neighbor across edge slots (i0,i1)=0, (i1,i2)=1, (i2,i0)=2; -1 = boundary.
struct TriAdj { int nbr[3]; };

// Build triangle adjacency. Vertices are welded by EXACT position first.
std::vector<TriAdj> build_adjacency(const float* positions, const unsigned short* indices,
                                    int triCount);

// Region-grow charts by normal-cone (coneDeg must be < 90). Returns per-triangle chart id.
std::vector<int> segment_charts(const float* positions, const unsigned short* indices,
                                int triCount, const std::vector<TriAdj>& adj,
                                float coneDeg, int& nCharts);

// Orthonormal basis (T,B) spanning the plane with normal n.
void plane_basis(const float n[3], float T[3], float B[3]);

struct ChartRect  { float minU, minV, w, h; };
struct ChartPlacement { int ox, oy; };

// Shelf-pack chart rects into an atlasW x atlasH grid with `pad` gutter texels.
bool pack_charts(const std::vector<ChartRect>& charts, int atlasW, int atlasH, int pad,
                 float& scale, std::vector<ChartPlacement>& placements);

} // namespace mesh_charting
```

- [ ] **Step 2: Write the library source**

Create `MeshChartingLib/src/mesh_charting.cpp` by copying the bodies of `build_adjacency`, `segment_charts`, `plane_basis`, `shelf_pack`, `pack_charts` from `MatterSurfaceLib/src/imposter_asset.cpp` (lines 165-301), plus the local `v3/sub3/cross3/norm3` helpers (lines 165-168). Wrap them in `namespace mesh_charting`, replace `ChartRect`/`ChartPlacement`/`TriAdj` references (now from the new header), and add at top:
```cpp
#include "../include/mesh_charting.h"
#include <map>
#include <array>
#include <vector>
#include <algorithm>
#include <cmath>

namespace mesh_charting {
namespace {
struct float3c { float x,y,z; };
static float3c v3(float x,float y,float z){ return {x,y,z}; }
static float3c sub3(float3c a,float3c b){ return {a.x-b.x,a.y-b.y,a.z-b.z}; }
static float3c cross3(float3c a,float3c b){ return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
static float3c norm3(float3c a){ float l=std::sqrt(a.x*a.x+a.y*a.y+a.z*a.z); return l>1e-12f?float3c{a.x/l,a.y/l,a.z/l}:float3c{0,0,0}; }
} // namespace
```
Then port the four functions verbatim, substituting `float3c`/`v3` for `make_float3`/`float3`. (The originals use raylib `make_float3`; the standalone lib must not depend on raylib, so use the local `float3c`.)

- [ ] **Step 3: Write the library Makefile**

Create `MeshChartingLib/Makefile`:
```makefile
# Standalone mesh-charting utilities. Header-light, raylib-free.
CXX ?= g++
CXXFLAGS = -Wall -Wextra -O2 -std=c++17 -Iinclude

lib: src/mesh_charting.cpp
	$(CXX) $(CXXFLAGS) -c src/mesh_charting.cpp -o mesh_charting.o

test:
	$(MAKE) -C tests run

clean:
	rm -f mesh_charting.o tests/mesh_charting_tests
```

- [ ] **Step 4: Write the test Makefile**

Create `MeshChartingLib/tests/Makefile`:
```makefile
CXX ?= g++
CXXFLAGS = -Wall -Wextra -O2 -std=c++17 -I../include

mesh_charting_tests: mesh_charting_tests.cpp ../src/mesh_charting.cpp
	$(CXX) $(CXXFLAGS) $^ -o $@

run: mesh_charting_tests
	./mesh_charting_tests
```

- [ ] **Step 5: Write the failing tests**

Create `MeshChartingLib/tests/mesh_charting_tests.cpp`:
```cpp
#include "../include/mesh_charting.h"
#include <cstdio>
#include <cmath>
using namespace mesh_charting;
static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)

// Two triangles sharing an edge form a quad; they must be mutual neighbors.
static void test_adjacency_quad() {
    float pos[] = {0,0,0, 1,0,0, 1,1,0, 0,1,0};
    unsigned short idx[] = {0,1,2, 0,2,3}; // shared edge (0,2)
    auto adj = build_adjacency(pos, idx, 2);
    bool linked = (adj[0].nbr[0]==1||adj[0].nbr[1]==1||adj[0].nbr[2]==1) &&
                  (adj[1].nbr[0]==0||adj[1].nbr[1]==0||adj[1].nbr[2]==0);
    CHECK(linked, "quad triangles are mutual neighbors");
}

// A coplanar quad is one chart at a 30-degree cone.
static void test_segment_one_chart() {
    float pos[] = {0,0,0, 1,0,0, 1,1,0, 0,1,0};
    unsigned short idx[] = {0,1,2, 0,2,3};
    auto adj = build_adjacency(pos, idx, 2);
    int n=0; auto cid = segment_charts(pos, idx, 2, adj, 30.0f, n);
    CHECK(n==1, "coplanar quad is a single chart");
    CHECK(cid[0]==cid[1], "both tris in same chart");
}

static void test_plane_basis_orthonormal() {
    float n[3]={0,0,1}, T[3],B[3]; plane_basis(n,T,B);
    float dotTB=T[0]*B[0]+T[1]*B[1]+T[2]*B[2];
    float dotTN=T[0]*n[0]+T[1]*n[1]+T[2]*n[2];
    CHECK(std::fabs(dotTB)<1e-5f, "T perp B");
    CHECK(std::fabs(dotTN)<1e-5f, "T perp N");
}

static void test_pack_fits() {
    std::vector<ChartRect> r = {{0,0,1,1},{0,0,1,1}};
    float scale=1; std::vector<ChartPlacement> pl;
    CHECK(pack_charts(r, 64, 64, 2, scale, pl), "two unit charts pack into 64x64");
    CHECK(pl.size()==2, "placement per chart");
}

int main(){
    test_adjacency_quad(); test_segment_one_chart();
    test_plane_basis_orthonormal(); test_pack_fits();
    if(!failures) printf("All mesh_charting tests passed\n");
    return failures?1:0;
}
```

- [ ] **Step 6: Run tests, expect PASS** (code is ported, not new)

Run: `make -C MeshChartingLib/tests run`
Expected: `All mesh_charting tests passed`

- [ ] **Step 7: Register the suite in build-all.sh**

In `build-all.sh`, after the MatterSurfaceLib test loop (after line ~129), add:
```bash
    if make -C MeshChartingLib/tests mesh_charting_tests >/dev/null 2>&1; then
        echo; echo "--- MeshChartingLib (mesh_charting_tests) ---"
        ./MeshChartingLib/tests/mesh_charting_tests || RESULT[MeshChartingLib]="FAIL (tests)"
    else
        RESULT[MeshChartingLib]="FAIL (test build)"
    fi
```

- [ ] **Step 8: Commit**

```bash
git add MeshChartingLib build-all.sh
git commit -m "feat: salvage mesh-charting utilities into MeshChartingLib"
```

---

## Task 2: Voxel asset header + grid-dim selection

**Files:**
- Create: `MatterSurfaceLib/include/voxel_imposter.h`
- Create: `MatterSurfaceLib/src/voxel_imposter.cpp`
- Create: `MatterSurfaceLib/tests/voxel_imposter_tests.cpp`
- Modify: `MatterSurfaceLib/tests/Makefile`

- [ ] **Step 1: Write the header (asset + first declaration)**

Create `MatterSurfaceLib/include/voxel_imposter.h`:
```cpp
#pragma once
#include "bvh.h"        // Tri, float3
#include <cstdint>
#include <string>
#include <vector>

// Dense voxel-volume imposter. See
// docs/superpowers/specs/2026-06-22-voxel-box-imposter-design.md
namespace voxel_imposter {

constexpr uint32_t kMagic = 0x49584F56u;   // 'VOXI'
constexpr uint32_t kFormatVersion = 1u;

struct VoxGenParams {
    int   maxDim;       // resolution budget for the longest axis (e.g. 128)
    int   seed;         // reserved
    float coverThresh;  // surface-fill threshold in [0,1] (default 0.5)
};
static_assert(sizeof(VoxGenParams) == 12, "VoxGenParams padding-free for byte hashing");

struct VoxelImposter {
    float    bounds_min[3] = {0,0,0};
    float    bounds_max[3] = {0,0,0};
    int      nx = 0, ny = 0, nz = 0;
    uint64_t source_part_hash = 0;
    std::vector<uint8_t> coverage;  // nx*ny*nz, 0=empty 255=full
    std::vector<uint8_t> albedo;    // nx*ny*nz*3, RGB
    std::vector<uint8_t> normal;    // nx*ny*nz*2, octahedral RG8
    int voxel_index(int x,int y,int z) const { return (z*ny + y)*nx + x; }
};

// Choose per-axis grid dims so voxels stay ~isotropic in world space.
// v = maxExtent/maxDim; nx = clamp(ceil(extentX/v), 1, maxDim); etc.
// Returns false on degenerate (non-positive) extent on all axes.
bool choose_grid_dims(const float bounds_min[3], const float bounds_max[3],
                      int maxDim, int& nx, int& ny, int& nz);

} // namespace voxel_imposter
```

- [ ] **Step 2: Write failing test for grid-dim selection**

Create `MatterSurfaceLib/tests/voxel_imposter_tests.cpp`:
```cpp
#include "../include/voxel_imposter.h"
#include <cstdio>
#include <cmath>
using namespace voxel_imposter;
static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)

static void test_grid_dims_cube() {
    float lo[3]={0,0,0}, hi[3]={2,2,2}; int nx,ny,nz;
    CHECK(choose_grid_dims(lo,hi,128,nx,ny,nz), "cube dims ok");
    CHECK(nx==128 && ny==128 && nz==128, "cube -> 128^3");
}
static void test_grid_dims_flat() {
    float lo[3]={0,0,0}, hi[3]={4,4,1}; int nx,ny,nz; // z is 1/4 the extent
    CHECK(choose_grid_dims(lo,hi,128,nx,ny,nz), "flat dims ok");
    CHECK(nx==128 && ny==128 && nz==32, "flat brick -> 128x128x32");
}
static void test_grid_dims_degenerate() {
    float lo[3]={0,0,0}, hi[3]={0,0,0}; int nx,ny,nz;
    CHECK(!choose_grid_dims(lo,hi,128,nx,ny,nz), "zero extent rejected");
}

int main(){
    test_grid_dims_cube(); test_grid_dims_flat(); test_grid_dims_degenerate();
    if(!failures) printf("All voxel_imposter tests passed\n");
    return failures?1:0;
}
```

- [ ] **Step 3: Add the test target to tests/Makefile**

In `MatterSurfaceLib/tests/Makefile`, after the imposter target block (~line 253), add:
```makefile
VOX_TARGET = voxel_imposter_tests
VOX_CPP = voxel_imposter_tests.cpp ../src/voxel_imposter.cpp ../src/bvh.cpp \
          ../src/part_asset.cpp ../src/material_registry.c
$(VOX_TARGET): $(VOX_CPP)
	$(CC) $(VOX_CPP) -o $(VOX_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)

run-vox: $(VOX_TARGET)
	./$(VOX_TARGET)
```
(If `material_registry.c` must compile as C, mirror the imposter target's split `gcc -c` step instead; check whether `$(CC)` already handles the `.c` in this Makefile and adjust to match the imposter pattern at lines 240-253.)

- [ ] **Step 4: Run test, expect FAIL (link error: choose_grid_dims undefined)**

Run: `make -C MatterSurfaceLib/tests voxel_imposter_tests`
Expected: link error `undefined reference to ...choose_grid_dims`.

- [ ] **Step 5: Implement choose_grid_dims**

Create `MatterSurfaceLib/src/voxel_imposter.cpp`:
```cpp
#include "../include/voxel_imposter.h"
#include "../include/part_asset.h"   // fnv1a64 (used in later tasks)
#include <algorithm>
#include <cmath>
#include <cstring>

namespace voxel_imposter {

bool choose_grid_dims(const float lo[3], const float hi[3],
                      int maxDim, int& nx, int& ny, int& nz) {
    float ex = hi[0]-lo[0], ey = hi[1]-lo[1], ez = hi[2]-lo[2];
    float maxExtent = std::max(ex, std::max(ey, ez));
    if (maxExtent <= 0.0f || maxDim < 1) return false;
    float v = maxExtent / (float)maxDim;
    auto dim = [&](float e){ int d = (int)std::ceil(e / v); return std::max(1, std::min(maxDim, d)); };
    nx = dim(ex); ny = dim(ey); nz = dim(ez);
    return true;
}

} // namespace voxel_imposter
```

- [ ] **Step 6: Run test, expect PASS**

Run: `make -C MatterSurfaceLib/tests run-vox`
Expected: `All voxel_imposter tests passed`

- [ ] **Step 7: Commit**

```bash
git add MatterSurfaceLib/include/voxel_imposter.h MatterSurfaceLib/src/voxel_imposter.cpp \
        MatterSurfaceLib/tests/voxel_imposter_tests.cpp MatterSurfaceLib/tests/Makefile
git commit -m "feat: voxel imposter asset struct + grid-dim selection"
```

---

## Task 3: Triangle / voxel-box overlap test (SAT)

**Files:**
- Modify: `MatterSurfaceLib/include/voxel_imposter.h`
- Modify: `MatterSurfaceLib/src/voxel_imposter.cpp`
- Modify: `MatterSurfaceLib/tests/voxel_imposter_tests.cpp`

- [ ] **Step 1: Declare the overlap helper in the header** (inside the namespace, after `choose_grid_dims`)
```cpp
// Akenine-Moller triangle / axis-aligned-box overlap. boxCenter/boxHalf in the
// same space as the triangle verts. Returns true if the triangle intersects the box.
bool tri_box_overlap(const float boxCenter[3], const float boxHalf[3],
                     const float v0[3], const float v1[3], const float v2[3]);
```

- [ ] **Step 2: Write failing tests** (append to `voxel_imposter_tests.cpp`, call from main)
```cpp
static void test_tribox_hit() {
    float c[3]={0,0,0}, h[3]={1,1,1};
    float a[3]={-2,0,0}, b[3]={2,0,0}, d[3]={0,2,0}; // big tri through the box
    CHECK(tri_box_overlap(c,h,a,b,d), "triangle crossing box overlaps");
}
static void test_tribox_miss() {
    float c[3]={0,0,0}, h[3]={1,1,1};
    float a[3]={5,5,5}, b[3]={6,5,5}, d[3]={5,6,5}; // far away
    CHECK(!tri_box_overlap(c,h,a,b,d), "distant triangle does not overlap");
}
```
Add `test_tribox_hit(); test_tribox_miss();` to `main()`.

- [ ] **Step 3: Run test, expect FAIL (undefined reference)**

Run: `make -C MatterSurfaceLib/tests voxel_imposter_tests`
Expected: link error for `tri_box_overlap`.

- [ ] **Step 4: Implement tri_box_overlap** (standard Akenine-Moller 13-axis SAT) in `voxel_imposter.cpp`:
```cpp
namespace {
inline void sub(float r[3],const float a[3],const float b[3]){ r[0]=a[0]-b[0];r[1]=a[1]-b[1];r[2]=a[2]-b[2]; }
inline void cross(float r[3],const float a[3],const float b[3]){ r[0]=a[1]*b[2]-a[2]*b[1]; r[1]=a[2]*b[0]-a[0]*b[2]; r[2]=a[0]*b[1]-a[1]*b[0]; }
inline float dot(const float a[3],const float b[3]){ return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
inline bool plane_box_overlap(const float n[3],float d,const float h[3]){
    float vmin[3],vmax[3];
    for(int q=0;q<3;++q){ if(n[q]>0){vmin[q]=-h[q];vmax[q]=h[q];}else{vmin[q]=h[q];vmax[q]=-h[q];} }
    if(dot(n,vmin)+d>0) return false;
    if(dot(n,vmax)+d>=0) return true;
    return false;
}
} // namespace

bool tri_box_overlap(const float bc[3], const float bh[3],
                     const float V0[3], const float V1[3], const float V2[3]) {
    float v0[3],v1[3],v2[3]; sub(v0,V0,bc); sub(v1,V1,bc); sub(v2,V2,bc);
    float e0[3],e1[3],e2[3]; sub(e0,v1,v0); sub(e1,v2,v1); sub(e2,v0,v2);
    float fex,fey,fez,p0,p1,p2,rad,minv,maxv;
    #define AXISTEST_X(a,b,fa,fb,va,vb) \
        p0=a*va[1]-b*va[2]; p1=a*vb[1]-b*vb[2]; \
        minv=p0<p1?p0:p1; maxv=p0<p1?p1:p0; rad=fa*bh[1]+fb*bh[2]; \
        if(minv>rad||maxv<-rad) return false;
    #define AXISTEST_Y(a,b,fa,fb,va,vb) \
        p0=-a*va[0]+b*va[2]; p1=-a*vb[0]+b*vb[2]; \
        minv=p0<p1?p0:p1; maxv=p0<p1?p1:p0; rad=fa*bh[0]+fb*bh[2]; \
        if(minv>rad||maxv<-rad) return false;
    #define AXISTEST_Z(a,b,fa,fb,va,vb) \
        p0=a*va[0]-b*va[1]; p1=a*vb[0]-b*vb[1]; \
        minv=p0<p1?p0:p1; maxv=p0<p1?p1:p0; rad=fa*bh[0]+fb*bh[1]; \
        if(minv>rad||maxv<-rad) return false;
    fex=std::fabs(e0[0]);fey=std::fabs(e0[1]);fez=std::fabs(e0[2]);
    AXISTEST_X(e0[2],e0[1],fez,fey,v0,v2); AXISTEST_Y(e0[2],e0[0],fez,fex,v0,v2); AXISTEST_Z(e0[1],e0[0],fey,fex,v1,v2);
    fex=std::fabs(e1[0]);fey=std::fabs(e1[1]);fez=std::fabs(e1[2]);
    AXISTEST_X(e1[2],e1[1],fez,fey,v0,v2); AXISTEST_Y(e1[2],e1[0],fez,fex,v0,v2); AXISTEST_Z(e1[1],e1[0],fey,fex,v0,v1);
    fex=std::fabs(e2[0]);fey=std::fabs(e2[1]);fez=std::fabs(e2[2]);
    AXISTEST_X(e2[2],e2[1],fez,fey,v0,v1); AXISTEST_Y(e2[2],e2[0],fez,fex,v0,v1); AXISTEST_Z(e2[1],e2[0],fey,fex,v1,v2);
    #undef AXISTEST_X
    #undef AXISTEST_Y
    #undef AXISTEST_Z
    auto mm=[&](float a,float b,float c,float&mn,float&mx){ mn=mx=a; if(b<mn)mn=b; if(b>mx)mx=b; if(c<mn)mn=c; if(c>mx)mx=c; };
    mm(v0[0],v1[0],v2[0],minv,maxv); if(minv>bh[0]||maxv<-bh[0]) return false;
    mm(v0[1],v1[1],v2[1],minv,maxv); if(minv>bh[1]||maxv<-bh[1]) return false;
    mm(v0[2],v1[2],v2[2],minv,maxv); if(minv>bh[2]||maxv<-bh[2]) return false;
    float n[3]; cross(n,e0,e1); float d=-dot(n,v0);
    return plane_box_overlap(n,d,bh);
}
```

- [ ] **Step 5: Run test, expect PASS**

Run: `make -C MatterSurfaceLib/tests run-vox`
Expected: `All voxel_imposter tests passed`

- [ ] **Step 6: Commit**

```bash
git add MatterSurfaceLib/include/voxel_imposter.h MatterSurfaceLib/src/voxel_imposter.cpp MatterSurfaceLib/tests/voxel_imposter_tests.cpp
git commit -m "feat: triangle/voxel-box SAT overlap test"
```

---

## Task 4: Octahedral normal encode/decode

**Files:**
- Modify: `MatterSurfaceLib/include/voxel_imposter.h`
- Modify: `MatterSurfaceLib/src/voxel_imposter.cpp`
- Modify: `MatterSurfaceLib/tests/voxel_imposter_tests.cpp`

- [ ] **Step 1: Declare in header**
```cpp
// Octahedral-encode a unit normal into two bytes (RG8) and back. Must match the
// GLSL decode in bvh_tlas_common.glsl (Task 12).
void oct_encode(const float n[3], uint8_t out[2]);
void oct_decode(const uint8_t in[2], float n[3]);
```

- [ ] **Step 2: Write failing round-trip test**
```cpp
static void test_oct_roundtrip() {
    const float ns[][3] = {{0,0,1},{0,0,-1},{1,0,0},{0,1,0},
                           {0.577f,0.577f,0.577f},{-0.5f,0.7f,-0.5f}};
    for (auto& n : ns) {
        float ln=std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
        float u[3]={n[0]/ln,n[1]/ln,n[2]/ln};
        uint8_t enc[2]; oct_encode(u,enc);
        float dec[3]; oct_decode(enc,dec);
        float err=std::fabs(u[0]-dec[0])+std::fabs(u[1]-dec[1])+std::fabs(u[2]-dec[2]);
        CHECK(err<0.02f, "oct round-trip within tolerance");
    }
}
```
Add `test_oct_roundtrip();` to main.

- [ ] **Step 3: Run, expect FAIL (undefined reference)**

Run: `make -C MatterSurfaceLib/tests voxel_imposter_tests`
Expected: link error for `oct_encode`.

- [ ] **Step 4: Implement** in `voxel_imposter.cpp`:
```cpp
void oct_encode(const float n[3], uint8_t out[2]) {
    float ax=std::fabs(n[0])+std::fabs(n[1])+std::fabs(n[2]);
    float x=n[0]/ax, y=n[1]/ax;
    if (n[2] < 0.0f) {
        float ox=(1.0f-std::fabs(y))*(x>=0?1.0f:-1.0f);
        float oy=(1.0f-std::fabs(x))*(y>=0?1.0f:-1.0f);
        x=ox; y=oy;
    }
    auto q=[&](float v){ v=0.5f*(v+1.0f); v=v<0?0:(v>1?1:v); return (uint8_t)(v*255.0f+0.5f); };
    out[0]=q(x); out[1]=q(y);
}
void oct_decode(const uint8_t in[2], float n[3]) {
    float x=in[0]/255.0f*2.0f-1.0f, y=in[1]/255.0f*2.0f-1.0f;
    float z=1.0f-std::fabs(x)-std::fabs(y);
    if (z<0.0f) { float ox=(1.0f-std::fabs(y))*(x>=0?1.0f:-1.0f);
                  float oy=(1.0f-std::fabs(x))*(y>=0?1.0f:-1.0f); x=ox; y=oy; }
    float l=std::sqrt(x*x+y*y+z*z); if(l<1e-12f)l=1.0f;
    n[0]=x/l; n[1]=y/l; n[2]=z/l;
}
```

- [ ] **Step 5: Run, expect PASS**

Run: `make -C MatterSurfaceLib/tests run-vox`
Expected: `All voxel_imposter tests passed`

- [ ] **Step 6: Commit**

```bash
git add MatterSurfaceLib/include/voxel_imposter.h MatterSurfaceLib/src/voxel_imposter.cpp MatterSurfaceLib/tests/voxel_imposter_tests.cpp
git commit -m "feat: octahedral normal encode/decode"
```

---

## Task 5: Material-aware flatten (albedo source)

The voxel bake needs per-triangle albedo. `BLASEntry` (blas_manager.hpp:54-64) keeps only `Tri`; `flatten_part_triangles` (imposter_bake.cpp:129) drops material. Retain a parallel `TriEx` array on `BLASEntry` and add a flatten variant that returns per-world-triangle `materialId` + `tint`, so the bake can look up `MaterialRegistryGet(materialId)->albedo` and blend the tint.

**Files:**
- Modify: `MatterSurfaceLib/include/blas_manager.hpp` (BLASEntry + constructor + register paths)
- Modify: `MatterSurfaceLib/src/blas_manager.cpp`
- Modify: `MatterSurfaceLib/include/voxel_imposter.h` (FlatTri type + flatten decl)
- Modify: `MatterSurfaceLib/src/voxel_imposter.cpp`
- Modify: `MatterSurfaceLib/tests/voxel_imposter_tests.cpp`

- [ ] **Step 1: Retain TriEx on BLASEntry**

In `blas_manager.hpp`, add to `BLASEntry` (after line 58):
```cpp
        std::vector<TriEx> tri_extra;   // parallel to triangles; empty if none supplied
```
Update the constructor (line 62-63) to accept and move it:
```cpp
        BLASEntry(BLASHandle h, std::unique_ptr<BvhMesh> m, std::unique_ptr<BVH> b,
                  std::vector<Tri>&& tris, std::vector<TriEx>&& tex, uint32_t hash_val)
            : handle(h), mesh(std::move(m)), bvh(std::move(b)), triangles(std::move(tris)),
              tri_extra(std::move(tex)), hash(hash_val), ref_count(1) {}
```

- [ ] **Step 2: Populate tri_extra at registration**

In `blas_manager.cpp`, find every `BLASEntry` construction (where the entry is emplaced). For the `register_triangles(..., const TriEx* triex)` and `(..., const std::vector<TriEx>&)` paths, build a `std::vector<TriEx>` copy (or empty when `triex==nullptr`) and pass it to the constructor. For paths with no TriEx, pass `std::vector<TriEx>{}`.

Run: `make WSL_LINUX=1 -C MatterSurfaceLib -j4 2>&1 | tail -5`
Expected: compiles (constructor signature change resolved at all call sites).

- [ ] **Step 3: Declare the flatten variant in voxel_imposter.h**
```cpp
#include "tlas_manager.hpp"
#include "blas_manager.hpp"
// A flattened world-space triangle plus its material/tint for albedo baking.
struct FlatTri { float3 v0, v1, v2; int materialId; float tint[4]; };
// World-space flatten of all TLAS instances' triangles, carrying per-triangle
// materialId+tint (from the BLAS tri_extra, falling back to the draw record's
// material when tri_extra is empty). GL-free.
std::vector<FlatTri> flatten_part_triangles_mat(const BLASManager& blas,
                                                const TLASManager& tlas);
```

- [ ] **Step 4: Write a failing test** (uses a tiny hand-built BLAS+TLAS)
```cpp
#include "../include/blas_manager.hpp"
#include "../include/tlas_manager.hpp"
static void test_flatten_mat_count() {
    BLASManager blas; TLASManager tlas;
    Tri t{}; t.vertex0=make_float3(0,0,0); t.vertex1=make_float3(1,0,0); t.vertex2=make_float3(0,1,0);
    TriEx tx{}; tx.materialId=2; tx.tint=make_float4(1,1,1,0);
    auto h = blas.register_triangles(std::vector<Tri>{t}, std::vector<TriEx>{tx});
    TLASManager::DrawInstance di; di.blas_handle=h; di.material_id=0; di.transform=Matrix4x4();
    std::vector<TLASManager::DrawInstance> b{di}; tlas.draw_batch(b); tlas.build(blas);
    auto fl = voxel_imposter::flatten_part_triangles_mat(blas, tlas);
    CHECK(fl.size()==1, "one flattened triangle");
    CHECK(fl[0].materialId==2, "materialId carried through flatten");
}
```
Add `test_flatten_mat_count();` to main. (Confirm the exact `register_triangles` overload + `Matrix4x4`/`make_float4` names against the headers before finalizing; adjust to match.)

- [ ] **Step 5: Run, expect FAIL (undefined reference to flatten_part_triangles_mat)**

Run: `make -C MatterSurfaceLib/tests voxel_imposter_tests`
Expected: link error.

- [ ] **Step 6: Implement** in `voxel_imposter.cpp` (mirror `flatten_part_triangles` at imposter_bake.cpp:129):
```cpp
std::vector<FlatTri> flatten_part_triangles_mat(const BLASManager& blas, const TLASManager& tlas) {
    std::vector<FlatTri> out;
    const auto& recs = tlas.get_draw_records();
    for (const auto& r : recs) {
        const BLASManager::BLASEntry* e = blas.get_entry(r.blas_handle);
        if (!e) continue;
        const float* m = r.transform.m;
        auto xf = [&](float3 p){ return make_float3(
            m[0]*p.x+m[1]*p.y+m[2]*p.z+m[3],
            m[4]*p.x+m[5]*p.y+m[6]*p.z+m[7],
            m[8]*p.x+m[9]*p.y+m[10]*p.z+m[11]); };
        for (size_t i=0;i<e->triangles.size();++i) {
            const Tri& t=e->triangles[i];
            FlatTri f{}; f.v0=xf(t.vertex0); f.v1=xf(t.vertex1); f.v2=xf(t.vertex2);
            if (i<e->tri_extra.size()) { f.materialId=e->tri_extra[i].materialId;
                const float4& tn=e->tri_extra[i].tint; f.tint[0]=tn.x; f.tint[1]=tn.y; f.tint[2]=tn.z; f.tint[3]=tn.w; }
            else { f.materialId=r.material_id; f.tint[0]=f.tint[1]=f.tint[2]=1.0f; f.tint[3]=0.0f; }
            out.push_back(f);
        }
    }
    return out;
}
```

- [ ] **Step 7: Run vox tests + full app build, expect PASS**

Run: `make -C MatterSurfaceLib/tests run-vox && make WSL_LINUX=1 -C MatterSurfaceLib -j4 2>&1 | tail -3`
Expected: vox tests pass; app links.

- [ ] **Step 8: Commit**

```bash
git add MatterSurfaceLib/include/blas_manager.hpp MatterSurfaceLib/src/blas_manager.cpp \
        MatterSurfaceLib/include/voxel_imposter.h MatterSurfaceLib/src/voxel_imposter.cpp \
        MatterSurfaceLib/tests/voxel_imposter_tests.cpp
git commit -m "feat: retain TriEx on BLAS entries + material-aware flatten"
```

---

## Task 6: Surface voxelization + albedo/normal accumulation (the bake)

**Files:**
- Modify: `MatterSurfaceLib/include/voxel_imposter.h`
- Modify: `MatterSurfaceLib/src/voxel_imposter.cpp`
- Modify: `MatterSurfaceLib/tests/voxel_imposter_tests.cpp`

- [ ] **Step 1: Declare bake_voxels in the header**
```cpp
// Surface-voxelize the flattened triangles into a dense grid: coverage=255 for
// any voxel a triangle overlaps (tri_box_overlap), with area-weighted albedo
// (MaterialRegistryGet(materialId)->albedo blended by tint) and area-weighted
// octahedral normal per covered voxel. Fills bounds/dims/coverage/albedo/normal.
// Returns false on empty/degenerate input. GL-free, unit-testable.
bool bake_voxels(const std::vector<FlatTri>& tris, const VoxGenParams& p,
                 uint64_t source_part_hash, VoxelImposter& out);
```

- [ ] **Step 2: Write a failing test** — one axis-aligned quad in the z=mid plane fills a connected surface set, leaving interior voxels on other planes empty (surface, not solid):
```cpp
static void test_bake_surface_only() {
    // A 1x1 quad at z=0.5 spanning x,y in [0,1], inside a unit-ish AABB padded in z.
    voxel_imposter::FlatTri a{}, b{};
    a.v0=make_float3(0,0,0.5f); a.v1=make_float3(1,0,0.5f); a.v2=make_float3(1,1,0.5f);
    b.v0=make_float3(0,0,0.5f); b.v1=make_float3(1,1,0.5f); b.v2=make_float3(0,1,0.5f);
    a.materialId=b.materialId=0; a.tint[0]=a.tint[1]=a.tint[2]=1; a.tint[3]=0;
    b.tint[0]=b.tint[1]=b.tint[2]=1; b.tint[3]=0;
    // Force a z extent so the grid has several z layers.
    a.v0.z=0.0f; // give the AABB z-extent via a stray low vertex? Instead set bounds via a third tri.
    voxel_imposter::FlatTri c=a; c.v0=make_float3(0,0,0.0f); c.v1=make_float3(0,0,1.0f); c.v2=make_float3(0,0,1.0f);
    std::vector<voxel_imposter::FlatTri> tris{a,b,c};
    voxel_imposter::VoxGenParams p{16,0,0.5f};
    voxel_imposter::VoxelImposter v;
    CHECK(voxel_imposter::bake_voxels(tris,p,123,v), "bake ok");
    int covered=0; for(uint8_t c8:v.coverage) if(c8>0) ++covered;
    int total=v.nx*v.ny*v.nz;
    CHECK(covered>0, "some voxels covered");
    CHECK(covered < total, "surface fill is sparse, not solid");
    CHECK(v.albedo.size()==(size_t)total*3, "albedo sized");
    CHECK(v.normal.size()==(size_t)total*2, "normal sized");
    CHECK(v.source_part_hash==123, "hash stored");
}
```
(Refine the test geometry while implementing so it cleanly yields a sub-solid covered fraction; the invariant under test is "surface-only, sized arrays, hash stored.") Add to main.

- [ ] **Step 3: Run, expect FAIL (undefined reference)**

Run: `make -C MatterSurfaceLib/tests voxel_imposter_tests`
Expected: link error for `bake_voxels`.

- [ ] **Step 4: Implement bake_voxels** in `voxel_imposter.cpp`:
```cpp
#include "../include/material_registry.h"

bool bake_voxels(const std::vector<FlatTri>& tris, const VoxGenParams& p,
                 uint64_t source_part_hash, VoxelImposter& out) {
    if (tris.empty() || p.maxDim < 1) return false;
    float lo[3]={1e30f,1e30f,1e30f}, hi[3]={-1e30f,-1e30f,-1e30f};
    auto grow=[&](const float3& v){ lo[0]=std::min(lo[0],v.x);lo[1]=std::min(lo[1],v.y);lo[2]=std::min(lo[2],v.z);
                                    hi[0]=std::max(hi[0],v.x);hi[1]=std::max(hi[1],v.y);hi[2]=std::max(hi[2],v.z); };
    for (auto& t:tris){ grow(t.v0); grow(t.v1); grow(t.v2); }
    int nx,ny,nz;
    if (!choose_grid_dims(lo,hi,p.maxDim,nx,ny,nz)) return false;

    out = VoxelImposter{};
    out.source_part_hash = source_part_hash;
    for (int i=0;i<3;++i){ out.bounds_min[i]=lo[i]; out.bounds_max[i]=hi[i]; }
    out.nx=nx; out.ny=ny; out.nz=nz;
    const size_t N=(size_t)nx*ny*nz;
    out.coverage.assign(N,0); out.albedo.assign(N*3,0); out.normal.assign(N*2,0);

    // Per-voxel weighted accumulators.
    std::vector<float> wsum(N,0.0f), nacc(N*3,0.0f), aacc(N*3,0.0f);
    float cell[3]={ (hi[0]-lo[0])/std::max(1,nx), (hi[1]-lo[1])/std::max(1,ny), (hi[2]-lo[2])/std::max(1,nz) };
    for (int a=0;a<3;++a) if (cell[a]<=0.0f) cell[a]=1e-6f;
    float half[3]={cell[0]*0.5f,cell[1]*0.5f,cell[2]*0.5f};

    for (const FlatTri& t : tris) {
        const float V0[3]={t.v0.x,t.v0.y,t.v0.z}, V1[3]={t.v1.x,t.v1.y,t.v1.z}, V2[3]={t.v2.x,t.v2.y,t.v2.z};
        // Triangle AABB -> voxel index range.
        float tlo[3]={std::min(V0[0],std::min(V1[0],V2[0])),std::min(V0[1],std::min(V1[1],V2[1])),std::min(V0[2],std::min(V1[2],V2[2]))};
        float thi[3]={std::max(V0[0],std::max(V1[0],V2[0])),std::max(V0[1],std::max(V1[1],V2[1])),std::max(V0[2],std::max(V1[2],V2[2]))};
        int x0=std::max(0,(int)std::floor((tlo[0]-lo[0])/cell[0])), x1=std::min(nx-1,(int)std::floor((thi[0]-lo[0])/cell[0]));
        int y0=std::max(0,(int)std::floor((tlo[1]-lo[1])/cell[1])), y1=std::min(ny-1,(int)std::floor((thi[1]-lo[1])/cell[1]));
        int z0=std::max(0,(int)std::floor((tlo[2]-lo[2])/cell[2])), z1=std::min(nz-1,(int)std::floor((thi[2]-lo[2])/cell[2]));
        // Face normal + area for weighting.
        float e1[3]={V1[0]-V0[0],V1[1]-V0[1],V1[2]-V0[2]}, e2[3]={V2[0]-V0[0],V2[1]-V0[1],V2[2]-V0[2]};
        float fn[3]={e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0]};
        float area2=std::sqrt(fn[0]*fn[0]+fn[1]*fn[1]+fn[2]*fn[2]);
        float w=std::max(1e-6f, 0.5f*area2);
        float un[3]={fn[0],fn[1],fn[2]}; if(area2>1e-12f){un[0]/=area2;un[1]/=area2;un[2]/=area2;}
        const MaterialDef* md=MaterialRegistryGet(t.materialId);
        float al[3]={md->albedo[0],md->albedo[1],md->albedo[2]};
        if (t.tint[3]>0.0f){ for(int k=0;k<3;++k) al[k]=al[k]*(1.0f-t.tint[3])+t.tint[k]*t.tint[3]; }
        for (int z=z0;z<=z1;++z) for (int y=y0;y<=y1;++y) for (int x=x0;x<=x1;++x) {
            float bc[3]={lo[0]+(x+0.5f)*cell[0], lo[1]+(y+0.5f)*cell[1], lo[2]+(z+0.5f)*cell[2]};
            if (!tri_box_overlap(bc,half,V0,V1,V2)) continue;
            size_t vi=(size_t)out.voxel_index(x,y,z);
            wsum[vi]+=w;
            for(int k=0;k<3;++k){ nacc[vi*3+k]+=un[k]*w; aacc[vi*3+k]+=al[k]*w; }
        }
    }
    for (size_t vi=0;vi<N;++vi) {
        if (wsum[vi] <= 0.0f) continue;
        out.coverage[vi]=255;
        float inv=1.0f/wsum[vi];
        for(int k=0;k<3;++k) out.albedo[vi*3+k]=(uint8_t)std::min(255.0f,std::max(0.0f, aacc[vi*3+k]*inv*255.0f+0.5f));
        float nn[3]={nacc[vi*3]*inv, nacc[vi*3+1]*inv, nacc[vi*3+2]*inv};
        float l=std::sqrt(nn[0]*nn[0]+nn[1]*nn[1]+nn[2]*nn[2]); if(l<1e-12f){nn[0]=0;nn[1]=0;nn[2]=1;l=1;} else {nn[0]/=l;nn[1]/=l;nn[2]/=l;}
        oct_encode(nn,&out.normal[vi*2]);
    }
    (void)p.coverThresh; // binary coverage in v1
    return true;
}
```

- [ ] **Step 5: Run, expect PASS**

Run: `make -C MatterSurfaceLib/tests run-vox`
Expected: `All voxel_imposter tests passed`

- [ ] **Step 6: Commit**

```bash
git add MatterSurfaceLib/include/voxel_imposter.h MatterSurfaceLib/src/voxel_imposter.cpp MatterSurfaceLib/tests/voxel_imposter_tests.cpp
git commit -m "feat: surface voxelization with area-weighted albedo + normal"
```

---

## Task 7: Serialization (.vxi)

**Files:**
- Modify: `MatterSurfaceLib/include/voxel_imposter.h`
- Modify: `MatterSurfaceLib/src/voxel_imposter.cpp`
- Modify: `MatterSurfaceLib/tests/voxel_imposter_tests.cpp`

Mirror `imposter_asset::save/load` (imposter_asset.cpp:49-163): atomic temp+rename, magic/version/imp_hash/source_hash/content_hash guards.

- [ ] **Step 1: Declare in header**
```cpp
uint64_t compute_vox_hash(const VoxGenParams& p);   // fnv1a64(p) ^ kFormatVersion
std::string cache_path(uint64_t hash);              // "imposters/<16hex>.vxi"
bool save(const std::string& path, const VoxelImposter& v, uint64_t vox_hash);
bool load(const std::string& path, uint64_t expected_vox_hash,
          uint64_t expected_source_hash, VoxelImposter& out);
```

- [ ] **Step 2: Write failing round-trip + guard tests**
```cpp
static void test_vox_roundtrip() {
    voxel_imposter::VoxelImposter v; v.nx=2;v.ny=2;v.nz=2; v.source_part_hash=99;
    for(int i=0;i<3;++i){v.bounds_min[i]=0;v.bounds_max[i]=1;}
    v.coverage.assign(8,0); v.coverage[0]=255; v.albedo.assign(24,7); v.normal.assign(16,3);
    const char* path="imposters/test_rt.vxi";
    CHECK(voxel_imposter::save(path,v,0xABCD), "save ok");
    voxel_imposter::VoxelImposter r;
    CHECK(voxel_imposter::load(path,0xABCD,99,r), "load ok");
    CHECK(r.nx==2&&r.coverage[0]==255&&r.albedo[0]==7&&r.normal[0]==3, "round-trip data");
    voxel_imposter::VoxelImposter bad;
    CHECK(!voxel_imposter::load(path,0xBEEF,99,bad), "vox-hash mismatch rejected");
    CHECK(!voxel_imposter::load(path,0xABCD,1,bad), "source-hash mismatch rejected");
}
```
Add to main.

- [ ] **Step 3: Run, expect FAIL (undefined reference)**

Run: `make -C MatterSurfaceLib/tests voxel_imposter_tests`
Expected: link error for `save`.

- [ ] **Step 4: Implement** save/load/compute_vox_hash/cache_path in `voxel_imposter.cpp`, copying the `put/put_bytes/ensure_parent_dir/Reader` helpers and structure from imposter_asset.cpp:15-163 but serializing `bounds_min/max, nx/ny/nz, coverage, albedo, normal`. Header fields: `kMagic, kFormatVersion, vox_hash, source_part_hash, content_hash`. `compute_vox_hash` = `part_asset::fnv1a64(&p,sizeof(p)) ^ kFormatVersion`. `cache_path` returns `"imposters/<16hex>.vxi"`.

- [ ] **Step 5: Run, expect PASS**

Run: `make -C MatterSurfaceLib/tests run-vox`
Expected: `All voxel_imposter tests passed`

- [ ] **Step 6: Commit**

```bash
git add MatterSurfaceLib/include/voxel_imposter.h MatterSurfaceLib/src/voxel_imposter.cpp MatterSurfaceLib/tests/voxel_imposter_tests.cpp
git commit -m "feat: .vxi serialization with content/version/hash guards"
```

---

## Task 8: 3D-DDA traversal helper (host-portable)

A GL-free reference of the exact DDA the shader will run, so traversal order + first-hit are unit-tested before going to GLSL. The shader (Task 12) ports this logic line-for-line.

**Files:**
- Modify: `MatterSurfaceLib/include/voxel_imposter.h`
- Modify: `MatterSurfaceLib/src/voxel_imposter.cpp`
- Modify: `MatterSurfaceLib/tests/voxel_imposter_tests.cpp`

- [ ] **Step 1: Declare in header**
```cpp
// Amanatides-Woo 3D-DDA over a coverage grid in NORMALIZED box space [0,1]^3.
// origin/dir are in box space. On the first voxel with coverage>0 sets
// hitX/Y/Z and tHit (ray param in box space) and returns true; false on
// pass-through. dims = nx,ny,nz. coverage indexed via (z*ny+y)*nx+x.
bool dda_first_hit(const float origin[3], const float dir[3],
                   int nx,int ny,int nz, const std::vector<uint8_t>& coverage,
                   int& hitX,int& hitY,int& hitZ, float& tHit);
```

- [ ] **Step 2: Write failing tests** — plant a single covered voxel and shoot a ray that must reach it; and a ray that misses all coverage returns false:
```cpp
static void test_dda_hits_planted_voxel() {
    int nx=8,ny=8,nz=8; std::vector<uint8_t> cov((size_t)nx*ny*nz,0);
    auto idx=[&](int x,int y,int z){ return (z*ny+y)*nx+x; };
    cov[idx(4,4,4)]=255;
    float o[3]={0.01f,0.5625f,0.5625f}, d[3]={1,0,0}; // travels +x at the row/col of voxel 4
    int hx,hy,hz; float t;
    CHECK(dda_first_hit(o,d,nx,ny,nz,cov,hx,hy,hz,t), "ray reaches planted voxel");
    CHECK(hx==4&&hy==4&&hz==4, "first hit is the planted voxel");
}
static void test_dda_pass_through() {
    int nx=8,ny=8,nz=8; std::vector<uint8_t> cov((size_t)nx*ny*nz,0);
    float o[3]={0.01f,0.5f,0.5f}, d[3]={1,0,0};
    int hx,hy,hz; float t;
    CHECK(!dda_first_hit(o,d,nx,ny,nz,cov,hx,hy,hz,t), "empty grid passes through");
}
```
Add to main.

- [ ] **Step 3: Run, expect FAIL (undefined reference)**

Run: `make -C MatterSurfaceLib/tests voxel_imposter_tests`
Expected: link error for `dda_first_hit`.

- [ ] **Step 4: Implement dda_first_hit** in `voxel_imposter.cpp`:
```cpp
bool dda_first_hit(const float o[3], const float d[3],
                   int nx,int ny,int nz, const std::vector<uint8_t>& cov,
                   int& hitX,int& hitY,int& hitZ, float& tHit) {
    int dim[3]={nx,ny,nz};
    // Clamp the start into the grid; assume caller passes a point on/inside [0,1]^3.
    float p[3]={o[0],o[1],o[2]};
    int vx[3];
    for (int a=0;a<3;++a){ int c=(int)std::floor(p[a]*dim[a]); vx[a]=std::max(0,std::min(dim[a]-1,c)); }
    int step[3]; float tMax[3], tDelta[3];
    for (int a=0;a<3;++a){
        if (std::fabs(d[a])<1e-12f){ step[a]=0; tMax[a]=1e30f; tDelta[a]=1e30f; continue; }
        step[a]=d[a]>0?1:-1;
        float cellSize=1.0f/dim[a];
        float voxelBoundary=(vx[a]+(step[a]>0?1:0))*cellSize;
        tMax[a]=(voxelBoundary-p[a])/d[a];
        tDelta[a]=cellSize/std::fabs(d[a]);
    }
    auto idx=[&](int x,int y,int z){ return (size_t)((z*ny+y)*nx+x); };
    if (cov[idx(vx[0],vx[1],vx[2])]>0){ hitX=vx[0];hitY=vx[1];hitZ=vx[2]; tHit=0.0f; return true; }
    for (int guard=0; guard < (nx+ny+nz)*2; ++guard) {
        int axis = (tMax[0]<tMax[1]) ? (tMax[0]<tMax[2]?0:2) : (tMax[1]<tMax[2]?1:2);
        vx[axis]+=step[axis];
        if (vx[axis]<0||vx[axis]>=dim[axis]) return false;
        float tEnter=tMax[axis];
        tMax[axis]+=tDelta[axis];
        if (cov[idx(vx[0],vx[1],vx[2])]>0){ hitX=vx[0];hitY=vx[1];hitZ=vx[2]; tHit=tEnter; return true; }
    }
    return false;
}
```

- [ ] **Step 5: Run, expect PASS**

Run: `make -C MatterSurfaceLib/tests run-vox`
Expected: `All voxel_imposter tests passed`

- [ ] **Step 6: Commit**

```bash
git add MatterSurfaceLib/include/voxel_imposter.h MatterSurfaceLib/src/voxel_imposter.cpp MatterSurfaceLib/tests/voxel_imposter_tests.cpp
git commit -m "feat: host-portable 3D-DDA first-hit reference"
```

---

## Task 9: Runtime — unit-cube BLAS + AABB→box instance

**Files:**
- Modify: `MatterSurfaceLib/main.cpp`
- Add to MatterSurfaceLib/Makefile SRC: `src/voxel_imposter.cpp`

Replace the chart-cage demo's BLAS/instance registration (main.cpp ~610-642) with a unit-cube BLAS and an instance transform that maps `[0,1]³` to the part's world AABB.

- [ ] **Step 1: Add voxel_imposter.cpp to the build**

In `MatterSurfaceLib/Makefile` SRC list (line 130), append `src/voxel_imposter.cpp` (leave `src/imposter_asset.cpp`/`src/imposter_bake.cpp` for now — removed in T15).

- [ ] **Step 2: Write a unit-cube triangle helper + bake/upload in setup_imposter_demo**

In main.cpp, replace the body that builds `cage_tris`/`bake_imposter` with:
```cpp
// Bake (or load) the voxel volume for the demo part.
std::vector<voxel_imposter::FlatTri> part_tris =
    voxel_imposter::flatten_part_triangles_mat(*blas_manager_, *tlas_manager_);
voxel_imposter::VoxGenParams vp{128, 0, 0.5f};
uint64_t vhash = voxel_imposter::compute_vox_hash(vp);
uint64_t shash = /* same source_part_hash used today */;
voxel_imposter::VoxelImposter vox;
std::string vpath = voxel_imposter::cache_path(vhash);
if (!voxel_imposter::load(vpath, vhash, shash, vox)) {
    if (!voxel_imposter::bake_voxels(part_tris, vp, shash, vox)) { /* skip imposter */ return; }
    voxel_imposter::save(vpath, vox, vhash);
}
voxel_imposter_ = vox; // store dims/bounds for uniforms

// Unit-cube BLAS: 12 triangles with corners at [0,1]^3.
std::vector<Tri> cube = make_unit_cube_tris(); // helper added below
imposter_cube_blas_ = blas_manager_->register_triangles(cube.data(), (int)cube.size(), nullptr);

// Instance: map [0,1]^3 -> world AABB (scale by extent, translate to bounds_min), + demo +X offset.
TLASManager::DrawInstance di; di.blas_handle=imposter_cube_blas_; di.material_id=0; di.is_imposter=true;
Matrix4x4 T; // row-major; diag = extent, translation = bounds_min
float ex=vox.bounds_max[0]-vox.bounds_min[0], ey=vox.bounds_max[1]-vox.bounds_min[1], ez=vox.bounds_max[2]-vox.bounds_min[2];
T.m[0]=ex; T.m[5]=ey; T.m[10]=ez;
T.m[3]=vox.bounds_min[0]+24.0f; T.m[7]=vox.bounds_min[1]; T.m[11]=vox.bounds_min[2];
di.transform=T;
std::vector<TLASManager::DrawInstance> one{di};
tlas_manager_->draw_batch(one); tlas_manager_->build(*blas_manager_);
```
Add `make_unit_cube_tris()` (12 tris, corners 0/1 per axis, CCW outward) as a static helper in main.cpp. Add `voxel_imposter_`, `imposter_cube_blas_` members. (Match exact `Matrix4x4` field/layout and `register_triangles` overload against headers.)

- [ ] **Step 3: Build, expect link/compile success**

Run: `make WSL_LINUX=1 -C MatterSurfaceLib shaders && make WSL_LINUX=1 -C MatterSurfaceLib -j4 2>&1 | tail -5`
Expected: links (shader still references old imposter uniforms — fine until Task 11/12; the app builds).

- [ ] **Step 4: Commit**

```bash
git add MatterSurfaceLib/main.cpp MatterSurfaceLib/Makefile
git commit -m "feat: unit-cube imposter BLAS + AABB->box instance, voxel bake/load"
```

---

## Task 10: Runtime — 3D texture upload + sampler3D binding

raylib's `LoadTextureFromImage`/`SetShaderValueTexture` are 2D-only. Use raw GL: `glGenTextures` + `glBindTexture(GL_TEXTURE_3D,...)` + `glTexImage3D`, and bind per-frame with `glActiveTexture`/`glBindTexture` + `glUniform1i` on a free texture unit (use units 10/11 to avoid raylib's low units).

**Files:**
- Modify: `MatterSurfaceLib/main.cpp`

- [ ] **Step 1: Create the two 3D textures after bake**

Add a helper in main.cpp (near GL includes; rlgl pulls in glad):
```cpp
static unsigned int upload_volume_rgba8(int nx,int ny,int nz,const uint8_t* data){
    unsigned int id=0; glGenTextures(1,&id); glBindTexture(GL_TEXTURE_3D,id);
    glTexParameteri(GL_TEXTURE_3D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_3D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_3D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D,GL_TEXTURE_WRAP_R,GL_CLAMP_TO_EDGE);
    glTexImage3D(GL_TEXTURE_3D,0,GL_RGBA8,nx,ny,nz,0,GL_RGBA,GL_UNSIGNED_BYTE,data);
    glBindTexture(GL_TEXTURE_3D,0); return id;
}
static unsigned int upload_volume_rg8(int nx,int ny,int nz,const uint8_t* data){
    unsigned int id=0; glGenTextures(1,&id); glBindTexture(GL_TEXTURE_3D,id);
    glTexParameteri(GL_TEXTURE_3D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_3D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_3D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D,GL_TEXTURE_WRAP_R,GL_CLAMP_TO_EDGE);
    glTexImage3D(GL_TEXTURE_3D,0,GL_RG8,nx,ny,nz,0,GL_RG,GL_UNSIGNED_BYTE,data);
    glBindTexture(GL_TEXTURE_3D,0); return id;
}
```
Build the interleaved RGBA8 color+coverage volume on the CPU first (RGB = albedo, A = coverage):
```cpp
std::vector<uint8_t> rgba((size_t)vox.nx*vox.ny*vox.nz*4);
for (size_t i=0,n=(size_t)vox.nx*vox.ny*vox.nz;i<n;++i){
    rgba[i*4+0]=vox.albedo[i*3+0]; rgba[i*4+1]=vox.albedo[i*3+1];
    rgba[i*4+2]=vox.albedo[i*3+2]; rgba[i*4+3]=vox.coverage[i]; }
imposter_color_vol_ = upload_volume_rgba8(vox.nx,vox.ny,vox.nz,rgba.data());
imposter_normal_vol_ = upload_volume_rg8(vox.nx,vox.ny,vox.nz,vox.normal.data());
```
Add `imposter_color_vol_`, `imposter_normal_vol_` (unsigned int) members.

- [ ] **Step 2: Bind the 3D textures + uniforms each frame**

Replace the old imposter uniform block (main.cpp ~1393-1404) with:
```cpp
if (imposter_enabled_) {
    int colUnit=10, nrmUnit=11;
    glActiveTexture(GL_TEXTURE0+colUnit); glBindTexture(GL_TEXTURE_3D, imposter_color_vol_);
    glUniform1i(GetShaderLocation(raytracing_shader_,"imposterColorVolume"), colUnit);
    glActiveTexture(GL_TEXTURE0+nrmUnit); glBindTexture(GL_TEXTURE_3D, imposter_normal_vol_);
    glUniform1i(GetShaderLocation(raytracing_shader_,"imposterNormalVolume"), nrmUnit);
    glActiveTexture(GL_TEXTURE0);
    // Box AABB so the shader maps local hit -> [0,1]^3 (dims via textureSize()).
    float bmin[3]={voxel_imposter_.bounds_min[0],voxel_imposter_.bounds_min[1],voxel_imposter_.bounds_min[2]};
    float bext[3]={voxel_imposter_.bounds_max[0]-bmin[0],voxel_imposter_.bounds_max[1]-bmin[1],voxel_imposter_.bounds_max[2]-bmin[2]};
    SetShaderValue(raytracing_shader_, GetShaderLocation(raytracing_shader_,"imposterBoxMin"), bmin, SHADER_UNIFORM_VEC3);
    SetShaderValue(raytracing_shader_, GetShaderLocation(raytracing_shader_,"imposterBoxExt"), bext, SHADER_UNIFORM_VEC3);
}
```
(The `glUniform1i` calls require the shader to be active; ensure they run after `BeginShaderMode`/the existing bind sequence. If raylib defers shader binding to draw time, set these via the rlgl draw path the existing uniforms used — confirm where the current `SetShaderValueTexture` block executes and place these in the same scope.)

- [ ] **Step 3: Build**

Run: `make WSL_LINUX=1 -C MatterSurfaceLib -j4 2>&1 | tail -5`
Expected: links. (Shader will warn about unused/missing uniforms until Task 12 declares them — acceptable; verify no hard errors.)

- [ ] **Step 4: Commit**

```bash
git add MatterSurfaceLib/main.cpp
git commit -m "feat: upload voxel volumes as GL 3D textures + bind sampler3D"
```

---

## Task 11: Shader — declare 3D samplers + entry/exit box mapping

Split the shader work in two (declarations + ray-into-box mapping here; the DDA march + HitResult in Task 12) so each compiles and is inspectable.

**Files:**
- Modify: `MatterSurfaceLib/shaders/bvh_tlas_common.glsl`
- Run `make shaders` after edits.

- [ ] **Step 1: Add uniforms** (near the existing imposter samplers, lines 16-24)
```glsl
uniform sampler3D imposterColorVolume;   // RGB albedo, A coverage
uniform sampler3D imposterNormalVolume;  // RG octahedral
uniform vec3 imposterBoxMin;
uniform vec3 imposterBoxExt;
```

- [ ] **Step 2: Add GLSL octahedral decode + a stub voxelMarch** matching the host `oct_decode`/`dda_first_hit`:
```glsl
vec3 octDecode(vec2 e){
    vec2 f = e*2.0-1.0;
    vec3 n = vec3(f.x, f.y, 1.0-abs(f.x)-abs(f.y));
    float t = max(-n.z,0.0);
    n.x += n.x>=0.0 ? -t : t;
    n.y += n.y>=0.0 ? -t : t;
    return normalize(n);
}
// Filled in Task 12. Returns false for now (pass-through) so the build is green.
bool voxelMarch(vec3 originBox, vec3 dirBox, out ivec3 hitVox, out float tBox){
    hitVox = ivec3(0); tBox = 0.0; return false;
}
```

- [ ] **Step 3: Build shaders, expect success**

Run: `make WSL_LINUX=1 -C MatterSurfaceLib shaders 2>&1 | tail -5`
Expected: `raytrace_tlas_blas_processed.fs` regenerated, no preprocessor error.

- [ ] **Step 4: Commit**

```bash
git add MatterSurfaceLib/shaders/bvh_tlas_common.glsl MatterSurfaceLib/shaders/raytrace_tlas_blas_processed.fs
git commit -m "feat: shader 3D voxel samplers + octahedral decode (march stub)"
```

---

## Task 12: Shader — 3D-DDA march replaces relief march, populate HitResult

**Files:**
- Modify: `MatterSurfaceLib/shaders/bvh_tlas_common.glsl`
- Run `make shaders`.

- [ ] **Step 1: Implement voxelMarch** (port of `dda_first_hit`, sampling `imposterColorVolume.a` for coverage):
```glsl
bool voxelMarch(vec3 o, vec3 d, out ivec3 hitVox, out float tBox){
    ivec3 dim = textureSize(imposterColorVolume, 0);
    vec3 fdim = vec3(dim);
    ivec3 v = clamp(ivec3(floor(o*fdim)), ivec3(0), dim-1);
    vec3 step = sign(d);
    vec3 tDelta, tMax;
    for (int a=0;a<3;++a){
        if (abs(d[a])<1e-12){ tDelta[a]=1e30; tMax[a]=1e30; }
        else { float cs=1.0/fdim[a];
               float bnd=(float(v[a])+(d[a]>0.0?1.0:0.0))*cs;
               tMax[a]=(bnd-o[a])/d[a]; tDelta[a]=cs/abs(d[a]); }
    }
    if (texelFetch(imposterColorVolume, v, 0).a > 0.5){ hitVox=v; tBox=0.0; return true; }
    int budget = (dim.x+dim.y+dim.z)*2;
    for (int i=0;i<budget;++i){
        int axis = (tMax.x<tMax.y) ? (tMax.x<tMax.z?0:2) : (tMax.y<tMax.z?1:2);
        v[axis]+=int(step[axis]);
        if (v[axis]<0 || v[axis]>=dim[axis]) return false;
        float tEnter=tMax[axis];
        tMax[axis]+=tDelta[axis];
        if (texelFetch(imposterColorVolume, v, 0).a > 0.5){ hitVox=v; tBox=tEnter; return true; }
    }
    return false;
}
```

- [ ] **Step 2: Replace the imposter branch** (lines ~849-911, the `if (inst.isImposter)` block that calls `reliefMarch` and sets `bakedColor`) with the voxel path:
```glsl
if (inst.isImposter) {
    // Transform the world ray into local box space [0,1]^3.
    vec3 oLocal = transformPosition(ray.O, inst.invTransform); // unit-cube space [0,1]
    vec3 dLocal = transformVector(ray.D, inst.invTransform);
    ivec3 hv; float tBox;
    if (voxelMarch(oLocal, dLocal, hv, tBox)) {
        ivec3 dim = textureSize(imposterColorVolume,0);
        vec3 albedo = texelFetch(imposterColorVolume, hv, 0).rgb;
        vec2 ne = texelFetch(imposterNormalVolume, hv, 0).rg;
        vec3 nLocal = octDecode(ne);
        // World hit position: march t in local space maps to the same parametric t on the
        // world ray because invTransform is affine; recover world t via box entry.
        vec3 pLocal = oLocal + dLocal * tBox;
        vec3 pWorld = transformPosition(pLocal, inst.transform);
        result.hit = true;
        result.position = pWorld;
        result.t = length(pWorld - ray.O);
        result.normal = normalize(transformNormal(nLocal, inst.transform));
        result.tint = albedo;        // feed albedo through the standard tint path
        result.tintAlpha = 1.0;
        result.material = 0;
        result.isImposter = true;
        // (No bakedColor: lighting runs normally — Task 13.)
    } else {
        result.hit = false; // pass-through (porosity)
    }
}
```
(Confirm the exact in-scope variable names — `ray`, `result`, `inst` — and how a leaf currently writes `result`; match the surrounding code. The key behaviors: hit → standard HitResult with albedo; miss → `result.hit=false`.)

- [ ] **Step 3: Build shaders**

Run: `make WSL_LINUX=1 -C MatterSurfaceLib shaders 2>&1 | tail -5`
Expected: regenerated, no error.

- [ ] **Step 4: Build app + smoke-run headless** (confirms shader compiles on GL and doesn't crash)

Run:
```bash
make WSL_LINUX=1 -C MatterSurfaceLib -j4 2>&1 | tail -3 && \
cd MatterSurfaceLib && DISPLAY=:0 MSL_SHOW_IMPOSTER=1 MSL_RENDER_MODE=0 MSL_CAPTURE=vox_smoke.png MSL_FRAMES=2 \
  MSL_CAM="40,10,40,24,0,0" timeout 30 ./matter_surface_lib 2>&1 | tail -20; cd ..
```
Expected: no GL shader-compile error in output; `vox_smoke.png` written.

- [ ] **Step 5: Commit**

```bash
git add MatterSurfaceLib/shaders/bvh_tlas_common.glsl MatterSurfaceLib/shaders/raytrace_tlas_blas_processed.fs
git commit -m "feat: 3D-DDA voxel march replaces relief march in imposter branch"
```

---

## Task 13: Lighting integration (drop bakedColor bypass)

The voxel hit already populates a standard `HitResult` (Task 12). This task verifies the imposter is shaded by the normal lighting path and removes any remaining `bakedColor` special-casing for imposters in the shade stage.

**Files:**
- Modify: `MatterSurfaceLib/shaders/bvh_tlas_common.glsl` (and `lighting.glsl`/`raytrace_tlas_blas.fs` if they branch on `isImposter`/`bakedColor`)

- [ ] **Step 1: Find imposter shading special-cases**

Run: `grep -rn "bakedColor\|isImposter" MatterSurfaceLib/shaders`
Expected: a list including the old `bakedColor` use in the shade stage.

- [ ] **Step 2: Remove the bakedColor bypass** so an imposter hit shades like a triangle hit (albedo via `tint`/`tintAlpha`, world normal via `result.normal`). Keep `isImposter` only if it is needed for a non-lighting reason (e.g. AO defaults); otherwise drop the lighting branch.

- [ ] **Step 3: Build shaders + app**

Run: `make WSL_LINUX=1 -C MatterSurfaceLib shaders && make WSL_LINUX=1 -C MatterSurfaceLib -j4 2>&1 | tail -3`
Expected: builds clean.

- [ ] **Step 4: Capture to confirm dynamic shading** (imposter should respond to lights, not look flat-lit)

Run:
```bash
cd MatterSurfaceLib && DISPLAY=:0 MSL_SHOW_IMPOSTER=1 MSL_RENDER_MODE=0 MSL_CAPTURE=vox_lit.png MSL_FRAMES=4 \
  MSL_CAM="40,10,40,24,0,0" timeout 30 ./matter_surface_lib 2>&1 | tail -5; cd ..
```
Then Read `MatterSurfaceLib/vox_lit.png` to verify the box shows a shaded, normal-lit surface (gradient across the form), not a flat unlit fill.

- [ ] **Step 5: Commit**

```bash
git add MatterSurfaceLib/shaders
git commit -m "feat: shade voxel imposter via standard lighting (drop bakedColor)"
```

---

## Task 14: Visual validation — convex + non-convex parts

**Files:** none modified (captures + inspection). May add a non-convex demo part to `setup_imposter_demo` if one is not already available.

- [ ] **Step 1: Convex part — imposter vs real, multiple angles**

Capture the existing brick/sphere demo part as a voxel imposter from 3 angles and compare silhouette/shading against the real mesh (toggle `MSL_SHOW_IMPOSTER`):
```bash
cd MatterSurfaceLib
for cam in "40,10,40,24,0,0" "24,30,5,24,0,0" "0,5,40,24,0,0"; do
  DISPLAY=:0 MSL_SHOW_IMPOSTER=1 MSL_RENDER_MODE=0 MSL_CAPTURE="vox_conv_${cam//,/_}.png" \
    MSL_FRAMES=3 MSL_CAM="$cam" timeout 30 ./matter_surface_lib >/dev/null 2>&1
done
cd ..
```
Read each PNG; confirm silhouette matches the part and shading is consistent.

- [ ] **Step 2: Non-convex part — porosity + all-angle silhouette**

Ensure `setup_imposter_demo` can bake a forked/porous test part (a simple "tuning-fork"/cross of boxes stands in for a shrub). If not present, add a small procedural one behind an env flag (e.g. `MSL_IMPOSTER_PART=fork`). Capture from multiple angles:
```bash
cd MatterSurfaceLib
for cam in "40,10,40,24,0,0" "24,2,40,24,0,0" "44,2,4,24,0,0"; do
  DISPLAY=:0 MSL_SHOW_IMPOSTER=1 MSL_IMPOSTER_PART=fork MSL_RENDER_MODE=0 \
    MSL_CAPTURE="vox_fork_${cam//,/_}.png" MSL_FRAMES=3 MSL_CAM="$cam" timeout 30 ./matter_surface_lib >/dev/null 2>&1
done
cd ..
```
Read PNGs; confirm: gaps between the forks show background (porosity), silhouette is correct from every angle, no fragmentation.

- [ ] **Step 3: Record outcome** — if artifacts appear, note them and loop back to Task 6 (voxelization) or Task 12 (march). Success bar (per the user): correct silhouette + visible porosity; minor shading artifacts acceptable.

- [ ] **Step 4: Commit any demo-part code**

```bash
git add MatterSurfaceLib/main.cpp
git commit -m "test: non-convex voxel imposter demo part + visual validation"
```

---

## Task 15: Cleanup — delete dead chart-cage path

Only after Task 14 passes. Salvaged code already lives in MeshChartingLib (Task 1).

**Files:**
- Modify: `MatterSurfaceLib/include/imposter_asset.h`, `src/imposter_asset.cpp`
- Delete: `MatterSurfaceLib/src/imposter_bake.cpp` (and its shader `imposter_bake.fs` + processed variant if unused)
- Modify: `MatterSurfaceLib/shaders/bvh_tlas_common.glsl` (remove relief-march code + 2D samplers)
- Modify: `MatterSurfaceLib/main.cpp` (remove 2D atlas texture creation/binding + old members)
- Modify: `MatterSurfaceLib/Makefile`, `tests/Makefile`, `build-all.sh`

- [ ] **Step 1: Delete chart-cage CPU code**

Remove from `imposter_asset.{h,cpp}`: `build_cage`, `bake_displacement_cpu`, `dilate_atlas`, `pack_cage_uvs_bvh_order`, `pack_cage_tri_data`, `cage_to_tris`, `build_adjacency`, `segment_charts`, `plane_basis`, `pack_charts`, the chart fields on `ImposterAsset` (`tri_chart`, etc.), and `compute_imp_hash`/`save`/`load`/`bake_imposter` if no longer referenced. If the whole `imposter_asset` unit is now dead, delete both files and drop `src/imposter_asset.cpp` from `Makefile` SRC and from `tests/Makefile`.

- [ ] **Step 2: Delete the GPU radiance bake**

`git rm MatterSurfaceLib/src/imposter_bake.cpp` and remove from `Makefile` SRC. Move `flatten_part_triangles` (if anything else still needs the non-material version) — but Task 5's `flatten_part_triangles_mat` in `voxel_imposter.cpp` is now the canonical flatten, so just delete the old one. Remove `imposter_bake.fs`/`imposter_bake_processed.fs` and the `make shaders` rule for them if unused.

- [ ] **Step 3: Delete relief-march shader code**

In `bvh_tlas_common.glsl` remove `reliefMarch`, `projectInTri`, `neighborForExit`, `fetchCageTri`, the `LIN/BIN/MAXHOP` constants, and the 2D samplers `imposterColorTex/imposterDispTex/imposterTriUvTex/imposterCageTriTex/imposterTriIdTex` + related uniforms (`imposterTriBase`, `imposterMaxDisp`, `imposterTriCount`, `imposterDbg`).

- [ ] **Step 4: Delete 2D texture wiring in main.cpp**

Remove the 2D atlas `LoadTextureFromImage` blocks and their `SetShaderValueTexture` binds + the `imposter_*_tex_` members.

- [ ] **Step 5: Move/delete tests**

Delete `tests/imposter_asset_tests.cpp` (its salvaged subset already passes under MeshChartingLib). Remove its target from `tests/Makefile` and from the `build-all.sh` suite loop.

- [ ] **Step 6: Clean-rebuild both targets** (per the clean-rebuild-Windows memory: struct/header churn → clear objects)

Run:
```bash
make -C MatterSurfaceLib clean && make WSL_LINUX=1 -C MatterSurfaceLib shaders && make WSL_LINUX=1 -C MatterSurfaceLib -j4 2>&1 | tail -5
make -C MatterSurfaceLib clean && make -C MatterSurfaceLib shaders && make -C MatterSurfaceLib -j4 2>&1 | tail -5
```
Expected: both the Linux (`matter_surface_lib`) and Windows (`.exe`) targets build clean.

- [ ] **Step 7: Run full test suite**

Run: `./build-all.sh test 2>&1 | tail -30`
Expected: `mesh_charting_tests` and `voxel_imposter_tests` pass; no FAIL rows.

- [ ] **Step 8: Final headless capture sanity**

Re-run the Task 14 non-convex capture; confirm it still renders correctly after cleanup.

- [ ] **Step 9: Commit**

```bash
git add -A
git commit -m "refactor: remove dead chart-cage imposter path"
```

---

## Self-Review notes (resolved)

- **Spec coverage:** salvage lib (T1), voxel asset+bake (T2-6), serialization (T7), DDA (T8), runtime BLAS+instance (T9), 3D textures (T10), shader march (T11-12), lighting (T13), non-convex visual check (T14), cleanup (T15). All spec components mapped.
- **Albedo gap closed:** spec assumed per-triangle material albedo was available; it is not on the CPU today, so T5 adds `TriEx` retention + `flatten_part_triangles_mat` as the explicit source.
- **Type consistency:** `VoxelImposter`, `VoxGenParams`, `FlatTri`, `bake_voxels`, `dda_first_hit`, `oct_encode/decode`, `choose_grid_dims`, `tri_box_overlap`, `compute_vox_hash`, `cache_path`, `save/load` used consistently across tasks. GLSL `voxelMarch`/`octDecode` mirror the host `dda_first_hit`/`oct_decode`.
- **Verify-against-headers reminders** are inlined where the plan references engine APIs not fully quoted here (`Matrix4x4` layout, `register_triangles` overloads, exact shader scope variable names, where raylib binds shader uniforms vs. draw time).

