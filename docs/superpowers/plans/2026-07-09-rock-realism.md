# Rock Realism Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a native in-session `raycast()` DSL verb (analytic SDF probe) and rewrite `Rock.js` to build ellipsoid-blob boulders with surface-relative facet cuts.

**Architecture:** A new `field_distance()` signed-distance oracle in `csg_lowering.cpp` mirrors the mesher's staged log-sum-exp smooth-min field; `DslState::raycast()` sphere-traces it over the open voxel session's brush range and returns hit point + gradient normal to JS. `Rock.js` uses the probe to place plane cuts (large `lookAt`-oriented box brushes, `difference()`) at controlled depths below real surface points.

**Tech Stack:** C++ (MatterEngine3 kernel), QuickJS-ng DSL bindings, JS part schemas, headless C test harness (`check.h`), raylib math.

**Spec:** `docs/superpowers/specs/2026-07-09-rock-realism-design.md`

## Global Constraints

- **MatterSurfaceLib is read-only.** Mirror its smin formula in MatterEngine3; never edit `surface.c`.
- All engine paths below are relative to repo root `/mnt/d/Shared With Desktop/AI/matter-engine-cpp/` (worktree equivalents apply).
- Tests: `make -C MatterEngine3/tests run-iso` and `run-script` (headless, no GL). Do NOT run the full sweep per task — it is the final gate only.
- Every viewer/GPU run needs `GALLIUM_DRIVER=d3d12` (WSLg; without it Mesa falls back to llvmpipe GL 4.5 and FATALs).
- After engine changes, the Windows viewer must be rebuilt (`make -C MatterViewer windows`), and because headers change in this work, **clear all Windows object files first** (no header dep tracking; stale partial rebuilds cause wandering crashes).
- Scripted viewer runs must self-terminate — use `MatterViewer/tools/viewer_shots.sh` (FIFO quit + wait + kill trap). Never leave a viewer window open.
- Smoothing is whole-part (lowering collapses to max); `Rock.js` sets it once, before any brush.
- The mesher's smin: `f = fmin - k*ln(sum_i exp(-(f_i - fmin)/k))`, `k <= 1e-5` → hard min (`MatterSurfaceLib/src/surface.c:1345`). Stage fold (`surface.c:1421-1456`): first non-empty stage seeds the field regardless of its op; then Union → `fminf(field, d)`, Difference → `fmaxf(field, -d)`, Intersection → `fmaxf(field, d)`. Consecutive same-op brushes form one stage.

---

### Task 1: `field_distance()` oracle in csg_lowering

**Files:**
- Modify: `MatterEngine3/src/csg_lowering.h` (add declaration below `field_is_solid`)
- Modify: `MatterEngine3/src/csg_lowering.cpp` (implementation next to `field_is_solid`, ~line 153)
- Test: `MatterEngine3/tests/iso_primitive_tests.cpp`

**Interfaces:**
- Consumes: existing statics in `csg_lowering.cpp`: `sdSphere`, `sdBox`, `sdCapsule`, `sdCappedCone`, `mat_invert`, `xf`; `BuildOp` fields `kind/op/transform/center/halfExtents/segB/radius/r1`.
- Produces (Task 2 relies on this exact signature):
  ```cpp
  // csg_lowering.h
  // Analytic signed-distance oracle over ops[opBegin, opEnd), mirroring the
  // mesher's staged smooth-min field (surface.c): per-stage log-sum-exp smin
  // with fillet k, stages folded in authored order (consecutive same-op ops =
  // one stage; first non-empty stage seeds the field). k <= 1e-5 = hard ops.
  // Returns +INFINITY (1e9f) when the range is empty. Distances under
  // non-uniform brush transforms are distorted (same caveat as field_is_solid)
  // — callers must trace conservatively.
  float field_distance(const BuildBuffer& buf, size_t opBegin, size_t opEnd,
                       float k, const Vector3& worldPoint);
  ```

- [ ] **Step 1: Write the failing tests**

Append to `MatterEngine3/tests/iso_primitive_tests.cpp` (follow the file's existing `static void test_*` + `CHECK` style; find where tests are invoked — a `main()` or run list at the bottom — and register these the same way):

```cpp
// --- field_distance oracle (rock-realism raycast seam) ---------------------

static void test_field_distance_sphere_exact() {
    dsl::DslState s;
    s.beginVoxels(0.1f);
    s.sphere({0, 1, 0}, 0.5f, dsl::CsgOp::Union);
    s.endVoxels();
    const dsl::BuildBuffer& buf = s.buffer();
    float dOut  = dsl::field_distance(buf, 0, buf.ops.size(), 0.0f, {2.0f, 1, 0});
    float dIn   = dsl::field_distance(buf, 0, buf.ops.size(), 0.0f, {0.0f, 1, 0});
    float dSurf = dsl::field_distance(buf, 0, buf.ops.size(), 0.0f, {0.5f, 1, 0});
    CHECK(fabsf(dOut - 1.5f) < 1e-4f, "sphere: outside distance 1.5");
    CHECK(fabsf(dIn + 0.5f) < 1e-4f, "sphere: center distance -0.5");
    CHECK(fabsf(dSurf) < 1e-4f, "sphere: zero at surface");
}

static void test_field_distance_smin_bulge() {
    // Two overlapping spheres with k=0.1: at the midpoint the smooth union
    // must dip BELOW the hard min (metaball bulge), by k*ln(2) exactly
    // (both distances equal there).
    dsl::DslState s;
    s.beginVoxels(0.1f);
    s.sphere({-0.3f, 0.5f, 0}, 0.4f, dsl::CsgOp::Union);
    s.sphere({ 0.3f, 0.5f, 0}, 0.4f, dsl::CsgOp::Union);
    s.endVoxels();
    const dsl::BuildBuffer& buf = s.buffer();
    Vector3 mid{0, 0.5f, 0};
    float hard   = dsl::field_distance(buf, 0, buf.ops.size(), 0.0f, mid);
    float smooth = dsl::field_distance(buf, 0, buf.ops.size(), 0.1f, mid);
    CHECK(fabsf(hard - (-0.1f)) < 1e-4f, "smin: hard min at midpoint is -0.1");
    CHECK(fabsf(smooth - (hard - 0.1f * logf(2.0f))) < 1e-4f,
          "smin: k=0.1 dips k*ln(2) below hard min at equidistant point");
}

static void test_field_distance_difference_stage() {
    // Sphere r=0.5 at (0,1,0), then a difference box spanning x in [0.4, 0.9]:
    // the cut face is at x=0.4. Points in the removed region are positive.
    dsl::DslState s;
    s.beginVoxels(0.1f);
    s.sphere({0, 1, 0}, 0.5f, dsl::CsgOp::Union);
    s.box({0.65f, 1, 0}, {0.25f, 0.6f, 0.6f}, dsl::CsgOp::Union);
    s.set_last_op(dsl::CsgOp::Difference);
    s.endVoxels();
    const dsl::BuildBuffer& buf = s.buffer();
    float inCut  = dsl::field_distance(buf, 0, buf.ops.size(), 0.0f, {0.45f, 1, 0});
    float inBody = dsl::field_distance(buf, 0, buf.ops.size(), 0.0f, {0.0f, 1, 0});
    CHECK(inCut > 0.0f, "difference: removed region is outside (positive)");
    CHECK(inBody < 0.0f, "difference: body core still inside");
}

static void test_field_distance_sign_matches_oracle() {
    // With k=0 (hard ops) the sign of field_distance must agree with
    // field_is_solid everywhere. Probe a coarse grid over a CSG scene.
    dsl::DslState s;
    s.beginVoxels(0.1f);
    s.sphere({0, 0.5f, 0}, 0.5f, dsl::CsgOp::Union);
    s.sphere({0.4f, 0.7f, 0.1f}, 0.35f, dsl::CsgOp::Union);
    s.box({0.5f, 0.5f, 0}, {0.2f, 0.2f, 0.2f}, dsl::CsgOp::Union);
    s.set_last_op(dsl::CsgOp::Difference);
    s.endVoxels();
    const dsl::BuildBuffer& buf = s.buffer();
    int checked = 0;
    for (float x = -1.0f; x <= 1.0f; x += 0.2f)
    for (float y = -0.5f; y <= 1.5f; y += 0.2f)
    for (float z = -1.0f; z <= 1.0f; z += 0.2f) {
        Vector3 p{x, y, z};
        float d = dsl::field_distance(buf, 0, buf.ops.size(), 0.0f, p);
        if (fabsf(d) < 1e-3f) continue; // skip surface-ambiguous points
        bool solid = dsl::field_is_solid(buf, p);
        if (solid != (d < 0.0f)) {
            printf("FAIL: sign mismatch at %.2f,%.2f,%.2f (solid=%d d=%f)\n",
                   x, y, z, (int)solid, d);
            ++g_failures;
            return;
        }
        ++checked;
    }
    CHECK(checked > 500, "sign oracle: probed a meaningful grid");
}
```

Note: if `iso_primitive_tests.cpp` uses a different failure counter or check macro than `g_failures`/`CHECK`, match the file's local convention exactly.

- [ ] **Step 2: Run tests to verify they fail to compile**

Run: `make -C MatterEngine3/tests run-iso`
Expected: compile error — `field_distance` is not a member of namespace `dsl`.

- [ ] **Step 3: Implement `field_distance`**

Declaration in `MatterEngine3/src/csg_lowering.h` (below `field_is_solid`, comment from the Interfaces block above). Implementation in `MatterEngine3/src/csg_lowering.cpp`, directly after `field_is_solid`:

```cpp
// Signed-distance sibling of field_is_solid, mirroring the mesher's STAGED
// smooth-min evaluation (surface.c CalculateScalarStaged): consecutive same-op
// ops form a stage; within a stage distances combine via log-sum-exp smin with
// fillet k; stages fold in authored order (first non-empty stage seeds the
// field; Union=min, Difference=max(field,-d), Intersection=max(field,d)).
// With k<=1e-5 this degenerates to hard ops and matches field_is_solid's sign.
float field_distance(const BuildBuffer& buf, size_t opBegin, size_t opEnd,
                     float k, const Vector3& worldPoint) {
    float field = 1e9f;
    bool haveAny = false;

    // Accumulate one stage's per-brush distances, then fold and reset.
    std::vector<float> vals;
    vals.reserve(16);
    float stageMin = 1e9f;
    CsgOp stageOp = CsgOp::Union;
    bool stageOpen = false;

    auto foldStage = [&]() {
        if (!stageOpen || vals.empty()) { vals.clear(); stageOpen = false; return; }
        // smin_set (surface.c:1345): f = fmin - k*ln(sum exp(-(f_i-fmin)/k)).
        float d = stageMin;
        if (k > 1e-5f && vals.size() > 1) {
            float sum = 0.0f;
            for (float v : vals) sum += expf(-(v - stageMin) / k);
            d = stageMin - k * logf(sum);
        }
        if (!haveAny) { field = d; haveAny = true; }
        else switch (stageOp) {
            case CsgOp::Union:        field = fminf(field, d);  break;
            case CsgOp::Difference:   field = fmaxf(field, -d); break;
            case CsgOp::Intersection: field = fmaxf(field, d);  break;
        }
        vals.clear(); stageMin = 1e9f; stageOpen = false;
    };

    if (opEnd > buf.ops.size()) opEnd = buf.ops.size();
    for (size_t i = opBegin; i < opEnd; ++i) {
        const BuildOp& o = buf.ops[i];
        if (stageOpen && o.op != stageOp) foldStage();
        if (!stageOpen) { stageOp = o.op; stageOpen = true; }

        // Per-brush distance: identical metric to field_is_solid.
        Matrix inv = mat_invert(o.transform);
        Vector3 lpw = xf(inv, worldPoint);
        Vector3 lp = { lpw.x - o.center.x, lpw.y - o.center.y, lpw.z - o.center.z };
        float d;
        switch (o.kind) {
            case BrushKind::Sphere:   d = sdSphere(lp, o.radius); break;
            case BrushKind::Box:      d = sdBox(lp, o.halfExtents); break;
            case BrushKind::Capsule:  d = sdCapsule(lpw, o.center, o.segB, o.radius); break;
            case BrushKind::Cylinder: d = sdCappedCone(lpw, o.center, o.segB, o.radius, o.r1); break;
            default:                  d = sdSphere(lp, o.radius); break;
        }
        vals.push_back(d);
        if (d < stageMin) stageMin = d;
    }
    foldStage();
    return haveAny ? field : 1e9f;
}
```

Add `#include <vector>` to `csg_lowering.cpp` if not already present. Note the subtle difference from `field_is_solid`: that function folds op-by-op (equivalent for hard ops); this one folds by stage so smoothing blends within a stage exactly like the mesher.

- [ ] **Step 4: Run tests to verify they pass**

Run: `make -C MatterEngine3/tests run-iso`
Expected: PASS, zero failures (existing iso tests must stay green too).

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/csg_lowering.h MatterEngine3/src/csg_lowering.cpp MatterEngine3/tests/iso_primitive_tests.cpp
git commit -m "feat(dsl): field_distance oracle — staged smooth-min signed distance over a brush range"
```

---

### Task 2: `raycast()` DSL verb (DslState + binding + Part base)

**Files:**
- Modify: `MatterEngine3/src/dsl_state.h` (declare `raycast` near `set_last_op`, ~line 218)
- Modify: `MatterEngine3/src/dsl_state.cpp` (implement near `emit_voxel_*`, ~line 106)
- Modify: `MatterEngine3/src/dsl_bindings.cpp` (add `j_raycast` + `bind("__dsl_raycast", j_raycast, 6)` next to the other voxel verbs, ~line 779)
- Modify: `MatterEngine3/src/part_base.js.h` (add `raycast(o,d)` method)
- Test: `MatterEngine3/tests/script_host_tests.cpp`

**Interfaces:**
- Consumes (Task 1): `dsl::field_distance(buf, opBegin, opEnd, k, worldPoint)`.
- Produces (Task 3 relies on this exact JS surface):
  - `this.raycast(origin, dir)` → `{ point: [x,y,z], normal: [x,y,z] }` on hit, `null` on miss. `origin`/`dir` are 3-element arrays; `dir` need not be normalized (normalized internally). Valid only inside an open voxel session with ≥1 brush emitted in that session; otherwise `set_error` fail-closed (bake fails) and `null` is returned.
  - C++: `bool DslState::raycast(const Vector3& origin, const Vector3& dir, Vector3& outPoint, Vector3& outNormal);`

- [ ] **Step 1: Write the failing tests**

Append to `MatterEngine3/tests/script_host_tests.cpp` (match the file's existing `bake_source`/`CHECK` conventions — check how existing tests assert `r.ok` and error messages, e.g. the `Bad` class tests near line 169):

```cpp
// --- raycast(): in-session analytic surface probe ---------------------------

static void test_raycast_sphere_point_and_normal() {
    script_host::ScriptHost host;
    const char* src =
        "class Probe extends Part { static params={};\n"
        "  build(p){\n"
        "    this.beginVoxels(0.1);\n"
        "    this.sphere([0,1,0],0.5);\n"
        "    const h=this.raycast([3,1,0],[-1,0,0]);\n"
        "    if(!h) throw new Error('expected hit, got null');\n"
        "    if(Math.abs(h.point[0]-0.5)>0.02) throw new Error('bad point.x '+h.point[0]);\n"
        "    if(Math.abs(h.point[1]-1.0)>0.02) throw new Error('bad point.y '+h.point[1]);\n"
        "    if(Math.abs(h.point[2])>0.02) throw new Error('bad point.z '+h.point[2]);\n"
        "    if(h.normal[0]<0.95) throw new Error('bad normal.x '+h.normal[0]);\n"
        "    if(Math.abs(h.normal[1])>0.1||Math.abs(h.normal[2])>0.1) throw new Error('bad normal');\n"
        "    this.endVoxels();\n"
        "  } }";
    auto r = host.bake_source(src, "{}", {});
    CHECK(r.ok, "raycast sphere probe: point+normal within tolerance");
}

static void test_raycast_miss_returns_null() {
    script_host::ScriptHost host;
    const char* src =
        "class Probe extends Part { static params={};\n"
        "  build(p){\n"
        "    this.beginVoxels(0.1);\n"
        "    this.sphere([0,1,0],0.5);\n"
        "    const h=this.raycast([3,5,0],[-1,0,0]);\n"   // ray passes 4 units above
        "    if(h!==null) throw new Error('expected null miss');\n"
        "    this.endVoxels();\n"
        "  } }";
    auto r = host.bake_source(src, "{}", {});
    CHECK(r.ok, "raycast miss returns null");
}

static void test_raycast_outside_session_fails_closed() {
    script_host::ScriptHost host;
    const char* src =
        "class Probe extends Part { static params={};\n"
        "  build(p){ this.raycast([3,0,0],[-1,0,0]); } }";
    auto r = host.bake_source(src, "{}", {});
    CHECK(!r.ok, "raycast outside a voxel session fails the bake");
    CHECK(r.error.message.find("raycast") != std::string::npos,
          "error message names raycast");
}

static void test_raycast_no_brushes_fails_closed() {
    script_host::ScriptHost host;
    const char* src =
        "class Probe extends Part { static params={};\n"
        "  build(p){ this.beginVoxels(0.1); this.raycast([3,0,0],[-1,0,0]); this.endVoxels(); } }";
    auto r = host.bake_source(src, "{}", {});
    CHECK(!r.ok, "raycast with no brushes in session fails the bake");
}

static void test_raycast_sees_difference_cut() {
    script_host::ScriptHost host;
    // Sphere r=0.5 at (0,1,0); difference box spans x in [0.4,0.9] so the cut
    // face is the plane x=0.4. A +x probe must now hit that plane, not x=0.5.
    const char* src =
        "class Probe extends Part { static params={};\n"
        "  build(p){\n"
        "    this.beginVoxels(0.1);\n"
        "    this.sphere([0,1,0],0.5);\n"
        "    this.box([0.65,1,0],[0.25,0.6,0.6]);\n"
        "    this.difference();\n"
        "    const h=this.raycast([3,1,0],[-1,0,0]);\n"
        "    if(!h) throw new Error('expected hit');\n"
        "    if(Math.abs(h.point[0]-0.4)>0.02) throw new Error('cut face not seen: x='+h.point[0]);\n"
        "    if(h.normal[0]<0.95) throw new Error('cut face normal not +x');\n"
        "    this.endVoxels();\n"
        "  } }";
    auto r = host.bake_source(src, "{}", {});
    CHECK(r.ok, "raycast sees an earlier difference cut");
}
```

Register the five tests wherever the file invokes its test list. If `BakeResult`'s error field differs from `r.error.message`, match the struct (see `script_host.h`).

- [ ] **Step 2: Run tests to verify they fail**

Run: `make -C MatterEngine3/tests run-script`
Expected: FAIL — the probe bakes fail with a JS error like `raycast is not a function` (base class has no such method yet), so `CHECK(r.ok, ...)` fails.

- [ ] **Step 3: Implement DslState::raycast**

`MatterEngine3/src/dsl_state.h` — declare in the public section near the other voxel-session queries:

```cpp
    // In-session surface probe: sphere-trace the analytic smooth-min field of
    // the brushes emitted SO FAR in the open voxel session (smoothing cursor at
    // call time). Fail-closed outside a session / with no brushes. Returns true
    // and fills outPoint/outNormal on a hit; false on a miss (no error).
    bool raycast(const Vector3& origin, const Vector3& dir,
                 Vector3& outPoint, Vector3& outNormal);
```

`MatterEngine3/src/dsl_state.cpp` — implement after `emit_voxel_segment` (include `csg_lowering.h` at the top of the file):

```cpp
bool DslState::raycast(const Vector3& origin, const Vector3& dir,
                       Vector3& outPoint, Vector3& outNormal) {
    if (session_ != Session::Voxels) {
        set_error("raycast outside an open voxel session");
        return false;
    }
    if (buffer_.ops.size() <= session_start_) {
        set_error("raycast with no brushes emitted in this session");
        return false;
    }
    float len = sqrtf(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
    if (len < 1e-9f) { set_error("raycast with zero-length direction"); return false; }
    Vector3 d = { dir.x/len, dir.y/len, dir.z/len };

    const size_t b = session_start_, e = buffer_.ops.size();
    const float k = smoothing_;
    auto field = [&](const Vector3& p) {
        return field_distance(buffer_, b, e, k, p);
    };

    // Conservative sphere trace. The smin union under-reports distance (safe);
    // Difference/Intersection folds can over-report near carve boundaries, so
    // scale steps down and cap them. Bisect once a sign change brackets the hit.
    const float kStepScale = 0.7f;
    const float kMaxStep   = 0.5f;
    const float kMaxT      = 100.0f;
    const int   kMaxSteps  = 512;
    const float kEps       = 1e-4f;

    float t = 0.0f;
    Vector3 p = origin;
    float fd = field(p);
    if (fd <= kEps) {
        // Started on/inside the surface: report the origin itself.
        outPoint = origin;
    } else {
        float tPrev = t, fdPrev = fd;
        bool hit = false;
        for (int i = 0; i < kMaxSteps && t < kMaxT; ++i) {
            float step = fd * kStepScale;
            if (step > kMaxStep) step = kMaxStep;
            if (step < kEps)     step = kEps;
            tPrev = t; fdPrev = fd;
            t += step;
            p = { origin.x + d.x*t, origin.y + d.y*t, origin.z + d.z*t };
            fd = field(p);
            if (fd <= 0.0f) { hit = true; break; }
        }
        if (!hit) return false;   // miss: not an error
        // Bisection refine within [tPrev, t].
        float lo = tPrev, hi = t;
        (void)fdPrev;
        for (int i = 0; i < 32; ++i) {
            float mid = 0.5f * (lo + hi);
            Vector3 mp = { origin.x + d.x*mid, origin.y + d.y*mid, origin.z + d.z*mid };
            if (field(mp) > 0.0f) lo = mid; else hi = mid;
        }
        float tHit = 0.5f * (lo + hi);
        outPoint = { origin.x + d.x*tHit, origin.y + d.y*tHit, origin.z + d.z*tHit };
    }

    // Central-difference gradient, normalized outward.
    const float h = fmaxf(1e-3f, 0.25f * spacing_);
    Vector3 g = {
        field({outPoint.x + h, outPoint.y, outPoint.z}) - field({outPoint.x - h, outPoint.y, outPoint.z}),
        field({outPoint.x, outPoint.y + h, outPoint.z}) - field({outPoint.x, outPoint.y - h, outPoint.z}),
        field({outPoint.x, outPoint.y, outPoint.z + h}) - field({outPoint.x, outPoint.y, outPoint.z - h}),
    };
    float gl = sqrtf(g.x*g.x + g.y*g.y + g.z*g.z);
    if (gl < 1e-9f) { outNormal = { -d.x, -d.y, -d.z }; }
    else            { outNormal = { g.x/gl, g.y/gl, g.z/gl }; }
    return true;
}
```

`MatterEngine3/src/dsl_bindings.cpp` — add near the other voxel verbs:

```cpp
static JSValue j_raycast(JSContext* c, JSValueConst, int, JSValueConst* a){
    Vector3 hit{}, nrm{};
    bool ok = state_of(c)->raycast(
        {(float)argd(c,a[0]),(float)argd(c,a[1]),(float)argd(c,a[2])},
        {(float)argd(c,a[3]),(float)argd(c,a[4]),(float)argd(c,a[5])},
        hit, nrm);
    if (!ok) return JS_NULL;   // miss OR fail-closed error (bake fails anyway)
    JSValue pt = JS_NewArray(c);
    JS_SetPropertyUint32(c, pt, 0, JS_NewFloat64(c, hit.x));
    JS_SetPropertyUint32(c, pt, 1, JS_NewFloat64(c, hit.y));
    JS_SetPropertyUint32(c, pt, 2, JS_NewFloat64(c, hit.z));
    JSValue nm = JS_NewArray(c);
    JS_SetPropertyUint32(c, nm, 0, JS_NewFloat64(c, nrm.x));
    JS_SetPropertyUint32(c, nm, 1, JS_NewFloat64(c, nrm.y));
    JS_SetPropertyUint32(c, nm, 2, JS_NewFloat64(c, nrm.z));
    JSValue obj = JS_NewObject(c);
    JS_SetPropertyStr(c, obj, "point", pt);
    JS_SetPropertyStr(c, obj, "normal", nm);
    return obj;
}
```

and in the bind list (next to `__dsl_op`):

```cpp
    bind("__dsl_raycast", j_raycast, 6);
```

`MatterEngine3/src/part_base.js.h` — add to the Part class (after `smoothing`):

```js
  raycast(o,d)           { return __dsl_raycast(o[0],o[1],o[2], d[0],d[1],d[2]); }
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `make -C MatterEngine3/tests run-script && make -C MatterEngine3/tests run-iso`
Expected: PASS, zero failures, existing tests green.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/dsl_state.h MatterEngine3/src/dsl_state.cpp MatterEngine3/src/dsl_bindings.cpp MatterEngine3/src/part_base.js.h MatterEngine3/tests/script_host_tests.cpp
git commit -m "feat(dsl): raycast() — in-session analytic surface probe (point + normal)"
```

---

### Task 3: Rock.js rewrite — ellipsoid body + surface-relative facet cuts

**Files:**
- Modify: `MatterEngine3/examples/world_demo/schemas/Rock.js` (full rewrite)
- Test: `make -C MatterEngine3/tests run-meadow-check` (bake gate) — no new C++ tests

**Interfaces:**
- Consumes (Task 2): `this.raycast(origin, dir)` → `{point, normal} | null`; `shared-lib/vecmath` (`add`, `sub`, `scale`, `normalize`, `length`); existing verbs `pushMatrix/translate/rotateX/rotateY/scale/sphere/box/difference/lookAt/popMatrix`.
- Produces: `Rock` part with unchanged public contract (`static params = { seed: 0 }`, same module name, retopo modifier stack preserved) — Meadow needs no changes.

- [ ] **Step 1: Rewrite Rock.js**

Replace the entire contents of `MatterEngine3/examples/world_demo/schemas/Rock.js`:

```js
import { rng } from 'shared-lib/rng';
import { add, sub, scale as vscale, normalize, length } from 'shared-lib/vecmath';

// A ~1-unit SDF boulder. Body: 4-7 ellipsoid blobs (rotate+scale+sphere)
// sharing a dominant horizontal axis, squashed in Y so the mass reads as a
// settled boulder. Facets: 5-9 plane cuts placed via raycast() surface probes
// — each cut shaves a controlled depth below a real surface point, oriented
// by the (jittered) surface normal, so cuts can never gouge the core. One
// baked variant per seed; Meadow instances with random yaw/scale, sunk ~15%.
class Rock extends Part {
  static params = { seed: 0 };

  build(p) {
    const r = rng(1000 + p.seed);
    this.beginModifier();
    this.beginVoxels(0.10);
    this.fill(MAT.rock);
    this.smoothing(0.06);

    // Dominant axis + global anisotropy: settled/bedded, not potato-round.
    const yaw = r.range(0, Math.PI * 2);
    const axis = [Math.cos(yaw), 0, Math.sin(yaw)];
    const stretch = r.range(1.1, 1.5);
    const squashY = r.range(0.65, 0.9);

    const blobs = 4 + r.int(4); // 4-7 ellipsoids
    for (let i = 0; i < blobs; ++i) {
      const along = r.range(-0.35, 0.35);
      const c = add(vscale(axis, along * stretch),
                    [r.range(-0.12, 0.12), 0.42 + r.range(-0.10, 0.14), r.range(-0.12, 0.12)]);
      this.pushMatrix();
      this.translate(c[0], c[1], c[2]);
      this.rotateY(yaw + r.range(-0.4, 0.4));
      this.rotateX(r.range(-0.25, 0.25));
      this.scale(stretch * r.range(0.85, 1.15),
                 squashY * r.range(0.85, 1.15),
                 r.range(0.85, 1.15));
      this.sphere([0, 0, 0], r.range(0.28, 0.5));
      this.popMatrix();
    }

    // Facet cuts: probe the real surface, shave a shallow slice behind it.
    const C = [0, 0.42, 0];            // body centroid estimate (probe target)
    const B = 2.0;                     // cut-box half extent (acts as a plane)
    const cuts = 5 + r.int(5);         // 5-9
    for (let i = 0; i < cuts; ++i) {
      // Direction biased to sides/top (bottoms are sunk into the ground).
      const az = r.range(0, Math.PI * 2);
      const el = r.range(-0.15, 0.95);
      const horiz = Math.sqrt(Math.max(0, 1 - el * el));
      const dir = [Math.cos(az) * horiz, el, Math.sin(az) * horiz];

      const hit = this.raycast(add(C, vscale(dir, 3)), vscale(dir, -1));
      if (!hit) continue;

      // Cut-plane normal: surface normal jittered up to ~25 degrees.
      const m = normalize(add(hit.normal,
        [r.range(-0.45, 0.45), r.range(-0.45, 0.45), r.range(-0.45, 0.45)]));

      // Depth below the surface point, clamped so the plane keeps >=55% of
      // the centroid->surface distance (structurally no L-shape gouges).
      const hitDist = length(sub(hit.point, C));
      const t = Math.min(r.range(0.03, 0.12), 0.45 * hitDist);

      // Large box whose face lies on the plane through (hit.point - m*t):
      // center the box at q + m*B and aim +Z along -m via lookAt.
      const q = sub(hit.point, vscale(m, t));
      const c = add(q, vscale(m, B));
      const up = Math.abs(m[1]) > 0.95 ? [1, 0, 0] : [0, 1, 0];
      this.pushMatrix();
      this.translate(c[0], c[1], c[2]);
      this.lookAt(sub(c, m), up);
      this.rotateZ(r.range(0, Math.PI * 2)); // roll: varies facet intersections
      this.box([0, 0, 0], [B, B, B]);
      this.difference();
      this.popMatrix();
    }

    this.endVoxels();
    this.endModifier([
      { retopo: { target_ratio: 1.0, iterations: 3, seed: 42, timeout_seconds: 60 } },
    ]);
  }
}
```

Geometry check for the cut box (why one face lands on the plane): after `translate(c)` + `lookAt(c - m)`, the frame's +Z axis is `-m`, so the box face at local `z = -B` sits at world `c + m*B`... take the face at local `z = +B`: world `c + (+Z)*B = c - m*B = q`. That face is the plane through `q` with normal `m`; the box body extends away from the rock. `rotateZ` rolls the box about the plane normal, which only changes where the square face's edges land (facet-intersection variety), never the plane itself.

- [ ] **Step 2: Bake-gate the schema**

Run: `make -C MatterEngine3/tests run-meadow-check`
Expected: PASS — Meadow bakes with the new Rock (this gate exercises Rock through the real bake pipeline). If the gate FATALs on JS errors, fix Rock.js (typical culprits: import name collisions — note `scale` is both a vecmath export and a Part method, hence the `vscale` alias).

Also run: `make -C MatterEngine3/tests run-script` (unchanged, fast — confirms no part_base regressions).

- [ ] **Step 3: Commit**

```bash
git add MatterEngine3/examples/world_demo/schemas/Rock.js
git commit -m "feat(rock): ellipsoid blob body + raycast-placed facet cuts (0.10 spacing, 0.06 smoothing)"
```

---

### Task 4: Final gates — full test pass, Windows rebuild, visual sweep

**Files:**
- No source changes expected (fixes only if gates fail).
- Read: `MatterViewer/tools/viewer_shots.sh` (usage header) before running it.

**Interfaces:**
- Consumes: everything above; `MatterViewer` Makefile targets (`make`, `make windows`); FIFO viewer control (`MATTER_CMD_FIFO`).
- Produces: screenshots under `/tmp/rock_shots/` for human review; green test gates.

- [ ] **Step 1: Full headless test pass**

Run: `make -C MatterEngine3/tests run-iso run-script run-meadow-check run-meadow`
Expected: PASS. (`run-asyncbake`+autoremesher combination has a known pre-existing segfault — not part of this gate.)

- [ ] **Step 2: Rebuild the Linux viewer and capture before/after rock screenshots**

```bash
make -C MatterEngine3 && make -C MatterViewer
mkdir -p /tmp/rock_shots
# Read tools/viewer_shots.sh usage first; drive shots of Meadow rocks:
cd MatterViewer && GALLIUM_DRIVER=d3d12 tools/viewer_shots.sh <args per script usage — camera parked at 2-3 rock instances, several seeds> 
```
Expected: PNG shots in `/tmp/rock_shots/`, viewer self-terminated (verify no `viewer` process remains). The human partner reviews the shots — faceted, bedded boulders with no L-shape gouges is the acceptance bar. Iterate Rock.js tuning constants (cut count, depth range, smoothing) if the look is off, re-shooting between edits (use the FIFO `reload` command rather than relaunching — shader warm-up is ~60s).

- [ ] **Step 3: Clean-rebuild the Windows viewer**

Headers changed (`csg_lowering.h`, `dsl_state.h`), so clear ALL Windows object files first (no header dep tracking — stale objs cause wandering silent crashes), then:

Run: `make -C MatterViewer windows` (after clearing its obj dir per the Makefile's layout)
Expected: `viewer.exe` links clean.

- [ ] **Step 4: Commit any tuning changes**

```bash
git add MatterEngine3/examples/world_demo/schemas/Rock.js
git commit -m "tune(rock): facet depth/count/smoothing after visual sweep"
```
(Skip if Step 2 needed no tuning.)

---

## Self-Review Notes

- Spec coverage: §1 raycast verb → Tasks 1-2; §2 ellipsoid body → Task 3; §3 facet cuts → Task 3; §4 tuning → Tasks 3-4; Testing section → Tasks 1, 2, 4. Non-goals (multi-session meshes, shatter clusters, Pebble) intentionally absent.
- The `field_distance` staged fold intentionally differs from `field_is_solid`'s op-by-op fold; identical for hard ops (min/max associativity), required for smoothing fidelity. Covered by `test_field_distance_sign_matches_oracle`.
- Names used across tasks were cross-checked: `field_distance(buf, opBegin, opEnd, k, worldPoint)`, `DslState::raycast(origin, dir, outPoint, outNormal)`, JS `raycast(o, d)` → `{point, normal}|null`.
