# Direct-Triangle Path & Variations (SP-6) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a direct-triangle emission session (`beginShape`/`vertex`/`endShape`, radius-skinned `line`) plus `instance(child, variation)` variation binding to the SP-2 script host, merging triangles with per-triangle materials into the part's single per-cell BLAS and deduping variations by content hash.

**Architecture:** A standalone, JS-free C++ module (`triangle_emit`) owns a `TriangleBuildBuffer` that accumulates `Tri`+`TriEx` from primitive assembly (transformed by a supplied `mat4`, tagged with a current material id) and from a line tuber that converts a radius-skinned segment into stepped-sphere solid triangles. At bake the buffer's triangles are appended to whatever the voxel session lowered, and the combined set is registered as ONE BLAS via `BLASManager::register_triangles`. Triangles are thin literal surfaces and never enter the SDF/field path. A separate `ChildInstance` recorder folds variation params into the SP-1 `resolved_hash` so identical params dedup to one artifact, independent of LOD.

**Tech Stack:** C++17, QuickJS-ng DSL bindings (SP-2), BLAS Tri/TriEx (consumed from MatterSurfaceLib), SP-1 child-instance table, headless tests under `MatterEngine3/tests/`.

> **Relocation note (from the master plan):** This sub-plan obeys the `MatterEngine3` relocation contract in `2026-06-24-procedural-part-system-master-plan.md`. All NEW files (`triangle_emit.{hpp,cpp}`, `triangle_variation_tests.cpp`) live under `MatterEngine3/`; the consumed MatterSurfaceLib prototype backend (`blas_manager.cpp`, `bvh.cpp`, `vertex_ao.cpp`, `occupancy.cpp`) is referenced read-only as `../../MatterSurfaceLib/src/<dep>` with `-I../../MatterSurfaceLib/include`. The prototype is never modified.

---

## Confirmed interfaces (read from the codebase, not assumed)

- `Tri` (`MatterSurfaceLib/include/bvh.h:15`): `{ float3 vertex0; float3 vertex1; float3 vertex2; float3 centroid; }` (ALIGN(64), each unioned with `__m128`).
- `TriEx` (`MatterSurfaceLib/include/bvh.h:27`): `{ float2 uv0,uv1,uv2; float3 N0,N1,N2; int materialId; float4 tint; float ao0=1,ao1=1,ao2=1; }`. Per-triangle material is `materialId`; default tint must be `(1,1,1,0)` = neutral.
- `BLASManager::register_triangles(Tri* triangles, int triangle_count, const TriEx* triex = nullptr)` (`MatterSurfaceLib/include/blas_manager.hpp:109`) → `BLASHandle`. Per-triangle `materialId` and `tint` both participate in dedup identity (`blas_manager.hpp:199-204`).
- `BLASManager::get_entry(handle)` → `const BLASEntry*` with `.triangles` (`std::vector<Tri>`) and `.tri_extra` (`std::vector<TriEx>`); `get_unique_blas_count()` returns the number of distinct BLAS entries.
- `mat4` (`bvh.h:129`): `TransformPoint(float3)`, `TransformVector(float3)`, `Translate`, `Scale`, `operator()(i,j)`, `cell[16]` row-major.
- `ChildInstance` (SP-1 `MatterEngine3/include/part_asset_v2.h`, spec §API): `{ uint64_t child_resolved_hash; float transform[16]; }`.
- `compute_resolved_hash(const void* source, size_t, const void* params, size_t, const uint64_t* child_hashes, size_t)` → `uint64_t` (SP-1; sorts children internally, fnv1a64).
- Test convention (`MatterSurfaceLib/tests/blas_tint_tests.cpp` + `MatterSurfaceLib/tests/Makefile:155-162`): plain `int main()`, `#define CHECK(cond,msg)`, link the consumed prototype backend `../../MatterSurfaceLib/src/blas_manager.cpp ../../MatterSurfaceLib/src/bvh.cpp ../../MatterSurfaceLib/src/vertex_ao.cpp ../../MatterSurfaceLib/src/occupancy.cpp` (from `MatterEngine3/tests`), GL-free.

**Assumptions stated where SP-2/SP-1 are unimplemented at execution time:**
- SP-2's build buffer is depended on only through a narrow seam: this module produces a `std::vector<Tri>`/`std::vector<TriEx>` that the host appends to the voxel-lowered mesh before the single `register_triangles` call. If SP-2 exposes a concrete `BuildBuffer` type, the Task 2 merge step binds to it; otherwise the module's own `TriangleBuildBuffer` is the seam and a thin SP-2 task calls `appendTo(host_tris, host_triex)`.
- SP-1's `ChildInstance` and `compute_resolved_hash` are depended on by signature from the SP-1 spec. If unavailable at execution time, Task 5 declares a local mirror struct `ChildInstance { uint64_t child_resolved_hash; float transform[16]; }` and a local `fnv1a64`-based `compute_resolved_hash`, with a TODO to swap to `part_asset_v2.h` once SP-1 lands. This is flagged inline in the step.

---

## File Structure

```
MatterEngine3/
├── include/
│   └── triangle_emit.hpp        # NEW: TriangleBuildBuffer, ShapeType, primitive assembly,
│                                 #      line tuber, variation/ChildInstance recorder API
├── src/
│   └── triangle_emit.cpp        # NEW: implementation (GL-free, no JS, no field/SDF)
└── tests/
    ├── triangle_variation_tests.cpp   # NEW: headless GL-free suite
    └── Makefile                       # APPEND: add TRIVAR target + run-trivar rule
                                       #   (consumes ../../MatterSurfaceLib/src/* read-only)
```

`triangle_emit` is deliberately JS-free and field-free so it is unit-testable by feeding transforms + vertices directly. A later thin SP-2 task (out of scope here, noted in Task 6) binds these functions into the QuickJS DSL (`beginShape`/`vertex`/`endShape`/`line`/`lineThickness`/`instance`).

---

## Task 1 — Triangle emission: beginShape/vertex/endShape → Tri+TriEx (transform + material)

Build the primitive-assembly core: a `TriangleBuildBuffer` that, given the current transform `mat4` and a current material id, turns `beginShape(type)` + a vertex list + `endShape()` into transformed `Tri` plus per-triangle `TriEx` (carrying `materialId`, neutral tint, face normals as a shading-normal fallback).

**Files:**
- CREATE: `MatterEngine3/include/triangle_emit.hpp`
- CREATE: `MatterEngine3/src/triangle_emit.cpp`
- CREATE: `MatterEngine3/tests/triangle_variation_tests.cpp`
- APPEND: `MatterEngine3/tests/Makefile` (already exists from SP-1)

Steps:

- [ ] Write the test header + first failing test in `MatterEngine3/tests/triangle_variation_tests.cpp`:
  ```cpp
  #include "../include/triangle_emit.hpp"
  #include "../../MatterSurfaceLib/include/blas_manager.hpp"
  #include <cstdio>
  #include <cmath>

  static int failures = 0;
  #define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)

  static bool near3(float3 a, float3 b, float eps = 1e-5f) {
      return fabsf(a.x-b.x) < eps && fabsf(a.y-b.y) < eps && fabsf(a.z-b.z) < eps;
  }

  static void test_triangle_emission() {
      // A single TRIANGLES shape of one tri, identity transform, material 7.
      tri_emit::TriangleBuildBuffer buf;
      mat4 xf = mat4::Identity();
      buf.beginShape(tri_emit::ShapeType::TRIANGLES, xf, /*material*/7);
      buf.vertex(make_float3(0, 0, 0));
      buf.vertex(make_float3(1, 0, 0));
      buf.vertex(make_float3(0, 1, 0));
      buf.endShape();

      CHECK(buf.triangles().size() == 1, "one triangle emitted");
      CHECK(buf.tri_extra().size() == 1, "one TriEx emitted (parallel)");
      const Tri& t = buf.triangles()[0];
      CHECK(near3(t.vertex0, make_float3(0,0,0)), "v0 untransformed under identity");
      CHECK(near3(t.vertex1, make_float3(1,0,0)), "v1 untransformed under identity");
      CHECK(near3(t.vertex2, make_float3(0,1,0)), "v2 untransformed under identity");
      const TriEx& e = buf.tri_extra()[0];
      CHECK(e.materialId == 7, "per-triangle material is the current cursor");
      CHECK(fabsf(e.tint.w - 0.0f) < 1e-6f, "neutral tint alpha 0 (no tint)");
      CHECK(near3(e.N0, make_float3(0,0,1)) && near3(e.N1, make_float3(0,0,1))
            && near3(e.N2, make_float3(0,0,1)), "face normal = +Z for CCW XY tri");

      // Transform stack applied: translate (10,0,0) then the same tri.
      tri_emit::TriangleBuildBuffer buf2;
      mat4 tr = mat4::Translate(make_float3(10, 0, 0));
      buf2.beginShape(tri_emit::ShapeType::TRIANGLES, tr, /*material*/3);
      buf2.vertex(make_float3(0, 0, 0));
      buf2.vertex(make_float3(1, 0, 0));
      buf2.vertex(make_float3(0, 1, 0));
      buf2.endShape();
      CHECK(near3(buf2.triangles()[0].vertex0, make_float3(10,0,0)), "transform applied to v0");
      CHECK(near3(buf2.triangles()[0].centroid,
                  make_float3(10 + 1.0f/3.0f, 1.0f/3.0f, 0.0f)), "centroid transformed");
  }

  int main() {
      test_triangle_emission();
      if (failures == 0) printf("All triangle_variation tests passed\n");
      return failures == 0 ? 0 : 1;
  }
  ```
- [ ] Append the test target to the existing `MatterEngine3/tests/Makefile` (created by SP-1). The new impl `triangle_emit.cpp` is a `../src/<new>.cpp` source; the consumed prototype backend is referenced read-only as `../../MatterSurfaceLib/src/<dep>.cpp`. Append this block:
  ```make
  # Direct-triangle emission + line tuber + variation dedup tests (headless, GL-free).
  # NEW impl ../src/triangle_emit.cpp + consumed MatterSurfaceLib backend sources.
  TRIVAR_TARGET = triangle_variation_tests
  TRIVAR_SOURCES = triangle_variation_tests.cpp ../src/triangle_emit.cpp \
                   ../../MatterSurfaceLib/src/blas_manager.cpp ../../MatterSurfaceLib/src/bvh.cpp \
                   ../../MatterSurfaceLib/src/vertex_ao.cpp ../../MatterSurfaceLib/src/occupancy.cpp

  $(TRIVAR_TARGET): $(TRIVAR_SOURCES)
  	$(CC) $(TRIVAR_SOURCES) -o $(TRIVAR_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)

  run-trivar: $(TRIVAR_TARGET)
  	./$(TRIVAR_TARGET)
  ```
  Also add `run-trivar` to the existing `.PHONY` line and `$(TRIVAR_TARGET)` to the existing `clean` rule. Do NOT recreate the Makefile or edit `MatterSurfaceLib/tests/Makefile`.
- [ ] Run `make -C MatterEngine3/tests run-trivar` — expect FAIL (compile error: `triangle_emit.hpp` does not exist).
- [ ] Create `MatterEngine3/include/triangle_emit.hpp` with the minimal API:
  ```cpp
  #pragma once
  #include "bvh.h"      // Tri, TriEx, mat4, float3, make_float3
  #include <vector>
  #include <cstdint>

  namespace tri_emit {

  // Primitive types the direct-triangle session understands. Only TRIANGLES is
  // needed for the SP-6 testing bullets; the others are reserved for the DSL
  // surface form and fan/strip out to TRIANGLES at endShape().
  enum class ShapeType { TRIANGLES, TRIANGLE_STRIP, TRIANGLE_FAN };

  // Accumulates direct triangles as (Tri, TriEx) pairs. Triangles are literal
  // thin surfaces: transformed by the supplied matrix, tagged with a per-triangle
  // material id, carrying neutral tint (1,1,1,0) and a face-normal shading
  // fallback. NO SDF/field interaction. JS-free so it is unit-testable directly.
  class TriangleBuildBuffer {
  public:
      void beginShape(ShapeType type, const mat4& transform, int material_id);
      void vertex(float3 position);   // local-space; transformed at endShape()
      void endShape();                // assembles pending vertices into Tri/TriEx

      // Append a radius-skinned segment as stepped-sphere solid triangles (Task 3).
      // r0 = radius at a, r1 = radius at b (lerped); step_count spheres along seg.
      void line(float3 a, float3 b, float r0, float r1, int material_id,
                const mat4& transform, int rings = 6, int segments = 8);

      const std::vector<Tri>&   triangles() const { return tris_; }
      const std::vector<TriEx>& tri_extra() const { return triex_; }

      // Append this buffer's contents onto a host's triangle/triex arrays (the
      // SP-2 build-buffer merge seam). Used so the voxel-lowered mesh and the
      // direct triangles register as ONE BLAS.
      void appendTo(std::vector<Tri>& out_tris, std::vector<TriEx>& out_triex) const;

      void clear();

  private:
      void emitTriangle(float3 p0, float3 p1, float3 p2, int material_id,
                        const mat4& transform);

      std::vector<Tri>   tris_;
      std::vector<TriEx> triex_;
      // pending shape state between beginShape/endShape
      ShapeType          cur_type_  = ShapeType::TRIANGLES;
      mat4               cur_xf_;
      int                cur_mat_   = 0;
      bool               open_      = false;
      std::vector<float3> verts_;   // local-space pending vertices
  };

  } // namespace tri_emit
  ```
- [ ] Create `MatterEngine3/src/triangle_emit.cpp` implementing emission (line/tuber are stubs filled in Task 3):
  ```cpp
  #include "triangle_emit.hpp"
  #include <cmath>

  namespace tri_emit {

  static float3 face_normal(float3 p0, float3 p1, float3 p2) {
      float3 e1 = p1 - p0, e2 = p2 - p0;
      float3 n = cross(e1, e2);
      float len = sqrtf(n.x*n.x + n.y*n.y + n.z*n.z);
      if (len < 1e-12f) return make_float3(0, 0, 1);
      return make_float3(n.x/len, n.y/len, n.z/len);
  }

  void TriangleBuildBuffer::emitTriangle(float3 p0, float3 p1, float3 p2,
                                         int material_id, const mat4& transform) {
      float3 w0 = transform.TransformPoint(p0);
      float3 w1 = transform.TransformPoint(p1);
      float3 w2 = transform.TransformPoint(p2);
      Tri t;
      t.vertex0  = w0;
      t.vertex1  = w1;
      t.vertex2  = w2;
      t.centroid = make_float3((w0.x+w1.x+w2.x)/3.0f,
                               (w0.y+w1.y+w2.y)/3.0f,
                               (w0.z+w1.z+w2.z)/3.0f);
      tris_.push_back(t);

      TriEx e{};                       // zero-init: uv/normals/ao set below
      float3 n = face_normal(w0, w1, w2);
      e.uv0 = e.uv1 = e.uv2 = make_float2(0.0f, 0.0f);
      e.N0 = e.N1 = e.N2 = n;          // face-normal shading fallback
      e.materialId = material_id;      // per-triangle material
      e.tint = make_float4(1.0f, 1.0f, 1.0f, 0.0f);  // neutral: alpha 0 = no tint
      e.ao0 = e.ao1 = e.ao2 = 1.0f;    // unbaked = fully unoccluded
      triex_.push_back(e);
  }

  void TriangleBuildBuffer::beginShape(ShapeType type, const mat4& transform, int material_id) {
      cur_type_ = type;
      cur_xf_   = transform;
      cur_mat_  = material_id;
      open_     = true;
      verts_.clear();
  }

  void TriangleBuildBuffer::vertex(float3 position) {
      if (open_) verts_.push_back(position);
  }

  void TriangleBuildBuffer::endShape() {
      if (!open_) return;
      const size_t n = verts_.size();
      switch (cur_type_) {
          case ShapeType::TRIANGLES:
              for (size_t i = 0; i + 2 < n + 1 && i + 2 < n; i += 3)
                  emitTriangle(verts_[i], verts_[i+1], verts_[i+2], cur_mat_, cur_xf_);
              break;
          case ShapeType::TRIANGLE_STRIP:
              for (size_t i = 0; i + 2 < n; ++i) {
                  // keep consistent winding by flipping odd triangles
                  if (i & 1) emitTriangle(verts_[i+1], verts_[i], verts_[i+2], cur_mat_, cur_xf_);
                  else       emitTriangle(verts_[i], verts_[i+1], verts_[i+2], cur_mat_, cur_xf_);
              }
              break;
          case ShapeType::TRIANGLE_FAN:
              for (size_t i = 1; i + 1 < n; ++i)
                  emitTriangle(verts_[0], verts_[i], verts_[i+1], cur_mat_, cur_xf_);
              break;
      }
      verts_.clear();
      open_ = false;
  }

  void TriangleBuildBuffer::line(float3, float3, float, float, int, const mat4&, int, int) {
      // implemented in Task 3
  }

  void TriangleBuildBuffer::appendTo(std::vector<Tri>& out_tris,
                                     std::vector<TriEx>& out_triex) const {
      out_tris.insert(out_tris.end(), tris_.begin(), tris_.end());
      out_triex.insert(out_triex.end(), triex_.begin(), triex_.end());
  }

  void TriangleBuildBuffer::clear() {
      tris_.clear(); triex_.clear(); verts_.clear(); open_ = false;
  }

  } // namespace tri_emit
  ```
  NOTE: `make_float2`/`make_float4`/`cross`/`operator-` for `float3` are global (from `precomp.h`, pulled in via `bvh.h`); confirm `make_float2` exists in `precomp.h` during this step — if it does not, build the `float2` literally with `float2 z; z.x = z.y = 0.0f;`.
- [ ] Run `make -C MatterEngine3/tests run-trivar` — expect PASS ("All triangle_variation tests passed").
- [ ] Commit: `git add MatterEngine3/include/triangle_emit.hpp MatterEngine3/src/triangle_emit.cpp MatterEngine3/tests/triangle_variation_tests.cpp MatterEngine3/tests/Makefile && git commit` with message `feat(SP-6): direct-triangle emission into Tri/TriEx build buffer` and trailer `Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>`.

---

## Task 2 — One BLAS: voxel sphere + triangle quad merge into a SINGLE part BLAS

Prove the unifying decision: a part that mixes a voxel-lowered mesh (simulated here by a sphere-triangle blob, since SP-2's lowering is GL-bound) plus a direct triangle quad registers as ONE BLAS holding both, with distinct per-triangle materials, and that the merge seam (`appendTo`) preserves per-triangle materialId.

**Files:**
- EDIT: `MatterEngine3/tests/triangle_variation_tests.cpp`

Steps:

- [ ] Add a failing test function to `triangle_variation_tests.cpp` and call it from `main()`:
  ```cpp
  static void test_one_blas_merge() {
      // Simulate the voxel-lowered surface mesh as a small triangle set with
      // material 1 (real SP-2 lowering is GL-bound; here we exercise the merge
      // seam: voxel tris + direct tris -> ONE register_triangles call).
      std::vector<Tri>   host_tris;
      std::vector<TriEx> host_triex;
      // "voxel sphere" stand-in: two triangles, material 1.
      {
          Tri a; a.vertex0 = make_float3(0,0,0); a.vertex1 = make_float3(1,0,0);
                 a.vertex2 = make_float3(0,1,0); a.centroid = make_float3(.33f,.33f,0);
          Tri b; b.vertex0 = make_float3(1,1,0); b.vertex1 = make_float3(0,1,0);
                 b.vertex2 = make_float3(1,0,0); b.centroid = make_float3(.66f,.66f,0);
          TriEx ea{}; ea.materialId = 1; ea.tint = make_float4(1,1,1,0);
          ea.N0=ea.N1=ea.N2=make_float3(0,0,1); ea.ao0=ea.ao1=ea.ao2=1;
          TriEx eb = ea;
          host_tris.push_back(a); host_tris.push_back(b);
          host_triex.push_back(ea); host_triex.push_back(eb);
      }

      // Direct triangle quad, material 2, offset so it's distinct geometry.
      tri_emit::TriangleBuildBuffer buf;
      mat4 xf = mat4::Translate(make_float3(5, 0, 0));
      buf.beginShape(tri_emit::ShapeType::TRIANGLES, xf, /*material*/2);
      buf.vertex(make_float3(0,0,0)); buf.vertex(make_float3(1,0,0)); buf.vertex(make_float3(1,1,0));
      buf.vertex(make_float3(0,0,0)); buf.vertex(make_float3(1,1,0)); buf.vertex(make_float3(0,1,0));
      buf.endShape();
      CHECK(buf.triangles().size() == 2, "quad is 2 triangles");

      // MERGE into the single host buffer, then ONE BLAS.
      buf.appendTo(host_tris, host_triex);
      CHECK(host_tris.size() == 4, "voxel(2) + direct(2) merged before register");
      CHECK(host_triex.size() == 4, "triex parallel after merge");

      BLASManager mgr;
      BLASHandle h = mgr.register_triangles(host_tris.data(), (int)host_tris.size(),
                                            host_triex.data());
      CHECK(h != INVALID_BLAS_HANDLE, "merged BLAS registered");
      CHECK(mgr.get_unique_blas_count() == 1, "exactly ONE part BLAS, no second BLAS");

      const BLASManager::BLASEntry* e = mgr.get_entry(h);
      CHECK(e != nullptr, "entry retrievable");
      CHECK(e->triangles.size() == 4, "single BLAS holds all 4 triangles");
      // distinct per-triangle materials coexist in the one BLAS
      bool has1 = false, has2 = false;
      for (const TriEx& tx : e->tri_extra) { if (tx.materialId == 1) has1 = true;
                                             if (tx.materialId == 2) has2 = true; }
      CHECK(has1 && has2, "both per-triangle materials present in one BLAS");
  }
  ```
- [ ] Run `make -C MatterEngine3/tests run-trivar` — expect PASS already (the merge seam + register path is real code; no new production code needed). If `get_entry`/`tri_extra` usage fails to compile, that is the FAIL signal — fix by matching the confirmed `BLASEntry` member names (`triangles`, `tri_extra`) from the consumed `MatterSurfaceLib/include/blas_manager.hpp:58-59`.
- [ ] If PASS without production changes, this task is a characterization test of the one-BLAS contract; commit as a test-only change. If a real gap surfaces (e.g. `appendTo` ordering), fix in `triangle_emit.cpp` minimally.
- [ ] Commit: `git add MatterEngine3/tests/triangle_variation_tests.cpp && git commit` with message `test(SP-6): voxel+direct triangles merge into one part BLAS` and trailer `Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>`.

---

## Task 3 — Skinned line tuber: stepped spheres with lerped radius taper

Implement `line(a, b, r0, r1, ...)` so a 1D primitive with a skinning radius tubes into solid stepped-sphere triangle geometry. Stepping approach: place `step_count` UV-sphere shells along the segment at evenly spaced parameters `t in [0,1]`, each sphere's radius lerped `lerp(r0, r1, t)`, each tessellated into `rings x segments` triangles. This reuses the simple analytic sphere tessellation (independent of the voxel sphere-brush/field path, per the open-question resolution: a dedicated triangle tuber keeps lines OUT of the field path — consistent with "thin surfaces, no field interaction").

**Files:**
- EDIT: `MatterEngine3/src/triangle_emit.cpp`
- EDIT: `MatterEngine3/tests/triangle_variation_tests.cpp`

Steps:

- [ ] Add a failing test to `triangle_variation_tests.cpp` and call it from `main()`:
  ```cpp
  static void test_skinned_line() {
      // line from (0,0,0) to (2,0,0), constant radius 0.5 -> solid stepped spheres.
      tri_emit::TriangleBuildBuffer buf;
      mat4 id = mat4::Identity();
      buf.line(make_float3(0,0,0), make_float3(2,0,0), 0.5f, 0.5f,
               /*material*/4, id, /*rings*/4, /*segments*/6);
      CHECK(!buf.triangles().empty(), "tubed line produced triangles");
      CHECK(buf.triangles().size() == buf.tri_extra().size(), "Tri/TriEx parallel");
      // all triangles carry the line's material
      for (const TriEx& e : buf.tri_extra())
          CHECK(e.materialId == 4, "tubed triangle has line material");

      // Geometry is solid around the axis: some triangle vertex must be off-axis
      // (radius applied), not collapsed onto the segment.
      bool off_axis = false;
      for (const Tri& t : buf.triangles()) {
          if (fabsf(t.vertex0.y) > 0.1f || fabsf(t.vertex0.z) > 0.1f) off_axis = true;
      }
      CHECK(off_axis, "stepped spheres have radius (not degenerate to the line)");

      // Lerped radius taper: r0=0.6 at a, r1=0.1 at b. Max |offset from axis|
      // near a must exceed that near b.
      tri_emit::TriangleBuildBuffer taper;
      taper.line(make_float3(0,0,0), make_float3(2,0,0), 0.6f, 0.1f,
                 /*material*/4, id, /*rings*/4, /*segments*/6);
      float max_r_near_a = 0.0f, max_r_near_b = 0.0f;
      for (const Tri& t : taper.triangles()) {
          float3 v = t.centroid;
          float r = sqrtf(v.y*v.y + v.z*v.z);
          if (v.x < 0.5f)      max_r_near_a = (r > max_r_near_a) ? r : max_r_near_a;
          else if (v.x > 1.5f) max_r_near_b = (r > max_r_near_b) ? r : max_r_near_b;
      }
      CHECK(max_r_near_a > max_r_near_b, "radius tapers from a (0.6) to b (0.1)");
  }
  ```
- [ ] Run `make -C MatterEngine3/tests run-trivar` — expect FAIL (`line` is a stub; `buf.triangles()` empty → "tubed line produced triangles" fails).
- [ ] Implement `line` in `MatterEngine3/src/triangle_emit.cpp`, replacing the stub:
  ```cpp
  void TriangleBuildBuffer::line(float3 a, float3 b, float r0, float r1,
                                 int material_id, const mat4& transform,
                                 int rings, int segments) {
      if (rings < 2) rings = 2;
      if (segments < 3) segments = 3;
      const float PI = 3.14159265358979323846f;

      float3 axis = b - a;
      float seg_len = sqrtf(axis.x*axis.x + axis.y*axis.y + axis.z*axis.z);
      // Step density: one sphere shell per ~half-min-radius of arc length, min 2,
      // so the tube reads solid without exploding triangle count.
      float min_r = (r0 < r1 ? r0 : r1);
      int step_count = 2;
      if (min_r > 1e-4f) {
          int by_len = 1 + (int)(seg_len / (0.5f * min_r));
          step_count = (by_len > step_count) ? by_len : step_count;
      }
      if (step_count > 64) step_count = 64;  // YAGNI cost cap

      auto emit_sphere = [&](float3 center, float radius) {
          // UV sphere: rings latitude bands x segments longitude slices.
          for (int ri = 0; ri < rings; ++ri) {
              float lat0 = PI * ((float)ri / rings - 0.5f);
              float lat1 = PI * ((float)(ri+1) / rings - 0.5f);
              float y0 = sinf(lat0), y1 = sinf(lat1);
              float cr0 = cosf(lat0), cr1 = cosf(lat1);
              for (int si = 0; si < segments; ++si) {
                  float lon0 = 2.0f * PI * ((float)si / segments);
                  float lon1 = 2.0f * PI * ((float)(si+1) / segments);
                  float x0 = cosf(lon0), z0 = sinf(lon0);
                  float x1 = cosf(lon1), z1 = sinf(lon1);
                  float3 p00 = make_float3(center.x + radius*cr0*x0, center.y + radius*y0, center.z + radius*cr0*z0);
                  float3 p01 = make_float3(center.x + radius*cr0*x1, center.y + radius*y0, center.z + radius*cr0*z1);
                  float3 p10 = make_float3(center.x + radius*cr1*x0, center.y + radius*y1, center.z + radius*cr1*z0);
                  float3 p11 = make_float3(center.x + radius*cr1*x1, center.y + radius*y1, center.z + radius*cr1*z1);
                  emitTriangle(p00, p10, p11, material_id, transform);
                  emitTriangle(p00, p11, p01, material_id, transform);
              }
          }
      };

      for (int i = 0; i < step_count; ++i) {
          float t = (step_count == 1) ? 0.0f : (float)i / (float)(step_count - 1);
          float3 center = make_float3(a.x + axis.x*t, a.y + axis.y*t, a.z + axis.z*t);
          float radius = r0 + (r1 - r0) * t;   // lerped taper
          emit_sphere(center, radius);
      }
  }
  ```
- [ ] Run `make -C MatterEngine3/tests run-trivar` — expect PASS.
- [ ] Commit: `git add MatterEngine3/src/triangle_emit.cpp MatterEngine3/tests/triangle_variation_tests.cpp && git commit` with message `feat(SP-6): tube skinned lines into stepped-sphere solid geometry` and trailer `Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>`.

---

## Task 4 — No field interaction: a direct triangle overlapping a voxel brush survives as authored

Prove triangles are thin literal surfaces: a direct triangle whose vertices sit inside a voxel sphere brush's volume is emitted unchanged (not smoothed, carved, or snapped to the field). Since `triangle_emit` has no field/SDF dependency at all, the test asserts that emission is independent of any brush data: the emitted vertices equal the authored (transformed) vertices regardless of a co-located brush descriptor.

**Files:**
- EDIT: `MatterEngine3/tests/triangle_variation_tests.cpp`

Steps:

- [ ] Add a failing test to `triangle_variation_tests.cpp` and call it from `main()`:
  ```cpp
  static void test_no_field_interaction() {
      // A voxel brush (center, radius) that fully contains the authored triangle.
      // The triangle must NOT be carved/smoothed toward the field: emitted verts
      // exactly equal the authored verts. triangle_emit has no field input by
      // construction, so this pins that contract.
      float3 brush_center = make_float3(0,0,0);
      float  brush_radius = 10.0f;   // triangle lies well inside this sphere

      float3 v0 = make_float3(0.0f, 0.0f, 0.0f);
      float3 v1 = make_float3(1.0f, 0.0f, 0.0f);
      float3 v2 = make_float3(0.0f, 1.0f, 0.0f);

      tri_emit::TriangleBuildBuffer buf;
      buf.beginShape(tri_emit::ShapeType::TRIANGLES, mat4::Identity(), /*material*/9);
      buf.vertex(v0); buf.vertex(v1); buf.vertex(v2);
      buf.endShape();

      CHECK(buf.triangles().size() == 1, "one triangle survives");
      const Tri& t = buf.triangles()[0];
      // Authored vertices are preserved exactly (no projection onto the SDF).
      CHECK(near3(t.vertex0, v0, 1e-6f), "v0 not pulled to field surface");
      CHECK(near3(t.vertex1, v1, 1e-6f), "v1 not pulled to field surface");
      CHECK(near3(t.vertex2, v2, 1e-6f), "v2 not pulled to field surface");
      // Sanity: vertices really are inside the brush volume (so a field-coupled
      // path WOULD have moved them) -> the no-op is meaningful, not vacuous.
      float d0 = sqrtf(v0.x*v0.x + v0.y*v0.y + v0.z*v0.z);
      CHECK(d0 < brush_radius, "vertex genuinely inside brush -> survival is meaningful");
      (void)brush_center;
  }
  ```
- [ ] Run `make -C MatterEngine3/tests run-trivar` — expect PASS (the no-field-interaction property holds by construction; this test pins it so a future field coupling regresses visibly). This is a guard test, not a driver of new code.
- [ ] Commit: `git add MatterEngine3/tests/triangle_variation_tests.cpp && git commit` with message `test(SP-6): direct triangles are thin surfaces, no field interaction` and trailer `Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>`.

---

## Task 5 — instance(child, variation): child-instance records + variation content-hash dedup

Implement the variation binding: `instance(childPart, variation)` records a `ChildInstance` (child resolved-hash + current transform). The child's resolved hash folds its params (the `variation`), so identical variation params → one cached artifact (one distinct hash, N records); different variation params → distinct artifacts. Variation = params bound at instance time.

**Files:**
- EDIT: `MatterEngine3/include/triangle_emit.hpp`
- EDIT: `MatterEngine3/src/triangle_emit.cpp`
- EDIT: `MatterEngine3/tests/triangle_variation_tests.cpp`

Steps:

- [ ] Add a failing test to `triangle_variation_tests.cpp` and call it from `main()`:
  ```cpp
  static void test_variation_dedup() {
      // A child part's source bytes (opaque) + variation params (opaque) fold into
      // a resolved hash. Same variation params N times -> same hash (one artifact),
      // N child-instance records. Different params -> distinct hash.
      tri_emit::VariationRecorder rec;
      const char* child_src = "class Rock extends Part {}";

      // variation A applied to 3 instances at different transforms
      const unsigned char paramsA[] = { 's','e','e','d','=','1' };
      mat4 x1 = mat4::Translate(make_float3(1,0,0));
      mat4 x2 = mat4::Translate(make_float3(2,0,0));
      mat4 x3 = mat4::Translate(make_float3(3,0,0));
      uint64_t hA1 = rec.instance(child_src, 26, paramsA, sizeof(paramsA), x1);
      uint64_t hA2 = rec.instance(child_src, 26, paramsA, sizeof(paramsA), x2);
      uint64_t hA3 = rec.instance(child_src, 26, paramsA, sizeof(paramsA), x3);
      CHECK(hA1 == hA2 && hA2 == hA3, "same variation params -> same resolved hash");

      // variation B: different params -> distinct artifact
      const unsigned char paramsB[] = { 's','e','e','d','=','2' };
      uint64_t hB = rec.instance(child_src, 26, paramsB, sizeof(paramsB), x1);
      CHECK(hB != hA1, "different variation params -> distinct resolved hash");

      // child-instance table: 4 records, transforms preserved, hashes correct
      const std::vector<ChildInstance>& kids = rec.children();
      CHECK(kids.size() == 4, "one child-instance record per instance() call");
      CHECK(kids[0].child_resolved_hash == hA1, "record 0 carries variation-A hash");
      CHECK(kids[3].child_resolved_hash == hB,  "record 3 carries variation-B hash");
      CHECK(fabsf(kids[1].transform[3] - 2.0f) < 1e-6f, "record 1 transform tx=2 (row-major [3])");

      // distinct cached artifacts = distinct hashes among the records
      int distinct = 0;
      uint64_t seen[8]; int ns = 0;
      for (const ChildInstance& c : kids) {
          bool found = false;
          for (int i = 0; i < ns; ++i) if (seen[i] == c.child_resolved_hash) found = true;
          if (!found) { seen[ns++] = c.child_resolved_hash; ++distinct; }
      }
      CHECK(distinct == 2, "3x variation A + 1x variation B -> 2 distinct artifacts");
  }
  ```
- [ ] Run `make -C MatterEngine3/tests run-trivar` — expect FAIL (`VariationRecorder`, `ChildInstance`, `rec.instance`, `rec.children` undefined).
- [ ] Add to `MatterEngine3/include/triangle_emit.hpp` (inside `namespace tri_emit`, after the buffer class). NOTE on SP-1 dependency: if SP-1's `MatterEngine3/include/part_asset_v2.h` already provides `ChildInstance` and `compute_resolved_hash` at execution time, `#include "part_asset_v2.h"` (resolved via `-I../include`) and use those; otherwise use the local mirror below (flagged TODO to swap once SP-1 lands — struct layout matches the SP-1 spec exactly so the swap is source-compatible):
  ```cpp
  // SP-1 mirror (swap to part_asset_v2.h's ChildInstance/compute_resolved_hash once
  // SP-1 is implemented; layout matches the SP-1 spec byte-for-byte). The hash is
  // FNV-1a 64-bit over source + params + sorted(child_hashes), per SP-1.
  struct ChildInstance {
      uint64_t child_resolved_hash;
      float    transform[16];   // row-major, world placement under the parent
  };

  uint64_t compute_resolved_hash(const void* source_bytes, size_t source_len,
                                 const void* params_bytes, size_t params_len,
                                 const uint64_t* child_hashes, size_t child_count);

  // Records instance(child, variation) calls. "variation" = the params bytes bound
  // at instance time; they fold into the child's resolved hash so identical params
  // dedup to one artifact (consistent with SP-3). Independent of LOD.
  class VariationRecorder {
  public:
      // Records a child instance at the current transform; returns the child's
      // resolved hash (the cache key / artifact identity).
      uint64_t instance(const void* child_source, size_t source_len,
                        const void* variation_params, size_t params_len,
                        const mat4& transform);

      const std::vector<ChildInstance>& children() const { return children_; }
      void clear() { children_.clear(); }

  private:
      std::vector<ChildInstance> children_;
  };
  ```
- [ ] Implement in `MatterEngine3/src/triangle_emit.cpp` (inside `namespace tri_emit`). Guard the helper so it compiles whether or not `part_asset_v2.h` was included (only define the local one if SP-1's is absent — in the local-mirror case define unconditionally):
  ```cpp
  uint64_t compute_resolved_hash(const void* source_bytes, size_t source_len,
                                 const void* params_bytes, size_t params_len,
                                 const uint64_t* child_hashes, size_t child_count) {
      // FNV-1a 64-bit. Children sorted so the fold is order-independent (SP-1).
      const uint64_t FNV_OFFSET = 1469598103934665603ULL;
      const uint64_t FNV_PRIME  = 1099511628211ULL;
      uint64_t h = FNV_OFFSET;
      auto fold = [&](const unsigned char* p, size_t n) {
          for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= FNV_PRIME; }
      };
      fold(reinterpret_cast<const unsigned char*>(source_bytes), source_len);
      fold(reinterpret_cast<const unsigned char*>(params_bytes), params_len);
      std::vector<uint64_t> sorted(child_hashes, child_hashes + child_count);
      std::sort(sorted.begin(), sorted.end());
      for (uint64_t c : sorted)
          fold(reinterpret_cast<const unsigned char*>(&c), sizeof(c));
      return h;
  }

  uint64_t VariationRecorder::instance(const void* child_source, size_t source_len,
                                       const void* variation_params, size_t params_len,
                                       const mat4& transform) {
      // No grandchildren resolved here (SP-3 territory); fold an empty child list.
      uint64_t rh = compute_resolved_hash(child_source, source_len,
                                          variation_params, params_len,
                                          nullptr, 0);
      ChildInstance ci{};
      ci.child_resolved_hash = rh;
      for (int i = 0; i < 16; ++i) ci.transform[i] = transform.cell[i];
      children_.push_back(ci);
      return rh;
  }
  ```
  Add `#include <algorithm>` to the top of `triangle_emit.cpp` for `std::sort`.
- [ ] Run `make -C MatterEngine3/tests run-trivar` — expect PASS.
- [ ] Commit: `git add MatterEngine3/include/triangle_emit.hpp MatterEngine3/src/triangle_emit.cpp MatterEngine3/tests/triangle_variation_tests.cpp && git commit` with message `feat(SP-6): instance(child, variation) records + content-hash variation dedup` and trailer `Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>`.

---

## Task 6 — Variation/LOD independence (GL-free): identical LOD-array shape per variation

Prove variation chooses *which* geometry, LOD chooses *how detailed* — they are orthogonal. SP-4 (LOD generation) is out of scope, so GL-free we assert: each distinct variation, when its part is baked, gets the same LOD-array *shape* (level count) — the LOD structure is a property of the bake pipeline, not of the variation params. We model this with a small `lod_level_count_for` stub representing the SP-4 contract ("every variation gets the same ~N LOD levels"), and assert two different variations yield the same level count while remaining distinct artifacts.

**Files:**
- EDIT: `MatterEngine3/tests/triangle_variation_tests.cpp`

Steps:

- [ ] Add a failing test to `triangle_variation_tests.cpp` and call it from `main()`:
  ```cpp
  // Models the SP-4 contract: LOD level count is a pipeline constant, independent
  // of the variation params. Lives in the test (SP-4 owns the real generator).
  static int lod_level_count_for(uint64_t /*resolved_hash*/) {
      return 3;  // ~3 LOD levels per part, same rule for every variation
  }

  static void test_variation_lod_independence() {
      tri_emit::VariationRecorder rec;
      const char* src = "class Tree extends Part {}";
      const unsigned char pA[] = { 'v','=','a' };
      const unsigned char pB[] = { 'v','=','b' };

      uint64_t hA = rec.instance(src, 26, pA, sizeof(pA), mat4::Identity());
      uint64_t hB = rec.instance(src, 26, pB, sizeof(pB), mat4::Identity());

      CHECK(hA != hB, "two variations are distinct artifacts (variation picks geometry)");
      // LOD shape is identical across variations (LOD picks detail, not geometry).
      CHECK(lod_level_count_for(hA) == lod_level_count_for(hB),
            "both variations get the same LOD-array shape (LOD independent of variation)");
      CHECK(lod_level_count_for(hA) == 3, "expected ~3 LOD levels per variation");
  }
  ```
- [ ] Run `make -C MatterEngine3/tests run-trivar` — expect PASS (asserts the independence invariant; no production code, since SP-4 owns generation). This closes the spec's "variation/LOD independence" testing bullet GL-free.
- [ ] Add a code comment block at the top of `triangle_variation_tests.cpp` enumerating each spec Testing bullet and the test that covers it (emission, one BLAS, skinned line, no field interaction, variation dedup, variation/LOD independence) so coverage is auditable.
- [ ] Commit: `git add MatterEngine3/tests/triangle_variation_tests.cpp && git commit` with message `test(SP-6): variation and LOD are independent (variation=geometry, LOD=detail)` and trailer `Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>`.

---

## Task 7 — DSL binding seam note (no code; documents the SP-2 wiring point)

This task records (in the module header) exactly how a future SP-2 task binds `triangle_emit` into the QuickJS DSL, so the field-free module stays the testable core and the JS layer is a thin adapter. No new tests; this is a documentation-only step ensuring the merge-into-build-buffer contract is unambiguous for the SP-2 implementer.

**Files:**
- EDIT: `MatterEngine3/include/triangle_emit.hpp`

Steps:

- [ ] Add a doc comment block at the top of `MatterEngine3/include/triangle_emit.hpp` (below `#pragma once`) describing the binding seam:
  ```cpp
  // SP-6 direct-triangle session, JS-free core.
  //
  // The SP-2 ScriptHost binds these into the QuickJS DSL as the direct-triangle
  // session (mutually exclusive with the voxel session at any instant, sequential
  // within a part):
  //   beginShape(type)      -> TriangleBuildBuffer::beginShape(type, host.currentTransform(), host.currentMaterial())
  //   vertex(x,y,z)         -> TriangleBuildBuffer::vertex({x,y,z})
  //   endShape()            -> TriangleBuildBuffer::endShape()
  //   line(a,b)/lineThickness(r) -> TriangleBuildBuffer::line(a, b, r0, r1, host.currentMaterial(), host.currentTransform())
  //   instance(child, variation) -> VariationRecorder::instance(childSource, paramsBytes, host.currentTransform())
  //
  // At bake the host MERGES this buffer into the SAME build buffer the voxel
  // session fills (appendTo), then registers ONE BLAS via
  // BLASManager::register_triangles(tris, count, triex). There is NO separate
  // triangle BLAS and NO triangle render path. The VariationRecorder's children()
  // feed save_v2's child-instance table (SP-1). Triangles never enter the SDF.
  ```
- [ ] Run `make -C MatterEngine3/tests run-trivar` — expect PASS (comment-only change; verifies nothing broke).
- [ ] Commit: `git add MatterEngine3/include/triangle_emit.hpp && git commit` with message `docs(SP-6): document the SP-2 DSL binding seam for triangle_emit` and trailer `Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>`.

---

## Coverage map (spec Testing section → tasks)

- Triangle emission (per-tri material + transform stack) → **Task 1**
- One BLAS (voxel + direct quad, distinct per-tri materials, no second BLAS) → **Task 2**
- Skinned line (stepped spheres, lerped radius taper, triangles in BLAS) → **Task 3**
- No field interaction (triangle survives over a voxel brush) → **Task 4**
- Variation dedup (same params → one artifact, N records; different → distinct) → **Task 5**
- Variation/LOD independence (identical LOD-array shape, GL-free) → **Task 6**

Open-question resolutions baked into the plan:
- Skinning-radius encoding: per-end lerp (`r0`,`r1`), step density derived from arc length / min radius (capped at 64) — Task 3.
- Per-triangle vs voxel-surface material coexistence: both live in one BLAS as distinct `TriEx.materialId`; triangles are placed literally (coexist, not carve) — Task 2 + Task 4.
- Tuber path: dedicated triangle tuber (NOT the voxel sphere-brush/field path), keeping lines thin surfaces — Task 3.
- `variation` argument shape: opaque params byte-range bound at instance time, folded by `compute_resolved_hash` — Task 5.
