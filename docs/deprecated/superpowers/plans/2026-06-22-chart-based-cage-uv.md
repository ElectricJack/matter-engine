# Chart-Based Cage UV System — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace per-triangle grid packing for fitted imposters with adjacency-welded low-distortion charts (planar unwrap + bin packing) and a piecewise-affine relief march that re-anchors per cage triangle, so organic/curved imposters render without per-facet fragmentation or curvature smearing.

**Architecture:** All cage-geometry stages stay GL-free and unit-tested in `imposter_asset.cpp`: position-welded triangle adjacency → normal-cone chart segmentation → orthographic per-chart unwrap → shelf bin-packing → emit duplicated cage verts with packed UVs. The displacement bake rasterizes each cage triangle into its packed UV region and writes a per-texel cage-triangle-id. At runtime the shader marches in world space, re-projecting each sample into the current triangle's exact frame (looked up via the triangle-id atlas), bounded to the hit triangle's chart rect.

**Tech Stack:** C++14 (raylib `Mesh`/`MemAlloc`, `float3` from `bvh.h`), GLSL fragment shader (raylib OpenGL 3.3), custom `CHECK(cond,msg)` test harness in `MatterSurfaceLib/tests/imposter_asset_tests.cpp`.

**Spec:** `docs/superpowers/specs/2026-06-22-chart-based-cage-uv-design.md`

---

## File Structure

- **`MatterSurfaceLib/include/imposter_asset.h`** — add `chartConeDeg` to `ImpGenParams`, add `triid` + `tri_chart` to `ImposterAsset`, bump `kFormatVersion`, declare new functions (`build_adjacency`, `segment_charts`, `plane_basis`, `pack_charts`, `pack_cage_tri_data`, supporting structs).
- **`MatterSurfaceLib/src/imposter_asset.cpp`** — implement the new GL-free functions; restructure `build_cage` and `bake_displacement_cpu`; extend `save`/`load` and `pack_cage_uvs_bvh_order`.
- **`MatterSurfaceLib/tests/imposter_asset_tests.cpp`** — unit tests for every new/changed GL-free function.
- **`MatterSurfaceLib/main.cpp`** — build + upload `imposterCageTriTex` and `imposterTriIdTex`, add their uniforms.
- **`MatterSurfaceLib/shaders/bvh_tlas_common.glsl`** — new uniform decls; rewrite `reliefMarch` to piecewise-affine world-space stepping.

**Test build/run (every CPU task):**
```bash
cd "MatterSurfaceLib/tests" && make imposter_asset_tests && ./imposter_asset_tests
```
Passing output ends with: `All imposter_asset tests passed`

**Note on working tree:** `main.cpp` and the shaders currently hold uncommitted env-gated diagnostics (`MSL_IMP_DUMP_ATLAS`, `imposterDbg==2`). Leave them in place; stage only the specific files each task names — never `git add -A`.

---

### Task 1: Params, asset fields, format bump, serialization

**Files:**
- Modify: `MatterSurfaceLib/include/imposter_asset.h`
- Modify: `MatterSurfaceLib/src/imposter_asset.cpp` (`save`, `load`)
- Test: `MatterSurfaceLib/tests/imposter_asset_tests.cpp`

- [ ] **Step 1: Write the failing tests**

In `imposter_asset_tests.cpp`, extend `sample_params()` to set `p.maxCageTris = 4096; p.chartConeDeg = 75.0f;` (add after the existing fields). Extend `sample_asset()` to populate the new asset fields just before `return a;`:

```cpp
    a.tri_chart = { 0 };                  // one chart for the single triangle
    a.triid.assign(a.atlas_w*a.atlas_h*2, 0xFF); // default 0xFFFF per texel
    a.triid[0]=0x00; a.triid[1]=0x00;     // texel0 -> triangle 0
```

Add a new test function and call it from `main()`:

```cpp
static void test_chartcone_hash_and_new_fields() {
    using namespace imposter_asset;
    ImpGenParams a = sample_params();
    ImpGenParams b = sample_params(); b.chartConeDeg = 60.0f;
    CHECK(compute_imp_hash(a) != compute_imp_hash(b), "chartConeDeg change rehashes");

    ImposterAsset s = sample_asset();
    const char* path = "test_v2.imp";
    remove(path);
    CHECK(save(path, s, 0xABCDull), "save with new fields ok");
    ImposterAsset r;
    CHECK(load(path, 0xABCDull, s.source_part_hash, r), "load with new fields ok");
    CHECK(r.tri_chart == s.tri_chart, "tri_chart round-trips");
    CHECK(r.triid == s.triid, "triid round-trips");
    remove(path);
}
```

Add `test_chartcone_hash_and_new_fields();` to `main()` before the success print.

- [ ] **Step 2: Run to verify it fails**

Run: `cd "MatterSurfaceLib/tests" && make imposter_asset_tests`
Expected: compile FAIL — `ImpGenParams` has no `chartConeDeg`, `ImposterAsset` has no `tri_chart`/`triid`.

- [ ] **Step 3: Implement**

In `imposter_asset.h`:

```cpp
constexpr uint32_t kFormatVersion = 2u;   // was 1u: chart layout + triid + tri_chart
```

Add `chartConeDeg` as the last `ImpGenParams` field and update the guard:

```cpp
struct ImpGenParams {
    float    cageRatio;
    int      atlasW, atlasH;
    float    inflation;
    int      dispBits;
    uint32_t seed;
    int      maxCageTris;
    float    chartConeDeg;    // chart normal-cone half-angle in degrees (must be < 90)
};
static_assert(sizeof(ImpGenParams) == 32,
              "ImpGenParams must be padding-free for stable byte hashing");
```

Add to `ImposterAsset` (after `color`):

```cpp
    std::vector<uint32_t> tri_chart;   // one chart id per cage triangle (size == tris.size())
    std::vector<uint8_t>  triid;       // atlas_w*atlas_h*2: little-endian uint16 per texel, 0xFFFF = uncovered
```

In `save()`, after the `color` block (before `content_hash`):

```cpp
    put<uint32_t>(body, static_cast<uint32_t>(a.tri_chart.size()));
    put_bytes(body, a.tri_chart.data(), a.tri_chart.size()*sizeof(uint32_t));
    put<uint32_t>(body, static_cast<uint32_t>(a.triid.size()));
    put_bytes(body, a.triid.data(), a.triid.size());
```

In `load()`, after the `color` read block (`out.color.assign(...)`):

```cpp
    const uint32_t tcc = r.get<uint32_t>();
    if (!r.ok) return false;
    const uint8_t* tcp = r.take(tcc*sizeof(uint32_t));
    if (!r.ok) return false;
    out.tri_chart.resize(tcc); std::memcpy(out.tri_chart.data(), tcp, tcc*sizeof(uint32_t));
    const uint32_t idc = r.get<uint32_t>();
    if (!r.ok) return false;
    const uint8_t* idp = r.take(idc);
    if (!r.ok) return false;
    out.triid.assign(idp, idp+idc);
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd "MatterSurfaceLib/tests" && make imposter_asset_tests && ./imposter_asset_tests`
Expected: `All imposter_asset tests passed`

- [ ] **Step 5: Commit**

```bash
git add MatterSurfaceLib/include/imposter_asset.h MatterSurfaceLib/src/imposter_asset.cpp MatterSurfaceLib/tests/imposter_asset_tests.cpp
git commit -m "feat: add chartConeDeg param + tri_chart/triid asset fields (format v2)"
```

---

### Task 2: `build_adjacency` (position-welded triangle adjacency)

**Files:**
- Modify: `MatterSurfaceLib/include/imposter_asset.h`
- Modify: `MatterSurfaceLib/src/imposter_asset.cpp`
- Test: `MatterSurfaceLib/tests/imposter_asset_tests.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
static void test_build_adjacency() {
    using namespace imposter_asset;
    // Two triangles sharing edge (1,2), with the shared corners given as
    // bit-identical DUPLICATE vertices (mimics the cube cage branch).
    // tri0: A(0,0,0) B(1,0,0) C(0,1,0)
    // tri1: B'(1,0,0) D(1,1,0) C'(0,1,0)
    float pos[6*3] = {
        0,0,0,  1,0,0,  0,1,0,     // tri0 corners 0,1,2
        1,0,0,  1,1,0,  0,1,0,     // tri1 corners 3,4,5 (3==1, 5==2 by position)
    };
    unsigned short idx[6] = {0,1,2, 3,4,5};
    auto adj = build_adjacency(pos, idx, 2);
    CHECK(adj.size()==2, "adjacency size == triCount");
    // tri0 edge (B,C) = corner pair (1,2) = its 2nd edge slot -> neighbor tri1
    bool tri0_has1 = (adj[0].nbr[0]==1 || adj[0].nbr[1]==1 || adj[0].nbr[2]==1);
    bool tri1_has0 = (adj[1].nbr[0]==0 || adj[1].nbr[1]==0 || adj[1].nbr[2]==0);
    CHECK(tri0_has1, "tri0 sees tri1 across shared edge");
    CHECK(tri1_has0, "tri1 sees tri0 across shared edge");
    // The non-shared edges are boundaries (-1).
    int bcount0=0; for(int e=0;e<3;++e) if(adj[0].nbr[e]==-1) ++bcount0;
    CHECK(bcount0==2, "tri0 has two boundary edges");
}
```

Add `test_build_adjacency();` to `main()`.

- [ ] **Step 2: Run to verify it fails**

Run: `cd "MatterSurfaceLib/tests" && make imposter_asset_tests`
Expected: compile FAIL — `build_adjacency` undeclared.

- [ ] **Step 3: Implement**

In `imposter_asset.h` (in the `// --- CPU geometry` section), add includes guard note and declarations:

```cpp
// Per-triangle neighbor across edge slots (i0,i1)=0, (i1,i2)=1, (i2,i0)=2; -1 = boundary.
struct TriAdj { int nbr[3]; };

// Build triangle adjacency. Vertices are welded by EXACT position first (the cube
// cage emits bit-identical duplicate corners; simplify_mesh shares by index), so an
// edge shared by two triangles is detected regardless of index duplication. GL-free.
std::vector<TriAdj> build_adjacency(const float* positions, const unsigned short* indices,
                                    int triCount);
```

In `imposter_asset.cpp`, add includes near the top (after `<vector>`):

```cpp
#include <map>
#include <array>
```

Implement (place after the `norm3` helpers, before `build_cage`):

```cpp
std::vector<TriAdj> build_adjacency(const float* positions, const unsigned short* indices,
                                    int triCount) {
    // Weld corners by exact position -> welded vertex id.
    std::map<std::array<float,3>,int> weld;
    auto wid = [&](int corner)->int {
        int vi = indices[corner];
        std::array<float,3> k{ positions[vi*3+0], positions[vi*3+1], positions[vi*3+2] };
        auto it = weld.find(k);
        if (it != weld.end()) return it->second;
        int id = (int)weld.size(); weld.emplace(k, id); return id;
    };

    std::vector<TriAdj> adj(triCount);
    for (auto& a : adj) { a.nbr[0]=a.nbr[1]=a.nbr[2]=-1; }

    // edge (sorted welded id pair) -> first (tri, edgeSlot) that claimed it.
    std::map<std::pair<int,int>, std::pair<int,int>> seen;
    for (int t=0;t<triCount;++t) {
        int w[3] = { wid(t*3+0), wid(t*3+1), wid(t*3+2) };
        for (int e=0;e<3;++e) {
            int a=w[e], b=w[(e+1)%3];
            std::pair<int,int> key = (a<b) ? std::make_pair(a,b) : std::make_pair(b,a);
            auto it = seen.find(key);
            if (it == seen.end()) {
                seen.emplace(key, std::make_pair(t,e));
            } else {
                int ot = it->second.first, oe = it->second.second;
                adj[t].nbr[e]  = ot;
                adj[ot].nbr[oe] = t;
            }
        }
    }
    return adj;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd "MatterSurfaceLib/tests" && make imposter_asset_tests && ./imposter_asset_tests`
Expected: `All imposter_asset tests passed`

- [ ] **Step 5: Commit**

```bash
git add MatterSurfaceLib/include/imposter_asset.h MatterSurfaceLib/src/imposter_asset.cpp MatterSurfaceLib/tests/imposter_asset_tests.cpp
git commit -m "feat: position-welded triangle adjacency for cage charts"
```

---

### Task 3: `segment_charts` (normal-cone region growing)

**Files:**
- Modify: `MatterSurfaceLib/include/imposter_asset.h`
- Modify: `MatterSurfaceLib/src/imposter_asset.cpp`
- Test: `MatterSurfaceLib/tests/imposter_asset_tests.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
static void test_segment_charts() {
    using namespace imposter_asset;
    // A flat 2-triangle quad in z=0 -> one chart (normals identical).
    {
        float pos[6*3] = { 0,0,0, 1,0,0, 0,1,0,  1,0,0, 1,1,0, 0,1,0 };
        unsigned short idx[6] = {0,1,2, 3,4,5};
        auto adj = build_adjacency(pos, idx, 2);
        int nCharts=0;
        auto cid = segment_charts(pos, idx, 2, adj, 75.0f, nCharts);
        CHECK(nCharts==1, "flat quad -> 1 chart");
        CHECK(cid[0]==cid[1], "both tris in same chart");
    }
    // Axis-aligned unit cube (12 tris, shared corners) @ cone 75 -> 6 charts.
    {
        float c[8][3] = {
            {0,0,0},{1,0,0},{1,1,0},{0,1,0},
            {0,0,1},{1,0,1},{1,1,1},{0,1,1},
        };
        int F[6][4] = { {1,2,6,5},{0,4,7,3},{3,7,6,2},{0,1,5,4},{4,5,6,7},{0,3,2,1} };
        std::vector<float> pos; std::vector<unsigned short> idx;
        auto push=[&](int v){ pos.push_back(c[v][0]);pos.push_back(c[v][1]);pos.push_back(c[v][2]);
                              idx.push_back((unsigned short)(idx.size())); };
        for (int f=0;f<6;++f){ int a=F[f][0],b=F[f][1],d=F[f][2],e=F[f][3];
            push(a);push(b);push(d); push(a);push(d);push(e); }
        auto adj = build_adjacency(pos.data(), idx.data(), 12);
        int nCharts=0;
        auto cid = segment_charts(pos.data(), idx.data(), 12, adj, 75.0f, nCharts);
        CHECK(nCharts==6, "cube @ cone75 -> 6 charts");
        CHECK(cid[0]==cid[1], "two tris of a face share a chart");
    }
}
```

Add `test_segment_charts();` to `main()`.

- [ ] **Step 2: Run to verify it fails**

Run: `cd "MatterSurfaceLib/tests" && make imposter_asset_tests`
Expected: compile FAIL — `segment_charts` undeclared.

- [ ] **Step 3: Implement**

Declaration in `imposter_asset.h`:

```cpp
// Region-grow charts: a triangle joins a chart iff its outward face normal is within
// `coneDeg` of the chart's running average normal. coneDeg must be < 90 so each chart
// stays a function over its average plane (no projection fold). Returns per-triangle
// chart id [0..nCharts). Face normals are oriented outward via the mesh centroid. GL-free.
std::vector<int> segment_charts(const float* positions, const unsigned short* indices,
                                int triCount, const std::vector<TriAdj>& adj,
                                float coneDeg, int& nCharts);
```

Implementation in `imposter_asset.cpp` (after `build_adjacency`):

```cpp
std::vector<int> segment_charts(const float* positions, const unsigned short* indices,
                                int triCount, const std::vector<TriAdj>& adj,
                                float coneDeg, int& nCharts) {
    auto vpos = [&](int corner){ int vi=indices[corner];
        return make_float3(positions[vi*3+0],positions[vi*3+1],positions[vi*3+2]); };

    // Mesh centroid for outward orientation.
    float3 centroid = make_float3(0,0,0);
    for (int t=0;t<triCount;++t) for (int k=0;k<3;++k){ float3 p=vpos(t*3+k);
        centroid=make_float3(centroid.x+p.x,centroid.y+p.y,centroid.z+p.z); }
    float invn = (triCount>0) ? 1.0f/(float)(triCount*3) : 0.0f;
    centroid=make_float3(centroid.x*invn,centroid.y*invn,centroid.z*invn);

    // Outward per-face normals.
    std::vector<float3> fn(triCount);
    for (int t=0;t<triCount;++t){
        float3 p0=vpos(t*3+0),p1=vpos(t*3+1),p2=vpos(t*3+2);
        float3 n=cross3(sub3(p1,p0),sub3(p2,p0));
        float3 fc=make_float3((p0.x+p1.x+p2.x)/3-centroid.x,
                              (p0.y+p1.y+p2.y)/3-centroid.y,
                              (p0.z+p1.z+p2.z)/3-centroid.z);
        if (n.x*fc.x+n.y*fc.y+n.z*fc.z < 0.0f) n=make_float3(-n.x,-n.y,-n.z);
        fn[t]=norm3(n);
    }

    const float coneCos = cosf(coneDeg * 3.14159265358979f / 180.0f);
    std::vector<int> cid(triCount, -1);
    nCharts = 0;
    std::vector<int> stack;
    for (int seed=0; seed<triCount; ++seed) {
        if (cid[seed] != -1) continue;
        int c = nCharts++;
        cid[seed] = c;
        float3 sumN = fn[seed];               // running (unnormalized) chart normal
        stack.clear(); stack.push_back(seed);
        while (!stack.empty()) {
            int t = stack.back(); stack.pop_back();
            float3 avg = norm3(sumN);
            for (int e=0;e<3;++e) {
                int nb = adj[t].nbr[e];
                if (nb < 0 || cid[nb] != -1) continue;
                if (fn[nb].x*avg.x + fn[nb].y*avg.y + fn[nb].z*avg.z >= coneCos) {
                    cid[nb] = c;
                    sumN = make_float3(sumN.x+fn[nb].x, sumN.y+fn[nb].y, sumN.z+fn[nb].z);
                    stack.push_back(nb);
                }
            }
        }
    }
    return cid;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd "MatterSurfaceLib/tests" && make imposter_asset_tests && ./imposter_asset_tests`
Expected: `All imposter_asset tests passed`

- [ ] **Step 5: Commit**

```bash
git add MatterSurfaceLib/include/imposter_asset.h MatterSurfaceLib/src/imposter_asset.cpp MatterSurfaceLib/tests/imposter_asset_tests.cpp
git commit -m "feat: normal-cone chart segmentation for cage triangles"
```

---

### Task 4: `plane_basis` (orthographic projection basis)

**Files:**
- Modify: `MatterSurfaceLib/include/imposter_asset.h`
- Modify: `MatterSurfaceLib/src/imposter_asset.cpp`
- Test: `MatterSurfaceLib/tests/imposter_asset_tests.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
static void test_plane_basis() {
    using namespace imposter_asset;
    auto check_basis = [](float nx,float ny,float nz){
        float n[3]={nx,ny,nz}, T[3],B[3];
        plane_basis(n, T, B);
        float lt=sqrtf(T[0]*T[0]+T[1]*T[1]+T[2]*T[2]);
        float lb=sqrtf(B[0]*B[0]+B[1]*B[1]+B[2]*B[2]);
        CHECK(fabsf(lt-1.0f)<1e-4f && fabsf(lb-1.0f)<1e-4f, "basis vectors unit length");
        float tn=T[0]*nx+T[1]*ny+T[2]*nz, bn=B[0]*nx+B[1]*ny+B[2]*nz;
        float tb=T[0]*B[0]+T[1]*B[1]+T[2]*B[2];
        CHECK(fabsf(tn)<1e-4f && fabsf(bn)<1e-4f, "basis perpendicular to normal");
        CHECK(fabsf(tb)<1e-4f, "basis vectors orthogonal");
    };
    check_basis(0,0,1);   // z-up: must not collapse against the up-vector
    check_basis(1,0,0);
    check_basis(0.577f,0.577f,0.577f);

    // A flat triangle in z=0 projects with zero distortion: projected edge lengths
    // equal 3D edge lengths.
    float n[3]={0,0,1}, T[3],B[3]; plane_basis(n,T,B);
    float3 p0=make_float3(0,0,0), p1=make_float3(2,0,0), p2=make_float3(0,3,0);
    auto proj=[&](float3 p){ return make_float3(p.x*T[0]+p.y*T[1]+p.z*T[2],
                                                p.x*B[0]+p.y*B[1]+p.z*B[2], 0); };
    float3 q0=proj(p0),q1=proj(p1),q2=proj(p2);
    float e01=sqrtf((q1.x-q0.x)*(q1.x-q0.x)+(q1.y-q0.y)*(q1.y-q0.y));
    float e02=sqrtf((q2.x-q0.x)*(q2.x-q0.x)+(q2.y-q0.y)*(q2.y-q0.y));
    CHECK(fabsf(e01-2.0f)<1e-4f && fabsf(e02-3.0f)<1e-4f, "flat projection is isometric");
}
```

Add `test_plane_basis();` to `main()`.

- [ ] **Step 2: Run to verify it fails**

Run: `cd "MatterSurfaceLib/tests" && make imposter_asset_tests`
Expected: compile FAIL — `plane_basis` undeclared.

- [ ] **Step 3: Implement**

Declaration in `imposter_asset.h`:

```cpp
// Orthonormal basis (T,B) spanning the plane with normal n. Robust when n aligns
// with the chosen up-axis. GL-free.
void plane_basis(const float n[3], float T[3], float B[3]);
```

Implementation in `imposter_asset.cpp` (after `segment_charts`):

```cpp
void plane_basis(const float n[3], float T[3], float B[3]) {
    float3 N = norm3(make_float3(n[0],n[1],n[2]));
    float3 up = (fabsf(N.z) < 0.9f) ? make_float3(0,0,1) : make_float3(1,0,0);
    float3 t = norm3(cross3(up, N));
    float3 b = cross3(N, t);     // already unit (N,t orthonormal)
    T[0]=t.x; T[1]=t.y; T[2]=t.z;
    B[0]=b.x; B[1]=b.y; B[2]=b.z;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd "MatterSurfaceLib/tests" && make imposter_asset_tests && ./imposter_asset_tests`
Expected: `All imposter_asset tests passed`

- [ ] **Step 5: Commit**

```bash
git add MatterSurfaceLib/include/imposter_asset.h MatterSurfaceLib/src/imposter_asset.cpp MatterSurfaceLib/tests/imposter_asset_tests.cpp
git commit -m "feat: orthonormal plane basis for chart projection"
```

---

### Task 5: `pack_charts` (shelf bin packer)

**Files:**
- Modify: `MatterSurfaceLib/include/imposter_asset.h`
- Modify: `MatterSurfaceLib/src/imposter_asset.cpp`
- Test: `MatterSurfaceLib/tests/imposter_asset_tests.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
static void test_pack_charts() {
    using namespace imposter_asset;
    auto no_overlap_in_atlas = [](const std::vector<ChartRect>& cs, int W,int H,int pad){
        float scale=0; std::vector<ChartPlacement> pl;
        bool ok = pack_charts(cs, W, H, pad, scale, pl);
        CHECK(ok, "pack succeeds");
        CHECK(pl.size()==cs.size(), "one placement per chart");
        // Build texel rects and check bounds + pairwise non-overlap.
        struct R{int x0,y0,x1,y1;};
        std::vector<R> rs;
        for (size_t i=0;i<cs.size();++i){
            int w=(int)ceilf(cs[i].w*scale)+2*pad, h=(int)ceilf(cs[i].h*scale)+2*pad;
            R r{pl[i].ox,pl[i].oy,pl[i].ox+w,pl[i].oy+h}; rs.push_back(r);
            CHECK(r.x0>=0&&r.y0>=0&&r.x1<=W&&r.y1<=H, "rect within atlas");
        }
        for (size_t i=0;i<rs.size();++i) for (size_t j=i+1;j<rs.size();++j){
            bool disjoint = rs[i].x1<=rs[j].x0||rs[j].x1<=rs[i].x0||
                            rs[i].y1<=rs[j].y0||rs[j].y1<=rs[i].y0;
            CHECK(disjoint, "rects do not overlap");
        }
    };
    std::vector<ChartRect> few = { {0,0,1,1},{0,0,2,1},{0,0,1,3},{0,0,0.5f,0.5f} };
    no_overlap_in_atlas(few, 256, 256, 2);
    // Over-budget set still fits after auto rescale.
    std::vector<ChartRect> many;
    for (int i=0;i<400;++i) many.push_back({0,0,1.0f,1.0f});
    no_overlap_in_atlas(many, 256, 256, 1);
}
```

Add `test_pack_charts();` to `main()`.

- [ ] **Step 2: Run to verify it fails**

Run: `cd "MatterSurfaceLib/tests" && make imposter_asset_tests`
Expected: compile FAIL — `pack_charts`/`ChartRect`/`ChartPlacement` undeclared.

- [ ] **Step 3: Implement**

Declarations in `imposter_asset.h`:

```cpp
struct ChartRect  { float minU, minV, w, h; };  // projected-space bbox (world units)
struct ChartPlacement { int ox, oy; };           // texel offset of the chart's padded rect

// Shelf-pack chart rects into an atlasW x atlasH texel grid with `pad` texels of gutter
// around each chart. Picks a uniform world->texel `scale` from total area, shrinking and
// retrying on overflow. Returns false if charts cannot fit. `placements` is filled in the
// same order as `charts`. GL-free.
bool pack_charts(const std::vector<ChartRect>& charts, int atlasW, int atlasH, int pad,
                 float& scale, std::vector<ChartPlacement>& placements);
```

Implementation in `imposter_asset.cpp` (after `plane_basis`):

```cpp
static bool shelf_pack(const std::vector<ChartRect>& charts, int atlasW, int atlasH,
                       int pad, float scale, std::vector<ChartPlacement>& out) {
    const int n = (int)charts.size();
    out.assign(n, ChartPlacement{0,0});
    // Pack tallest-first for tighter shelves; remember original indices.
    std::vector<int> order(n); for (int i=0;i<n;++i) order[i]=i;
    std::sort(order.begin(), order.end(), [&](int a,int b){
        return charts[a].h > charts[b].h; });
    int cursorX=0, shelfY=0, shelfH=0;
    for (int oi=0; oi<n; ++oi) {
        int i = order[oi];
        int w = (int)ceilf(charts[i].w*scale)+2*pad;
        int h = (int)ceilf(charts[i].h*scale)+2*pad;
        if (w>atlasW || h>atlasH) return false;
        if (cursorX + w > atlasW) { shelfY += shelfH; cursorX = 0; shelfH = 0; }
        if (shelfY + h > atlasH) return false;
        out[i].ox = cursorX; out[i].oy = shelfY;
        cursorX += w; if (h>shelfH) shelfH = h;
    }
    return true;
}

bool pack_charts(const std::vector<ChartRect>& charts, int atlasW, int atlasH, int pad,
                 float& scale, std::vector<ChartPlacement>& placements) {
    if (charts.empty() || atlasW<=0 || atlasH<=0) return false;
    double area = 0.0;
    for (const auto& c : charts) area += (double)std::max(c.w,1e-6f) * std::max(c.h,1e-6f);
    if (area <= 0.0) return false;
    // Initial guess assumes 55% fill; iterate down if packing overflows.
    scale = (float)sqrt(0.55 * (double)atlasW * (double)atlasH / area);
    for (int attempt=0; attempt<24; ++attempt) {
        if (shelf_pack(charts, atlasW, atlasH, pad, scale, placements)) return true;
        scale *= 0.85f;
    }
    return false;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd "MatterSurfaceLib/tests" && make imposter_asset_tests && ./imposter_asset_tests`
Expected: `All imposter_asset tests passed`

- [ ] **Step 5: Commit**

```bash
git add MatterSurfaceLib/include/imposter_asset.h MatterSurfaceLib/src/imposter_asset.cpp MatterSurfaceLib/tests/imposter_asset_tests.cpp
git commit -m "feat: shelf bin packer for cage charts"
```

---

### Task 6: Restructure `build_cage` onto the chart pipeline

**Files:**
- Modify: `MatterSurfaceLib/src/imposter_asset.cpp` (`build_cage`, lines ~260-317)
- Test: `MatterSurfaceLib/tests/imposter_asset_tests.cpp` (extend `test_build_cage`)

This replaces the per-triangle grid packing (and the `quad`/`MSL_IMPOSTER_CUBE` packing special-case) with: adjacency → segmentation → per-chart projection → packing → emit. The cube branch (lines ~162-200) stays only as a *geometry source* (it still builds the indexed `cage` + `vn`); everything downstream is now unified.

- [ ] **Step 1: Write/extend the failing test**

Add a new assertion block. First inspect the existing `test_build_cage` to see its inputs; then append this function and call it from `main()`:

```cpp
static void test_build_cage_charts() {
    using namespace imposter_asset;
    // A simple closed tetrahedron-ish part: 4 triangles. Use distinct face normals.
    std::vector<Tri> part(4);
    auto T=[&](int i,float3 a,float3 b,float3 c){ part[i].vertex0=a;part[i].vertex1=b;part[i].vertex2=c; };
    T(0, make_float3(0,0,0), make_float3(1,0,0), make_float3(0,1,0));
    T(1, make_float3(0,0,0), make_float3(0,1,0), make_float3(0,0,1));
    T(2, make_float3(0,0,0), make_float3(0,0,1), make_float3(1,0,0));
    T(3, make_float3(1,0,0), make_float3(0,1,0), make_float3(0,0,1));
    ImpGenParams p{}; p.cageRatio=1.0f; p.atlasW=128; p.atlasH=128;
    p.inflation=0.02f; p.dispBits=16; p.seed=1u; p.maxCageTris=4096; p.chartConeDeg=75.0f;
    ImposterAsset a;
    CHECK(build_cage(part, p, 0x1ull, a), "build_cage succeeds");
    CHECK(a.tri_chart.size()==a.tris.size(), "tri_chart sized per triangle");
    CHECK(a.verts.size()==a.tris.size()*3, "3 emitted verts per triangle");
    int maxc=-1; for (uint32_t c:a.tri_chart) maxc=std::max(maxc,(int)c);
    CHECK(maxc>=0 && maxc < (int)a.tris.size(), "chart ids in range");
    bool uv_ok=true, finite=true;
    for (const auto& v:a.verts){
        if (v.u<0.0f||v.u>1.0f||v.v<0.0f||v.v>1.0f) uv_ok=false;
        if (!(v.u==v.u)||!(v.v==v.v)) finite=false;
    }
    CHECK(uv_ok, "all UVs within [0,1]");
    CHECK(finite, "no NaN UVs");
}
```

Add `test_build_cage_charts();` to `main()`.

- [ ] **Step 2: Run to verify it fails**

Run: `cd "MatterSurfaceLib/tests" && make imposter_asset_tests && ./imposter_asset_tests`
Expected: FAIL — `a.tri_chart` empty (old `build_cage` never sets it).

- [ ] **Step 3: Implement**

Replace the body of `build_cage` from the comment `// Atlas packing grid.` (line ~260) down to the `MemFree(cage.vertices); ...; return true;` (line ~315), with the chart pipeline below. Keep everything ABOVE line 260 (the cage construction + `vn` smoothed-normal computation) unchanged.

```cpp
    // ---- Chart pipeline (replaces per-triangle grid packing) ----
    const int nt = cage.triangleCount;

    std::vector<TriAdj> adj = build_adjacency(cage.vertices, cage.indices, nt);
    int nCharts = 0;
    std::vector<int> chartOf = segment_charts(cage.vertices, cage.indices, nt, adj,
                                              p.chartConeDeg, nCharts);

    // Inflated emitted position per (triangle, corner) using the smoothed normal `vn`.
    auto inflated = [&](int t,int k)->float3 {
        int vi = cage.indices[t*3+k];
        float3 pos = getv(vi), n = vn[vi];
        return make_float3(pos.x+n.x*p.inflation, pos.y+n.y*p.inflation, pos.z+n.z*p.inflation);
    };

    // Per-chart outward average normal (re-accumulated from oriented face normals) and
    // centroid of inflated corners.
    float3 meshC = make_float3(0,0,0);
    for (int t=0;t<nt;++t) for (int k=0;k<3;++k){ float3 q=inflated(t,k);
        meshC=make_float3(meshC.x+q.x,meshC.y+q.y,meshC.z+q.z); }
    if (nt>0){ float in=1.0f/(float)(nt*3); meshC=make_float3(meshC.x*in,meshC.y*in,meshC.z*in); }

    std::vector<float3> chartSumN(nCharts, make_float3(0,0,0));
    std::vector<float3> chartCsum(nCharts, make_float3(0,0,0));
    std::vector<int>    chartCcnt(nCharts, 0);
    for (int t=0;t<nt;++t){
        float3 p0=inflated(t,0),p1=inflated(t,1),p2=inflated(t,2);
        float3 fnv=cross3(sub3(p1,p0),sub3(p2,p0));
        float3 fc=make_float3((p0.x+p1.x+p2.x)/3-meshC.x,(p0.y+p1.y+p2.y)/3-meshC.y,(p0.z+p1.z+p2.z)/3-meshC.z);
        if (fnv.x*fc.x+fnv.y*fc.y+fnv.z*fc.z<0.0f) fnv=make_float3(-fnv.x,-fnv.y,-fnv.z);
        fnv=norm3(fnv);
        int c=chartOf[t];
        chartSumN[c]=make_float3(chartSumN[c].x+fnv.x,chartSumN[c].y+fnv.y,chartSumN[c].z+fnv.z);
        for (int k=0;k<3;++k){ float3 q=inflated(t,k);
            chartCsum[c]=make_float3(chartCsum[c].x+q.x,chartCsum[c].y+q.y,chartCsum[c].z+q.z);
            chartCcnt[c]++; }
    }

    // Per-chart basis + centroid.
    std::vector<float3> chartT(nCharts), chartB(nCharts), chartO(nCharts);
    for (int c=0;c<nCharts;++c){
        float3 N=norm3(chartSumN[c]); float nn[3]={N.x,N.y,N.z}, T[3],B[3];
        plane_basis(nn,T,B);
        chartT[c]=make_float3(T[0],T[1],T[2]); chartB[c]=make_float3(B[0],B[1],B[2]);
        float inv = chartCcnt[c]>0 ? 1.0f/(float)chartCcnt[c] : 0.0f;
        chartO[c]=make_float3(chartCsum[c].x*inv,chartCsum[c].y*inv,chartCsum[c].z*inv);
    }

    // Pass 1: project every corner into its chart's plane; track per-chart 2D bbox.
    std::vector<float> pu((size_t)nt*3), pv((size_t)nt*3);
    std::vector<float> cMinU(nCharts,1e30f), cMinV(nCharts,1e30f),
                       cMaxU(nCharts,-1e30f), cMaxV(nCharts,-1e30f);
    for (int t=0;t<nt;++t){ int c=chartOf[t];
        for (int k=0;k<3;++k){
            float3 q=inflated(t,k), d=sub3(q,chartO[c]);
            float u=d.x*chartT[c].x+d.y*chartT[c].y+d.z*chartT[c].z;
            float v=d.x*chartB[c].x+d.y*chartB[c].y+d.z*chartB[c].z;
            pu[t*3+k]=u; pv[t*3+k]=v;
            cMinU[c]=fminf(cMinU[c],u); cMinV[c]=fminf(cMinV[c],v);
            cMaxU[c]=fmaxf(cMaxU[c],u); cMaxV[c]=fmaxf(cMaxV[c],v);
        }
    }

    // Pack chart rects.
    std::vector<ChartRect> rects(nCharts);
    for (int c=0;c<nCharts;++c){
        float w=fmaxf(cMaxU[c]-cMinU[c],1e-5f), h=fmaxf(cMaxV[c]-cMinV[c],1e-5f);
        rects[c]=ChartRect{cMinU[c],cMinV[c],w,h};
    }
    const int pad=2;
    float scale=1.0f; std::vector<ChartPlacement> placements;
    if (!pack_charts(rects, p.atlasW, p.atlasH, pad, scale, placements)) {
        MemFree(cage.vertices); MemFree(cage.indices); return false;
    }

    // ---- Emit ----
    out = ImposterAsset{};
    out.source_part_hash = source_part_hash;
    out.atlas_w=(uint32_t)p.atlasW; out.atlas_h=(uint32_t)p.atlasH;
    out.disp_bits=p.dispBits; out.max_disp=p.inflation;
    out.verts.reserve(nt*3); out.tris.reserve(nt); out.tri_chart.reserve(nt);

    const float aw=(float)p.atlasW, ah=(float)p.atlasH;
    float bmin[3]={1e30f,1e30f,1e30f}, bmax[3]={-1e30f,-1e30f,-1e30f};
    for (int t=0;t<nt;++t){
        int c=chartOf[t];
        for (int k=0;k<3;++k){
            float3 ip=inflated(t,k); int vi=cage.indices[t*3+k]; float3 n=vn[vi];
            float u=(placements[c].ox+pad+(pu[t*3+k]-cMinU[c])*scale)/aw;
            float v=(placements[c].oy+pad+(pv[t*3+k]-cMinV[c])*scale)/ah;
            CageVert cv; cv.px=ip.x; cv.py=ip.y; cv.pz=ip.z;
            cv.nx=n.x; cv.ny=n.y; cv.nz=n.z; cv.u=u; cv.v=v;
            out.verts.push_back(cv);
            bmin[0]=fminf(bmin[0],ip.x); bmin[1]=fminf(bmin[1],ip.y); bmin[2]=fminf(bmin[2],ip.z);
            bmax[0]=fmaxf(bmax[0],ip.x); bmax[1]=fmaxf(bmax[1],ip.y); bmax[2]=fmaxf(bmax[2],ip.z);
        }
        out.tris.push_back({(uint32_t)(t*3),(uint32_t)(t*3+1),(uint32_t)(t*3+2)});
        out.tri_chart.push_back((uint32_t)c);
    }
    for (int i=0;i<3;++i){ out.bounds_min[i]=bmin[i]; out.bounds_max[i]=bmax[i]; }
    float ext=fmaxf(bmax[0]-bmin[0],fmaxf(bmax[1]-bmin[1],bmax[2]-bmin[2]));
    out.parallax_radius=ext*6.0f;

    MemFree(cage.vertices); MemFree(cage.indices);
    return true;
}
```

Note: the old code also called `MemFree(cage.normals)` — if the prior body freed it, keep that free in the cube/simplify branches as before; the snippet above frees only `vertices`/`indices` (the simplify branch already frees `cage.normals` on its error path; the success path here does not allocate/own `cage.normals` separately for the cube branch). If the compiler warns about a leaked `cage.normals`, add `if (cage.normals) MemFree(cage.normals);` before the final `return true;`.

- [ ] **Step 4: Run to verify it passes**

Run: `cd "MatterSurfaceLib/tests" && make imposter_asset_tests && ./imposter_asset_tests`
Expected: `All imposter_asset tests passed` (both the existing `test_build_cage` and the new `test_build_cage_charts`).

If the pre-existing `test_build_cage` asserted the old grid layout (e.g. a specific cell UV), update those assertions to the chart-pipeline reality: UVs in `[0,1]`, `verts.size()==tris.size()*3`, and `tri_chart` populated. Do not weaken coverage — re-express the same intent against the new layout.

- [ ] **Step 5: Commit**

```bash
git add MatterSurfaceLib/src/imposter_asset.cpp MatterSurfaceLib/tests/imposter_asset_tests.cpp
git commit -m "feat: build_cage uses adjacency/charts/projection/packing (unified cube+fitted)"
```

---

### Task 7: Restructure `bake_displacement_cpu` (triangle rasterization + triid)

**Files:**
- Modify: `MatterSurfaceLib/src/imposter_asset.cpp` (`bake_displacement_cpu`, lines ~319-403)
- Test: `MatterSurfaceLib/tests/imposter_asset_tests.cpp`

Replaces grid-cell iteration with per-triangle atlas rasterization, and writes `out.triid` (uint16 cage-triangle index per covered texel).

- [ ] **Step 1: Write the failing test**

```cpp
static void test_bake_triid_and_continuity() {
    using namespace imposter_asset;
    // Flat 2-triangle quad part facing +Z, so the cage's single chart maps the quad
    // contiguously; an interior strip should be covered by both triangles.
    std::vector<Tri> part(2);
    part[0].vertex0=make_float3(0,0,0); part[0].vertex1=make_float3(1,0,0); part[0].vertex2=make_float3(0,1,0);
    part[1].vertex0=make_float3(1,0,0); part[1].vertex1=make_float3(1,1,0); part[1].vertex2=make_float3(0,1,0);
    ImpGenParams p{}; p.cageRatio=1.0f; p.atlasW=64; p.atlasH=64;
    p.inflation=0.05f; p.dispBits=16; p.seed=1u; p.maxCageTris=4096; p.chartConeDeg=75.0f;
    ImposterAsset a;
    CHECK(build_cage(part, p, 0x1ull, a), "cage built");
    CHECK(bake_displacement_cpu(part, a), "bake ok");
    CHECK(a.triid.size()==(size_t)a.atlas_w*a.atlas_h*2, "triid sized W*H*2");
    // Every covered texel carries a valid (non-0xFFFF) triangle id; misses carry 0xFFFF.
    long covered=0, bad=0;
    for (int i=0;i<(int)a.atlas_w*(int)a.atlas_h;++i){
        uint16_t id; memcpy(&id, &a.triid[(size_t)i*2], 2);
        bool cov = a.color[i*4+3] > 127;
        if (cov){ ++covered; if (id==0xFFFF || id>=a.tris.size()) ++bad; }
        else    { if (id!=0xFFFF) ++bad; }
    }
    CHECK(covered>0, "some texels covered");
    CHECK(bad==0, "covered<->valid id and miss<->0xFFFF agree");
}
```

Add `test_bake_triid_and_continuity();` to `main()`.

- [ ] **Step 2: Run to verify it fails**

Run: `cd "MatterSurfaceLib/tests" && make imposter_asset_tests && ./imposter_asset_tests`
Expected: FAIL — `a.triid` empty (old bake never writes it).

- [ ] **Step 3: Implement**

Replace the `bake_displacement_cpu` body. Keep the BVH setup (lines ~322-327) and the `max_disp`/`MSL_IMP_SHELL` block; replace the cell loop with triangle rasterization and add triid output. Full replacement:

```cpp
bool bake_displacement_cpu(const std::vector<Tri>& part_tris, ImposterAsset& out) {
    if (part_tris.empty() || out.tris.empty() || out.atlas_w==0 || out.atlas_h==0) return false;

    BvhMesh mesh{};
    mesh.triCount=(int)part_tris.size();
    mesh.tri=(Tri*)MALLOC64(sizeof(Tri)*mesh.triCount);
    for (int i=0;i<mesh.triCount;++i) mesh.tri[i]=part_tris[i];
    BVH bvh(&mesh);

    const int W=(int)out.atlas_w, H=(int)out.atlas_h;
    const int bytes=out.disp_bits/8;
    const int nt=(int)out.tris.size();

    std::vector<float> dist((size_t)W*H, -1.0f);
    out.triid.assign((size_t)W*H*2, 0xFF);   // default uint16 0xFFFF
    auto set_id=[&](int px,uint16_t id){ memcpy(&out.triid[(size_t)px*2], &id, 2); };

    // Rasterize each cage triangle into atlas space; cast an inward ray per covered texel.
    for (int t=0;t<nt;++t){
        const CageTri& tr=out.tris[t];
        const CageVert& A=out.verts[tr.i0]; const CageVert& B=out.verts[tr.i1]; const CageVert& C=out.verts[tr.i2];
        float ax=A.u*W, ay=A.v*H, bx=B.u*W, by=B.v*H, cx=C.u*W, cy=C.v*H;
        int x0=(int)floorf(fminf(ax,fminf(bx,cx))), x1=(int)ceilf(fmaxf(ax,fmaxf(bx,cx)));
        int y0=(int)floorf(fminf(ay,fminf(by,cy))), y1=(int)ceilf(fmaxf(ay,fmaxf(by,cy)));
        if (x0<0)x0=0; if (y0<0)y0=0; if (x1>W)x1=W; if (y1>H)y1=H;
        float area=(bx-ax)*(cy-ay)-(cx-ax)*(by-ay);
        if (fabsf(area)<1e-9f) continue;
        float invArea=1.0f/area;
        for (int y=y0;y<y1;++y) for (int x=x0;x<x1;++x){
            float px=x+0.5f, py=y+0.5f;
            float wA=((bx-px)*(cy-py)-(cx-px)*(by-py))*invArea;
            float wB=((cx-px)*(ay-py)-(ax-px)*(cy-py))*invArea;
            float wC=1.0f-wA-wB;
            if (wA<0||wB<0||wC<0) continue;            // outside this triangle
            float3 pos=make_float3(wA*A.px+wB*B.px+wC*C.px,
                                   wA*A.py+wB*B.py+wC*C.py,
                                   wA*A.pz+wB*B.pz+wC*C.pz);
            float3 n=norm3(make_float3(wA*A.nx+wB*B.nx+wC*C.nx,
                                       wA*A.ny+wB*B.ny+wC*C.ny,
                                       wA*A.nz+wB*B.nz+wC*C.nz));
            float3 dir=make_float3(-n.x,-n.y,-n.z);
            BVHRay ray; ray.O=pos; ray.D=dir; ray.rD=make_float3(1.0f/dir.x,1.0f/dir.y,1.0f/dir.z);
            ray.hit.t=1e30f;
            bvh.Intersect(ray,0);
            int idx=y*W+x;
            if (ray.hit.t<1e29f && ray.hit.t>0.0f){
                dist[idx]=ray.hit.t;
                set_id(idx,(uint16_t)t);
            }
        }
    }

    // max_disp spans the full imposter depth (see design); never below inflation.
    float ext[3]={ out.bounds_max[0]-out.bounds_min[0],
                   out.bounds_max[1]-out.bounds_min[1],
                   out.bounds_max[2]-out.bounds_min[2] };
    float full_depth=fmaxf(ext[0],fmaxf(ext[1],ext[2]));
    out.max_disp=fmaxf(out.max_disp,full_depth);
    if (const char* e=std::getenv("MSL_IMP_SHELL")) out.max_disp=(float)atof(e);

    out.disp.assign((size_t)W*H*bytes,0);
    out.color.assign((size_t)W*H*4,0);
    for (int i=0;i<W*H;++i){
        float d=dist[i];
        if (d<0.0f){ out.color[i*4+3]=0; continue; }
        float nrm=d/out.max_disp; if(nrm>1.0f)nrm=1.0f; if(nrm<0.0f)nrm=0.0f;
        if (bytes==2){ uint16_t v=(uint16_t)(nrm*65535.0f+0.5f); memcpy(&out.disp[(size_t)i*2],&v,2); }
        else         { out.disp[i]=(uint8_t)(nrm*255.0f+0.5f); }
        out.color[i*4+3]=255;
    }

    FREE64(mesh.tri);
    return true;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd "MatterSurfaceLib/tests" && make imposter_asset_tests && ./imposter_asset_tests`
Expected: `All imposter_asset tests passed`. If the existing `test_displacement_reconstruction` asserted the old grid-cell layout, re-express it against the rasterized layout (coverage > 0, disp in range) — keep the intent.

- [ ] **Step 5: Commit**

```bash
git add MatterSurfaceLib/src/imposter_asset.cpp MatterSurfaceLib/tests/imposter_asset_tests.cpp
git commit -m "feat: bake displacement by triangle rasterization + per-texel triangle-id"
```

---

### Task 8: `pack_cage_tri_data` (cage-triangle data texture buffer)

**Files:**
- Modify: `MatterSurfaceLib/include/imposter_asset.h`
- Modify: `MatterSurfaceLib/src/imposter_asset.cpp`
- Test: `MatterSurfaceLib/tests/imposter_asset_tests.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
static void test_pack_cage_tri_data() {
    using namespace imposter_asset;
    ImposterAsset a;
    a.verts = { {1,2,3, 0,0,1, 0.1f,0.2f},
                {4,5,6, 0,0,1, 0.3f,0.4f},
                {7,8,9, 0,0,1, 0.5f,0.6f} };
    a.tris = { {0,1,2} };
    std::vector<float> buf = pack_cage_tri_data(a);
    CHECK(buf.size()==(size_t)1*6*4, "buffer = nTris*6*4");
    auto px=[&](int row,int tri,int c){ return buf[(size_t)(row*1 + tri)*4 + c]; };
    CHECK(px(0,0,0)==1&&px(0,0,1)==2&&px(0,0,2)==3, "row0 = corner0 pos");
    CHECK(px(2,0,0)==7&&px(2,0,1)==8&&px(2,0,2)==9, "row2 = corner2 pos");
    CHECK(px(3,0,0)==0.1f&&px(3,0,1)==0.2f, "row3 = corner0 uv");
    CHECK(px(5,0,0)==0.5f&&px(5,0,1)==0.6f, "row5 = corner2 uv");
}
```

Add `test_pack_cage_tri_data();` to `main()`.

- [ ] **Step 2: Run to verify it fails**

Run: `cd "MatterSurfaceLib/tests" && make imposter_asset_tests`
Expected: compile FAIL — `pack_cage_tri_data` undeclared.

- [ ] **Step 3: Implement**

Declaration in `imposter_asset.h` (near `pack_cage_uvs_bvh_order`):

```cpp
// Pack per-triangle cage data keyed by cage-triangle-id (native tris order; BVH-independent)
// into an RGBA32F buffer. Layout: width = nTris, height = 6. Rows 0-2 = corner positions
// (.xyz, cage space), rows 3-5 = corner UVs (.xy). float offset = (row*nTris + tri)*4.
// The shader uses this to re-anchor the tangent frame when the relief march crosses into a
// new triangle. GL-free.
std::vector<float> pack_cage_tri_data(const ImposterAsset& a);
```

Implementation in `imposter_asset.cpp` (next to `pack_cage_uvs_bvh_order`):

```cpp
std::vector<float> pack_cage_tri_data(const ImposterAsset& a) {
    const int nt=(int)a.tris.size();
    std::vector<float> buf((size_t)nt*6*4, 0.0f);
    auto setpx=[&](int row,int tri,float x,float y,float z,float w){
        size_t o=((size_t)row*nt + tri)*4; buf[o]=x; buf[o+1]=y; buf[o+2]=z; buf[o+3]=w; };
    for (int t=0;t<nt;++t){
        const CageTri& tr=a.tris[t];
        const CageVert& v0=a.verts[tr.i0]; const CageVert& v1=a.verts[tr.i1]; const CageVert& v2=a.verts[tr.i2];
        setpx(0,t, v0.px,v0.py,v0.pz, 0.0f);
        setpx(1,t, v1.px,v1.py,v1.pz, 0.0f);
        setpx(2,t, v2.px,v2.py,v2.pz, 0.0f);
        setpx(3,t, v0.u,v0.v, 0.0f, 0.0f);
        setpx(4,t, v1.u,v1.v, 0.0f, 0.0f);
        setpx(5,t, v2.u,v2.v, 0.0f, 0.0f);
    }
    return buf;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd "MatterSurfaceLib/tests" && make imposter_asset_tests && ./imposter_asset_tests`
Expected: `All imposter_asset tests passed`

- [ ] **Step 5: Commit**

```bash
git add MatterSurfaceLib/include/imposter_asset.h MatterSurfaceLib/src/imposter_asset.cpp MatterSurfaceLib/tests/imposter_asset_tests.cpp
git commit -m "feat: pack_cage_tri_data buffer for shader frame re-anchoring"
```

---

### Task 9: Extend `pack_cage_uvs_bvh_order` (chart rect + cage-tri id)

**Files:**
- Modify: `MatterSurfaceLib/src/imposter_asset.cpp` (`pack_cage_uvs_bvh_order`)
- Test: `MatterSurfaceLib/tests/imposter_asset_tests.cpp` (update `test_pack_cage_uvs_bvh_order`)

New channel map: row0 `.xy`=uv0 `.zw`=chartLo; row1 `.xy`=uv1 `.zw`=chartHi; row2 `.xy`=uv2 `.z`=cageTriId `.w`=0.

- [ ] **Step 1: Update the test (now failing on new channels)**

Replace `test_pack_cage_uvs_bvh_order` with:

```cpp
static void test_pack_cage_uvs_bvh_order() {
    using namespace imposter_asset;
    ImposterAsset a;
    a.verts.resize(6);
    for (int k=0;k<6;++k){ a.verts[k].u=(float)k; a.verts[k].v=(float)(10+k); }
    a.tris = { {0,1,2}, {3,4,5} };
    a.tri_chart = { 0, 1 };            // each triangle is its own chart
    uint32_t triIdx[2] = {1, 0};       // slot0 -> tri1, slot1 -> tri0
    std::vector<float> buf = pack_cage_uvs_bvh_order(a, triIdx, 2);
    CHECK(buf.size()==(size_t)2*3*4, "uv buffer size = nTris*3*4");
    auto at=[&](int row,int i,int c){ return buf[(size_t)(row*2 + i)*4 + c]; };
    // slot0 = tri1 = verts 3,4,5 -> uvs (3,13),(4,14),(5,15)
    CHECK(at(0,0,0)==3.0f && at(0,0,1)==13.0f, "slot0 uv0");
    CHECK(at(1,0,0)==4.0f && at(2,0,0)==5.0f, "slot0 uv1,uv2");
    // chart of tri1 spans u[3..5], v[13..15] -> chartLo/Hi in row0.zw / row1.zw
    CHECK(at(0,0,2)==3.0f && at(0,0,3)==13.0f, "slot0 chartLo");
    CHECK(at(1,0,2)==5.0f && at(1,0,3)==15.0f, "slot0 chartHi");
    CHECK(at(2,0,2)==1.0f, "slot0 cageTriId == 1");
    // slot1 = tri0 = verts 0,1,2 ; cageTriId==0
    CHECK(at(0,1,0)==0.0f && at(0,1,1)==10.0f, "slot1 uv0");
    CHECK(at(2,1,2)==0.0f, "slot1 cageTriId == 0");
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd "MatterSurfaceLib/tests" && make imposter_asset_tests && ./imposter_asset_tests`
Expected: FAIL on the new `chartLo`/`chartHi`/`cageTriId` assertions (old impl leaves `.zw`/`.z` as 0).

- [ ] **Step 3: Implement**

Replace `pack_cage_uvs_bvh_order` with:

```cpp
std::vector<float> pack_cage_uvs_bvh_order(const ImposterAsset& a,
                                           const uint32_t* triIdx, int nTris) {
    // Chart UV rects from tri_chart + emitted UVs.
    int nCharts=0; for (uint32_t c : a.tri_chart) nCharts=std::max(nCharts,(int)c+1);
    std::vector<float> lo((size_t)nCharts*2, 1e30f), hi((size_t)nCharts*2,-1e30f);
    for (int t=0;t<(int)a.tris.size();++t){
        int c=(int)a.tri_chart[t]; const CageTri& tr=a.tris[t];
        const CageVert* vs[3]={&a.verts[tr.i0],&a.verts[tr.i1],&a.verts[tr.i2]};
        for (auto* v : vs){
            lo[c*2]=fminf(lo[c*2],v->u); lo[c*2+1]=fminf(lo[c*2+1],v->v);
            hi[c*2]=fmaxf(hi[c*2],v->u); hi[c*2+1]=fmaxf(hi[c*2+1],v->v);
        }
    }
    std::vector<float> buf((size_t)nTris*3*4, 0.0f);
    auto setpx=[&](int row,int i,float x,float y,float z,float w){
        size_t o=((size_t)row*nTris + i)*4; buf[o]=x; buf[o+1]=y; buf[o+2]=z; buf[o+3]=w; };
    for (int i=0;i<nTris;++i){
        int cageTri=(int)triIdx[i]; const CageTri& tr=a.tris[cageTri];
        const CageVert& v0=a.verts[tr.i0]; const CageVert& v1=a.verts[tr.i1]; const CageVert& v2=a.verts[tr.i2];
        int c=(int)a.tri_chart[cageTri];
        setpx(0,i, v0.u,v0.v, lo[c*2],   lo[c*2+1]);     // uv0 + chartLo
        setpx(1,i, v1.u,v1.v, hi[c*2],   hi[c*2+1]);     // uv1 + chartHi
        setpx(2,i, v2.u,v2.v, (float)cageTri, 0.0f);     // uv2 + cageTriId
    }
    return buf;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd "MatterSurfaceLib/tests" && make imposter_asset_tests && ./imposter_asset_tests`
Expected: `All imposter_asset tests passed`

- [ ] **Step 5: Commit**

```bash
git add MatterSurfaceLib/src/imposter_asset.cpp MatterSurfaceLib/tests/imposter_asset_tests.cpp
git commit -m "feat: pack chart rect + cage-tri id into imposterTriUvTex channels"
```

---

### Task 10: Runtime textures + uniforms (main.cpp + shader decls)

**Files:**
- Modify: `MatterSurfaceLib/main.cpp` (imposter texture build region ~line 663; uniform uploads ~line 1363; members ~line 2160)
- Modify: `MatterSurfaceLib/shaders/bvh_tlas_common.glsl` (uniform decls ~line 16-25)

No unit test (GL upload). Validated by build + the visual task. This task only *adds* the textures/uniforms; the shader still uses the old `reliefMarch` until Task 11.

- [ ] **Step 1: Add shader uniform declarations**

In `bvh_tlas_common.glsl`, next to `uniform sampler2D imposterTriUvTex;` (line 21), add:

```glsl
uniform sampler2D imposterCageTriTex; // RGBA32F: col=cage-tri id, rows 0-2 pos.xyz, rows 3-5 uv.xy
uniform sampler2D imposterTriIdTex;   // R32F: atlas of cage-tri id per texel (-1 = uncovered)
```

- [ ] **Step 2: Build + upload the textures in main.cpp**

In the imposter texture build block (just after `imposter_triuv_tex_ = LoadTextureFromImage(...)`, ~line 670), add:

```cpp
        {
            // Cage-triangle data texture (cage-tri-id order): pos rows 0-2, uv rows 3-5.
            std::vector<float> tribuf = imposter_asset::pack_cage_tri_data(imp);
            Image tri{}; tri.data = tribuf.data();
            tri.width = (int)imp.tris.size(); tri.height = 6; tri.mipmaps = 1;
            tri.format = PIXELFORMAT_UNCOMPRESSED_R32G32B32A32;
            imposter_cagetri_tex_ = LoadTextureFromImage(tri);
            SetTextureFilter(imposter_cagetri_tex_, TEXTURE_FILTER_POINT);

            // Triangle-id atlas as R32F (-1 = uncovered) for exact point sampling.
            const int W=(int)imp.atlas_w, H=(int)imp.atlas_h;
            std::vector<float> idf((size_t)W*H);
            for (int i=0;i<W*H;++i){ uint16_t id; memcpy(&id,&imp.triid[(size_t)i*2],2);
                idf[i] = (id==0xFFFF) ? -1.0f : (float)id; }
            Image idi{}; idi.data = idf.data(); idi.width=W; idi.height=H; idi.mipmaps=1;
            idi.format = PIXELFORMAT_UNCOMPRESSED_R32;
            imposter_triid_tex_ = LoadTextureFromImage(idi);
            SetTextureFilter(imposter_triid_tex_, TEXTURE_FILTER_POINT);
        }
```

- [ ] **Step 3: Upload the new sampler uniforms**

Where the imposter samplers are bound to the shader (alongside the existing `imposterTriUvTex` / `imposterColorTex` `SetShaderValueTexture` calls, ~line 1363), add:

```cpp
        SetShaderValueTexture(shader_, GetShaderLocation(shader_, "imposterCageTriTex"), imposter_cagetri_tex_);
        SetShaderValueTexture(shader_, GetShaderLocation(shader_, "imposterTriIdTex"),  imposter_triid_tex_);
```

Match the exact binding pattern already used for `imposterTriUvTex` in that block (same shader handle, same call style). If the codebase caches shader locations in member ints, follow that pattern instead of `GetShaderLocation` per frame.

- [ ] **Step 4: Declare the member textures**

Next to `Texture2D imposter_triuv_tex_{};` (~line 2160), add:

```cpp
    Texture2D imposter_cagetri_tex_{};
    Texture2D imposter_triid_tex_{};
```

- [ ] **Step 5: Regenerate processed shaders + build**

```bash
cd "MatterSurfaceLib" && make shaders && make WSL_LINUX=1
```
Expected: compiles cleanly; both `raytrace_tlas_blas_processed.fs` and `raytrace_tlas_blas.fs` pick up the new uniforms (`make shaders` inlines includes).

- [ ] **Step 6: Commit**

```bash
git add MatterSurfaceLib/main.cpp MatterSurfaceLib/shaders/bvh_tlas_common.glsl MatterSurfaceLib/shaders/raytrace_tlas_blas.fs MatterSurfaceLib/shaders/raytrace_tlas_blas_processed.fs
git commit -m "feat: upload cage-tri data + triangle-id atlas textures for imposter march"
```

---

### Task 11: Piecewise-affine `reliefMarch` (curvature-correct world-space march)

**Files:**
- Modify: `MatterSurfaceLib/shaders/bvh_tlas_common.glsl` (`reliefMarch` ~lines 615-682; its call site ~line 814)

No unit test (GLSL). Validated by Task 12.

- [ ] **Step 1: Replace `reliefMarch` with the piecewise-affine version**

Replace the whole `reliefMarch` function (lines 615-682) with the version below. It steps in world space, re-projects each sample into the *current* triangle's exact frame, re-anchors via the triangle-id atlas, and is bounded to the hit triangle's chart rect.

```glsl
// Fetch cage triangle `id` (cage-tri-id order) world-space corners + UVs from
// imposterCageTriTex, applying the instance transform so positions match the seed frame.
void fetchCageTri(int id, mat4 xform, out vec3 p0, out vec3 p1, out vec3 p2,
                  out vec2 t0, out vec2 t1, out vec2 t2) {
    p0 = transformPosition(texelFetch(imposterCageTriTex, ivec2(id,0),0).xyz, xform);
    p1 = transformPosition(texelFetch(imposterCageTriTex, ivec2(id,1),0).xyz, xform);
    p2 = transformPosition(texelFetch(imposterCageTriTex, ivec2(id,2),0).xyz, xform);
    t0 = texelFetch(imposterCageTriTex, ivec2(id,3),0).xy;
    t1 = texelFetch(imposterCageTriTex, ivec2(id,4),0).xy;
    t2 = texelFetch(imposterCageTriTex, ivec2(id,5),0).xy;
}

// Project world point P into triangle (p0,p1,p2)/(t0,t1,t2): UV via barycentrics,
// and signed depth `pen` below the triangle plane (positive = inside the shell).
bool projectInTri(vec3 P, vec3 p0, vec3 p1, vec3 p2, vec2 t0, vec2 t1, vec2 t2,
                  out vec2 uv, out float pen) {
    vec3 e1=p1-p0, e2=p2-p0, ep=P-p0;
    float d00=dot(e1,e1), d01=dot(e1,e2), d11=dot(e2,e2), d20=dot(ep,e1), d21=dot(ep,e2);
    float den=d00*d11-d01*d01;
    if (abs(den)<1e-12) return false;
    float bv=(d11*d20-d01*d21)/den, bw=(d00*d21-d01*d20)/den, bu=1.0-bv-bw;
    uv = bu*t0 + bv*t1 + bw*t2;
    vec3 fn = normalize(cross(e1,e2));
    pen = -dot(ep, fn);            // below the plane along the outward face normal
    return true;
}

bool reliefMarch(vec3 entryPos, vec3 rayDir,
                 vec3 v0, vec3 v1, vec3 v2,
                 vec2 uv0, vec2 uv1, vec2 uv2,
                 vec3 cageN, vec2 chartLo, vec2 chartHi, int seedTriId, mat4 instXform,
                 out vec2 hitUV, out float hitS) {
    hitS = 0.0;
    vec3 ndir = normalize(rayDir);

    // Current triangle frame (starts as the hit triangle).
    int curId = seedTriId;
    vec3 p0=v0, p1=v1, p2=v2; vec2 t0=uv0, t1=uv1, t2=uv2;

    // Arc-length budget to traverse the full shell depth, using the seed inward rate.
    vec3 e1=v1-v0, e2=v2-v0;
    vec3 fn0 = normalize(cross(e1,e2));
    float inward0 = -dot(ndir, fn0);
    if (inward0 <= 1e-5) return false;        // ray not entering the shell
    float sMax = imposterMaxDisp / inward0;

    const int LIN = 128;
    const int BIN = 8;
    float ds = sMax / float(LIN);
    float prevS = 0.0;
    for (int i=1; i<=LIN; ++i) {
        float s = ds * float(i);
        vec3 P = entryPos + ndir * s;

        vec2 uvc; float pen;
        if (!projectInTri(P, p0,p1,p2, t0,t1,t2, uvc, pen)) { prevS=s; continue; }

        // Stay inside this triangle's chart rect; else terminate (option 1).
        if (uvc.x < chartLo.x-0.002 || uvc.x > chartHi.x+0.002 ||
            uvc.y < chartLo.y-0.002 || uvc.y > chartHi.y+0.002) break;

        // Re-anchor if the march moved onto a different cage triangle.
        float idf = texture(imposterTriIdTex, uvc).r;
        if (idf < 0.0) { prevS = s; continue; }    // uncovered texel: keep marching
        int sampleId = int(idf + 0.5);
        if (sampleId != curId) {
            curId = sampleId;
            fetchCageTri(curId, instXform, p0,p1,p2, t0,t1,t2);
            if (!projectInTri(P, p0,p1,p2, t0,t1,t2, uvc, pen)) { prevS=s; continue; }
        }

        float d = texture(imposterDispTex, uvc).r * imposterMaxDisp;
        float cov = texture(imposterColorTex, uvc).a;
        if (cov > 0.5 && pen - d >= 0.0) {
            // Binary refine on world arc length between prevS and s.
            float lo = prevS, hi = s;
            for (int b=0;b<BIN;++b){
                float mid=0.5*(lo+hi);
                vec3 Pm=entryPos+ndir*mid;
                vec2 um; float pm;
                if (!projectInTri(Pm, p0,p1,p2, t0,t1,t2, um, pm)) { lo=mid; continue; }
                float dm = texture(imposterDispTex, um).r * imposterMaxDisp;
                if (pm - dm >= 0.0) hi=mid; else lo=mid;
            }
            vec3 Ph=entryPos+ndir*hi;
            vec2 uh; float ph;
            projectInTri(Ph, p0,p1,p2, t0,t1,t2, uh, ph);
            hitUV = uh; hitS = hi;
            return texture(imposterColorTex, hitUV).a > 0.5;
        }
        prevS = s;
    }
    return false;
}
```

- [ ] **Step 2: Update the call site**

At the hit block (~line 794-814), fetch the chart rect + seed id from `imposterTriUvTex` and pass the instance transform. Replace the `uv0/uv1/uv2` fetch + the `reliefMarch(...)` call with:

```glsl
            vec4 r0 = texelFetch(imposterTriUvTex, ivec2(localTri, 0), 0);
            vec4 r1 = texelFetch(imposterTriUvTex, ivec2(localTri, 1), 0);
            vec4 r2 = texelFetch(imposterTriUvTex, ivec2(localTri, 2), 0);
            vec2 uv0 = r0.xy, uv1 = r1.xy, uv2 = r2.xy;
            vec2 chartLo = r0.zw, chartHi = r1.zw;
            int  seedTriId = int(r2.z + 0.5);
            vec3 ndir = normalize(rayDir);
            // (imposterDbg==2 diagnostic block stays here, using uv0/uv1/uv2)
            vec2 hitUV; float hitS;
            if (reliefMarch(result.position, ndir, w0, w1, w2, uv0, uv1, uv2,
                            cageN, chartLo, chartHi, seedTriId, inst.transform, hitUV, hitS)) {
```

Keep the existing post-hit body (advance `result.position`, sample `bakedColor`, etc.) unchanged. Remove the now-duplicate `vec2 uv0 = texelFetch(...)` lines (794-796) and the standalone `vec3 ndir = normalize(rayDir);` at line 797 if it now duplicates the one added above.

- [ ] **Step 3: Regenerate processed shaders + build**

```bash
cd "MatterSurfaceLib" && make shaders && make WSL_LINUX=1
```
Expected: compiles cleanly. (GLSL errors surface at shader-load time; if the engine logs a shader compile error on run, fix the reported line.)

- [ ] **Step 4: Commit**

```bash
git add MatterSurfaceLib/shaders/bvh_tlas_common.glsl MatterSurfaceLib/shaders/raytrace_tlas_blas.fs MatterSurfaceLib/shaders/raytrace_tlas_blas_processed.fs
git commit -m "feat: piecewise-affine re-anchoring relief march bounded to chart rect"
```

---

### Task 12: End-to-end visual validation

**Files:** none (validation only).

No unit test — this is the feature-correctness gate. The harness reaps backgrounded GUI children, so **the user launches the app**; ask them to run it and share screenshots, or use the existing headless screenshot capture.

- [ ] **Step 1: Build everything**

```bash
cd "MatterSurfaceLib" && make shaders && make WSL_LINUX=1
```
Expected: clean build.

- [ ] **Step 2: Cube regression (simplest case first)**

Ask the user to run with the cube part selected (e.g. `MSL_IMPOSTER_CUBE=1`) and confirm the imposter renders identically to the pre-change cube imposter (no fragmentation, correct color). The cube now flows through the unified chart path (6 charts), so this proves the unification didn't regress the known-good case.

Expected: cube imposter looks correct from all angles.

- [ ] **Step 3: Fitted part — fragmentation gone**

Ask the user to run a fitted (organic/simplified-cage) part as an imposter and compare against the prior per-facet fragmentation.

Expected: the surface reads as continuous; per-facet pass-through fragmentation is gone (residual thin seams at chart boundaries are acceptable per the design's option-1 scope).

- [ ] **Step 4: Curvature — no smearing**

Ask the user to view a clearly curved region of an organic part.

Expected: relief tracks the curved surface without the swimming/smearing that a single-frame march would show — confirming the piecewise-affine re-anchoring works. If smearing persists, check that `imposterTriIdTex` sampling returns valid ids (temporarily tighten `MSL_IMP_RATIO`/cage density) and that `inst.transform` matches the seed-frame space.

- [ ] **Step 5: Tuning pass (optional)**

If charts are too fragmented, raise `chartConeDeg` (wider charts, fewer seams); if the projection self-overlaps (UV folds visible as garbage), lower it (must stay < 90°). Default 75° is the starting point.

- [ ] **Step 6: Final review**

Dispatch a final code review over the whole change set (all commits from Task 1-11) before finishing the branch.

---

## Self-Review

**Spec coverage:**
- Adjacency → Task 2. Segmentation → Task 3. Projection (`plane_basis` + build_cage pass) → Tasks 4, 6. Packing → Tasks 5, 6. Triangle-id atlas → Task 7. Cage-tri-data texture → Task 8. Channel-map (chart rect + id) → Task 9. Runtime textures/uniforms → Task 10. Piecewise-affine march bounded to chart rect, terminate at chart edge → Task 11. Params/cache (chartConeDeg, version bump, serialization) → Task 1. Testing → per-task tests + Task 12 visual. Cube unification → Task 6. All spec sections map to a task.

**Placeholder scan:** No TBD/TODO; every code step shows complete code; every test step shows the assertions; commands have expected output.

**Type consistency:** `TriAdj{int nbr[3]}`, `ChartRect{minU,minV,w,h}`, `ChartPlacement{ox,oy}`, `ImposterAsset.tri_chart` (vector<uint32_t>), `ImposterAsset.triid` (vector<uint8_t>, uint16 LE, 0xFFFF sentinel), `chartConeDeg` (float, struct=32B), `kFormatVersion=2`. `pack_cage_uvs_bvh_order(a, triIdx, nTris)` signature unchanged; `pack_cage_tri_data(a)` width=nTris height=6; shader uniforms `imposterCageTriTex`/`imposterTriIdTex`; members `imposter_cagetri_tex_`/`imposter_triid_tex_`. `reliefMarch` new params (chartLo, chartHi, seedTriId, instXform) are produced at the call site in Task 11. Names are consistent across tasks.

**Known follow-ons (out of scope, per spec):** cross-chart adjacency handoff; area-aware packing for grazing-facet resolution.
