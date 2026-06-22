# Fitted-Cage Imposter Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render the fitted (arbitrary-geometry) imposter cage correctly by feeding the shader baked per-vertex cage UVs through a dedicated BVH-order UV texture, and unify the cube path onto the same mechanism.

**Architecture:** The bake already stores correct per-vertex atlas UVs in `ImposterAsset.verts`. We pack those UVs into a small dedicated texture in BVH-triangle order (using the cage BLAS `triIdx` permutation), and the shader `texelFetch`es the three corner UVs for the hit triangle instead of recomputing them from the triangle index. This is reorder-safe and geometry-agnostic, so the cube's normal/AABB special case is deleted.

**Tech Stack:** C++14, raylib (Texture2D / SetShaderValue), GLSL fragment shader, custom shader `#include` preprocessor (`make shaders`).

---

### Task 1: GL-free UV packing helper

**Files:**
- Modify: `MatterSurfaceLib/include/imposter_asset.h` (add declaration near `cage_to_tris`, ~line 103)
- Modify: `MatterSurfaceLib/src/imposter_asset.cpp` (add definition)
- Test: `MatterSurfaceLib/tests/imposter_asset_tests.cpp`

- [ ] **Step 1: Write the failing test**

Add to `tests/imposter_asset_tests.cpp`:

```cpp
// pack_cage_uvs_bvh_order: row r (0..2) col i holds the UV of corner r of the
// cage triangle that BVH slot i maps to (triIdx[i]). Layout is RGBA32F,
// width=nTris, height=3, so float offset = (row*nTris + i)*4, channels (u,v,0,0).
TEST_CASE("pack_cage_uvs_bvh_order respects the BVH permutation") {
    imposter_asset::ImposterAsset a;
    // two cage triangles, 6 verts; give each vert a distinct uv so we can trace it.
    a.verts.resize(6);
    for (int k = 0; k < 6; ++k) { a.verts[k].u = (float)k; a.verts[k].v = (float)(10 + k); }
    a.tris = { {0,1,2}, {3,4,5} };
    // BVH reorders: slot 0 -> original tri 1, slot 1 -> original tri 0.
    std::vector<uint32_t> triIdx = {1, 0};
    std::vector<float> buf = imposter_asset::pack_cage_uvs_bvh_order(a, triIdx.data(), 2);
    REQUIRE(buf.size() == (size_t)2 * 3 * 4);
    auto at = [&](int row, int i, int c){ return buf[(size_t)(row*2 + i)*4 + c]; };
    // slot 0 = original tri 1 = verts 3,4,5
    CHECK(at(0,0,0) == 3.0f); CHECK(at(0,0,1) == 13.0f);
    CHECK(at(1,0,0) == 4.0f); CHECK(at(2,0,0) == 5.0f);
    // slot 1 = original tri 0 = verts 0,1,2
    CHECK(at(0,1,0) == 0.0f); CHECK(at(0,1,1) == 10.0f);
    CHECK(at(1,1,0) == 1.0f); CHECK(at(2,1,0) == 2.0f);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd MatterSurfaceLib && make WSL_LINUX=1 -C tests imposter_asset_tests && ./tests/imposter_asset_tests`
Expected: compile error / link error — `pack_cage_uvs_bvh_order` not declared.

- [ ] **Step 3: Implement the helper**

In `include/imposter_asset.h` (after `cage_to_tris`, ~line 103):

```cpp
// Pack per-vertex cage UVs into a BVH-order RGBA32F buffer for the shader's
// imposterTriUvTex. Layout: width = nTris, height = 3 (row = triangle corner),
// channels = (u, v, 0, 0). For BVH slot i: original tri = triIdx[i], its three
// vertex UVs go to rows 0/1/2. GL-free so it is unit-testable.
std::vector<float> pack_cage_uvs_bvh_order(const ImposterAsset& a,
                                           const uint32_t* triIdx, int nTris);
```

In `src/imposter_asset.cpp` (anywhere after `cage_to_tris`):

```cpp
std::vector<float> imposter_asset::pack_cage_uvs_bvh_order(const ImposterAsset& a,
                                                           const uint32_t* triIdx, int nTris) {
    std::vector<float> buf((size_t)nTris * 3 * 4, 0.0f);
    for (int i = 0; i < nTris; ++i) {
        const ImposterTri& t = a.tris[triIdx[i]];
        const uint32_t vi[3] = { t.i0, t.i1, t.i2 };
        for (int r = 0; r < 3; ++r) {
            const CageVert& cv = a.verts[vi[r]];
            size_t o = (size_t)(r * nTris + i) * 4;
            buf[o + 0] = cv.u; buf[o + 1] = cv.v; buf[o + 2] = 0.0f; buf[o + 3] = 0.0f;
        }
    }
    return buf;
}
```

NOTE: confirm the `ImposterAsset.tris` element type and field names while implementing
(`ImposterTri` with `i0/i1/i2` per `imposter_asset.h`); if they differ, match the real names.

- [ ] **Step 4: Run test to verify it passes**

Run: `cd MatterSurfaceLib && make WSL_LINUX=1 -C tests imposter_asset_tests && ./tests/imposter_asset_tests`
Expected: all assertions pass.

- [ ] **Step 5: Commit**

```bash
git add MatterSurfaceLib/include/imposter_asset.h MatterSurfaceLib/src/imposter_asset.cpp MatterSurfaceLib/tests/imposter_asset_tests.cpp
git commit -m "feat: GL-free BVH-order cage UV packing for imposters"
```

---

### Task 2: Build and upload the UV texture (main.cpp)

**Files:**
- Modify: `MatterSurfaceLib/main.cpp` setup block (~line 650-680, after `imposter_grid_` is set)
- Modify: `MatterSurfaceLib/main.cpp` member declarations (~line 2164)
- Modify: `MatterSurfaceLib/main.cpp` uniform upload block (~line 1362-1376)

- [ ] **Step 1: Add members**

After `float imposter_aabb_min_[3] ...;` (~line 2166) add:

```cpp
    Texture2D imposter_triuv_tex_{};
    int   imposter_tri_count_ = 0;
```

- [ ] **Step 2: Build the texture at setup**

In `setup_imposter_demo`, after the `imposter_tri_base_ = ...` line (~line 663), add:

```cpp
            {
                const BLASManager::BLASEntry* e = blas_manager_->get_entry(imposter_cage_blas_);
                int nCageTris = (int)imp.tris.size();
                imposter_tri_count_ = nCageTris;
                std::vector<float> uvbuf =
                    imposter_asset::pack_cage_uvs_bvh_order(imp, e->bvh->triIdx, nCageTris);
                Image uvimg{}; uvimg.data = uvbuf.data();
                uvimg.width = nCageTris; uvimg.height = 3; uvimg.mipmaps = 1;
                uvimg.format = PIXELFORMAT_UNCOMPRESSED_R32G32B32A32;
                imposter_triuv_tex_ = LoadTextureFromImage(uvimg);
                SetTextureFilter(imposter_triuv_tex_, TEXTURE_FILTER_POINT);
            }
```

- [ ] **Step 3: Upload the uniforms**

In the `if (imposter_enabled_) { ... }` uniform block, replace the cube-specific uploads
(`imposterQuadCharts`, `imposterAabbMin`, `imposterAabbMax`) with:

```cpp
                SetShaderValueTexture(raytracing_shader_, GetShaderLocation(raytracing_shader_, "imposterTriUvTex"), imposter_triuv_tex_);
                SetShaderValue(raytracing_shader_, GetShaderLocation(raytracing_shader_, "imposterTriCount"), &imposter_tri_count_, SHADER_UNIFORM_INT);
```

Leave `imposterGrid`/`imposterPad` uploads in place for now (harmless; the shader stops
reading them in Task 3). Remove them in Task 4 cleanup.

- [ ] **Step 4: Build**

Run: `cd MatterSurfaceLib && make WSL_LINUX=1 2>&1 | tail -3`
Expected: `Built executable for linux` (the shader still references old uniforms — that is
fine, GLSL compiles; runtime correctness comes after Task 3). No C++ errors.

- [ ] **Step 5: Commit**

```bash
git add MatterSurfaceLib/main.cpp
git commit -m "feat: build and upload imposter per-triangle UV texture"
```

---

### Task 3: Shader reads UVs from the texture

**Files:**
- Modify: `MatterSurfaceLib/shaders/bvh_tlas_common.glsl` (uniform decls ~line 24-26; hit block ~line 822-840; reliefMarch cell bound; delete `imposterTriUVs`/`imposterCubeFace`/`imposterCubeUV`)

- [ ] **Step 1: Declare the new uniforms, remove old**

Replace lines 24-26 (`imposterQuadCharts`, `imposterAabbMin`, `imposterAabbMax`) with:

```glsl
uniform sampler2D imposterTriUvTex;   // RGBA32F: col=BVH triangle slot, row=corner, .xy=uv
uniform int   imposterTriCount;       // number of cage triangles (texture width)
```

- [ ] **Step 2: Replace the hit-block UV selection**

Replace the block that computes `uv0,uv1,uv2` (the `if (imposterQuadCharts != 0) { ... } else { imposterTriUVs(...) }`) with:

```glsl
            vec3 w0 = transformPosition(tri.v0, inst.transform);
            vec3 w1 = transformPosition(tri.v1, inst.transform);
            vec3 w2 = transformPosition(tri.v2, inst.transform);
            vec3 cageN = normalize(result.normal);
            vec2 uv0 = texelFetch(imposterTriUvTex, ivec2(localTri, 0), 0).xy;
            vec2 uv1 = texelFetch(imposterTriUvTex, ivec2(localTri, 1), 0).xy;
            vec2 uv2 = texelFetch(imposterTriUvTex, ivec2(localTri, 2), 0).xy;
```

- [ ] **Step 3: Fix reliefMarch cell bound to use triangle UV bounds**

In `reliefMarch`, replace the uniform-grid cell computation
(`float cellSz = 1.0/float(imposterGrid); vec2 cellLo = floor(entryUV/cellSz)*cellSz; vec2 cellHi = cellLo + cellSz;`)
with:

```glsl
    vec2 cellLo = min(uv0, min(uv1, uv2)) - vec2(0.002);
    vec2 cellHi = max(uv0, max(uv1, uv2)) + vec2(0.002);
```

(The break test that uses `cellLo`/`cellHi` stays as-is.)

- [ ] **Step 4: Delete now-dead functions**

Delete `imposterTriUVs`, `imposterCubeFace`, and `imposterCubeUV` entirely. Grep to confirm
no remaining references:

Run: `cd MatterSurfaceLib && grep -n "imposterTriUVs\|imposterCubeFace\|imposterCubeUV\|imposterQuadCharts\|imposterAabb" shaders/bvh_tlas_common.glsl`
Expected: no matches.

- [ ] **Step 5: Regenerate processed shaders and build**

Run: `cd MatterSurfaceLib && rm -f shaders/raytrace_tlas_blas_processed.fs shaders/imposter_bake_processed.fs && make WSL_LINUX=1 shaders && make WSL_LINUX=1 2>&1 | tail -3`
Expected: `Shader processed successfully` then `Built executable for linux`.
Verify: `grep -c imposterTriUvTex shaders/raytrace_tlas_blas_processed.fs` → ≥ 1.

- [ ] **Step 6: Visual check — fitted cage**

Run (clear cache so the fitted asset rebakes):
```bash
cd MatterSurfaceLib && rm -f imposters/*.imp
MSL_SHOW_IMPOSTER=1 MSL_IMP_INFLATION=0 MSL_CAPTURE=.claude/fitted_compare.png MSL_FRAMES=3 MSL_RENDER_MODE=0 MSL_CAM="3.2,-4.4,34,3.2,-4.4,-6.4" ./matter_surface_lib 2>&1 | tail -2
```
Expected: `.claude/fitted_compare.png` shows the imposter (right, +X 24) reproducing the
real part (left) — recognizable clusters, no shuffling/melting. Read the PNG to confirm.

- [ ] **Step 7: Visual regression — cube via unified path**

```bash
cd MatterSurfaceLib && rm -f imposters/*.imp
MSL_SHOW_IMPOSTER=1 MSL_IMPOSTER_CUBE=1 MSL_IMP_INFLATION=0 MSL_CAPTURE=.claude/cube_unified.png MSL_FRAMES=3 MSL_RENDER_MODE=0 MSL_CAM="15.2,-4.37,8,15.2,-4.37,-6.4" ./matter_surface_lib 2>&1 | tail -2
```
Expected: `.claude/cube_unified.png` matches the known-good cube (green top, red bottom-left,
blue bottom-right clovers). Confirms the unification didn't regress the cube.

- [ ] **Step 8: Commit**

```bash
git add MatterSurfaceLib/shaders/bvh_tlas_common.glsl MatterSurfaceLib/shaders/raytrace_tlas_blas_processed.fs MatterSurfaceLib/shaders/imposter_bake_processed.fs
git commit -m "feat: shader reads imposter UVs from texture, unify cube+fitted path"
```

---

### Task 4: Cleanup dead uniforms

**Files:**
- Modify: `MatterSurfaceLib/main.cpp` (remove `imposter_quad_charts_`, `imposter_aabb_min_/max_` members and their setup writes; remove `imposterGrid`/`imposterPad` uploads if the shader no longer reads them)
- Modify: `MatterSurfaceLib/shaders/bvh_tlas_common.glsl` (remove `imposterGrid`, `imposterPad` uniform declarations if unused after Task 3)

- [ ] **Step 1: Confirm what the shader still uses**

Run: `cd MatterSurfaceLib && grep -n "imposterGrid\|imposterPad" shaders/bvh_tlas_common.glsl`
Expected: only the `uniform` declarations remain (no uses). If so, they are dead.

- [ ] **Step 2: Remove dead shader uniforms**

Delete the `uniform int imposterGrid;` and `uniform float imposterPad;` declarations from
`bvh_tlas_common.glsl` (only if Step 1 showed no uses).

- [ ] **Step 3: Remove dead main.cpp members and uploads**

Remove `int imposter_quad_charts_ = 0;`, `float imposter_aabb_min_[3]...;`,
`float imposter_aabb_max_[3]...;` member lines; remove their assignments in
`setup_imposter_demo`; remove the `imposterGrid`/`imposterPad` `SetShaderValue` uploads.
Keep `imposter_grid_` only if still used elsewhere — grep first:

Run: `cd MatterSurfaceLib && grep -n "imposter_grid_\|imposter_quad_charts_\|imposter_aabb" main.cpp`
Remove every line that is now write-only/unused.

- [ ] **Step 4: Regenerate shaders and build**

Run: `cd MatterSurfaceLib && rm -f shaders/raytrace_tlas_blas_processed.fs shaders/imposter_bake_processed.fs && make WSL_LINUX=1 shaders && make WSL_LINUX=1 2>&1 | tail -3`
Expected: clean build, no "unused variable" warnings for the removed members.

- [ ] **Step 5: Re-run both visual checks**

Re-run Task 3 Steps 6 and 7. Expected: identical good results (cleanup changed nothing
visible).

- [ ] **Step 6: Commit**

```bash
git add MatterSurfaceLib/main.cpp MatterSurfaceLib/shaders/bvh_tlas_common.glsl MatterSurfaceLib/shaders/raytrace_tlas_blas_processed.fs MatterSurfaceLib/shaders/imposter_bake_processed.fs
git commit -m "refactor: drop cube-specific imposter uniforms after unification"
```

---

### Task 5: Quality pass (success bar B)

**Files:**
- Modify: `MatterSurfaceLib/main.cpp` `setup_imposter_demo` defaults (`ip.atlasW/H`, `ip.maxCageTris`) — only if visual checks show it's needed.

- [ ] **Step 1: Multi-angle visual audit**

Render the fitted imposter from several angles and compare to the real part:
```bash
cd MatterSurfaceLib
for cam in "15.2,-4.37,8,15.2,-4.37,-6.4" "8,-4.37,2,15.2,-4.37,-6.4" "15.2,4,2,15.2,-4.37,-6.4"; do
  MSL_SHOW_IMPOSTER=1 MSL_IMP_INFLATION=0 MSL_CAPTURE=.claude/fitted_ang_${cam// /}.png MSL_FRAMES=3 MSL_RENDER_MODE=0 MSL_CAM="$cam" ./matter_surface_lib >/dev/null 2>&1
done
```
Read each PNG. Judge: silhouette recognizable, no shuffling, parallax tracks the part.

- [ ] **Step 2: Tune only if a defect is visible**

If cells are too coarse (blocky silhouette), bump `ip.atlasW = ip.atlasH = 2048;` in
`setup_imposter_demo`. If the cage is too coarse (lumpy outline), raise `ip.maxCageTris`
(e.g. 4000) via the existing `MSL_IMP_MAXTRIS` env first to test before changing the default.
Make ONE change at a time, rebake (`rm -f imposters/*.imp`), and re-audit. Do not tune
speculatively — only fix defects you can see.

- [ ] **Step 3: Commit any tuning**

```bash
git add MatterSurfaceLib/main.cpp
git commit -m "tune: imposter atlas/cage density for fitted-cage quality"
```

---

## Self-Review

**Spec coverage:** UV texture (Task 1-2), shader fetch (Task 3), relief cell bound from UV
bounds (Task 3 Step 3), delete cube special case (Task 3 Step 4), unify cube (Task 3 Step 7
regression), cleanup uniforms (Task 4), B-quality tuning (Task 5), unit + visual tests
(Task 1 + Task 3/5). All spec sections map to a task.

**Placeholder scan:** One explicit "confirm field names" note in Task 1 Step 3 — that is a
verification instruction, not a placeholder; the code is complete and runs if the names
match the header. No TBDs.

**Type consistency:** `pack_cage_uvs_bvh_order(const ImposterAsset&, const uint32_t*, int)`
used identically in Task 1 and Task 2. `imposter_triuv_tex_` / `imposter_tri_count_` /
`imposterTriUvTex` / `imposterTriCount` consistent across Tasks 2-4. Texture layout
(width=nTris, height=3, RGBA32F, float offset `(row*nTris+i)*4`) consistent between the
packer (Task 1), the test (Task 1), the upload (Task 2), and the `texelFetch(ivec2(localTri,row))`
(Task 3).
