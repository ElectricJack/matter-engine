# LOD-Aware Instanced Children Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Flattened parts can keep opted-in children (e.g. TreeBranch under Tree) as shared instanced BLAS refs at fine LOD levels and inline them from their coarse LODs at coarse levels, cutting flatten input ~900k → tens of thousands of tris and de-duplicating branch geometry across tree variants.

**Architecture:** A `placeChild(module, params, { instanced: true, inlineBelowPx })` opt-in is persisted as a hints sidecar next to the baked part. The flattener splits the cluster ladder into a *fine segment* (trunk only, hinted children excluded, kept as `FlatInstanceRef`s with an `inline_cutover` threshold) and a *coarse segment* (trunk merged with each child's already-baked coarse LOD, double-decimated under a split error budget). The runtime resolver compares each instance's projected size against the cutover: above it, emit trunk (segment 0) + one child instance per ref; below it, emit the single merged instance (segment 1). The GPU culler gates cluster subranges per segment on the CPU side — zero shader changes.

**Tech Stack:** C++17 (MatterEngine3 kernel), QuickJS-ng DSL bindings, existing headless test suites (`MatterEngine3/tests`), GL 4.6 GPU tests via Mesa d3d12.

**Spec:** `docs/superpowers/specs/2026-07-10-lod-aware-instanced-children-design.md`

## Global Constraints

- `GALLIUM_DRIVER=d3d12` must be set for every GPU test / viewer run (WSLg llvmpipe is GL 4.5 and FATALs the gate).
- After any engine header/struct change, rebuild the Windows viewer with **clean objects**: `make -C MatterViewer clean-windows-objs 2>/dev/null; make -C MatterViewer windows` (no header dep tracking on that target — stale objs cause wandering crashes). If no `clean-windows-objs` target exists, delete the Windows `*.obj` output dir by hand before `make windows`.
- Live viewer validation must use self-terminating FIFO scripts (`tools/viewer_shots.sh` pattern: FIFO `quit` + wait + kill trap). Never leave a viewer window open.
- Run only the targeted suite(s) named in each task; the full sweep (`./build-all.sh test`) is the final gate only (Task 13).
- Known **pre-existing** failures, not caused by this work: `run-example` (resolve_hash mismatch), `run-graph-integration` (missing Trunk.js fixture), `run-asyncbake`+autoremesher segfault. Do not chase these.
- Flat artifact goes v5 → **v6** with **no back-compat loader** — the existing peek/auto-regen path re-flattens stale artifacts. One-time re-flatten of world_demo flats happens implicitly on first run after merge.
- Engine default `inlineBelowPx = 64` (applied in the JS binding when `instanced: true` and the field is absent).
- `placeChild` options are the **third** JS argument; the second argument stays params-JSON (it feeds the composite child-hash lookup and must not be disturbed).
- Any *new* `*_CPP` variable in `MatterEngine3/tests/Makefile` must include `$(PF_CPP)` (dsl_bindings.cpp unconditionally installs pf bindings). This plan adds **no** new test binaries, so this should not arise.
- Threshold/ladder math reference: `pixel_angle` ≈ 0.00145417 (1.047/720), `pixel_budget` = 1.0, level-i stored threshold = `pixel_budget × pixel_angle × radius_divisor[i]`, nominal ladder thresholds ≈ {0.74453, 0.37227, 0.18613, 0.09307, 0.04653, 0.02327, 0.01163, 0.00582, 0.00291}, last level 0.
- Out of scope (Jack, 2026-07-10): fixing per-instance-scale LOD selection (`bound_radius` is per part hash, lod_select.cpp:52).

---

### Task 1: `placeChild` options — DSL plumbing

**Files:**
- Modify: `MatterEngine3/src/dsl_state.h` (~:247 ChildPlacement, ~:278 placeChild decl)
- Modify: `MatterEngine3/src/dsl_state.cpp` (~:231-245 placeChild impl)
- Modify: `MatterEngine3/src/dsl_bindings.cpp` (~:170-192 j_placeChild)
- Modify: `MatterEngine3/src/part_base.js.h` (~:35 placeChild JS wrapper)
- Test: `MatterEngine3/tests/script_host_tests.cpp`

**Interfaces:**
- Consumes: existing `dsl::DslState::placeChild(module, params, params_len)` and `ChildPlacement { uint64_t hash; float transform[16]; }`.
- Produces: `ChildPlacement` gains `bool instanced = false; float inline_below_px = 0.0f;`. `DslState::placeChild` signature becomes `void placeChild(const std::string& module, const void* params = nullptr, size_t params_len = 0, bool instanced = false, float inline_below_px = 0.0f);`. JS surface: `this.placeChild('Mod', paramsOrNull, { instanced: true, inlineBelowPx: 32 })` — third arg optional; `instanced: true` without `inlineBelowPx` defaults to 64.

- [ ] **Step 1: Write the failing test**

In `MatterEngine3/tests/script_host_tests.cpp`, add near the other direct-DslState tests (follow the existing `CHECK` macro style from `check.h`):

```cpp
static void test_placechild_instanced_flags() {
    dsl::DslState st;
    st.placeChild("A", nullptr, 0);                    // plain
    st.placeChild("B", nullptr, 0, true, 64.0f);       // instanced default px
    st.placeChild("C", nullptr, 0, true, 32.0f);       // instanced custom px
    const auto& kids = st.children();
    CHECK(kids.size() == 3);
    CHECK(!kids[0].instanced);
    CHECK(kids[0].inline_below_px == 0.0f);
    CHECK(kids[1].instanced);
    CHECK(kids[1].inline_below_px == 64.0f);
    CHECK(kids[2].instanced);
    CHECK(kids[2].inline_below_px == 32.0f);
}
```

Register `test_placechild_instanced_flags();` in `main()` alongside the other tests.

- [ ] **Step 2: Run test to verify it fails**

Run: `make -C MatterEngine3/tests run-script`
Expected: compile FAILURE — `placeChild` has no 4th/5th parameter, `ChildPlacement` has no `instanced` member.

- [ ] **Step 3: Implement**

`dsl_state.h` — extend the struct (~:247):

```cpp
struct ChildPlacement {
    uint64_t hash;
    float transform[16];
    bool instanced = false;
    float inline_below_px = 0.0f;
};
```

and the declaration (~:278):

```cpp
void placeChild(const std::string& module, const void* params = nullptr,
                size_t params_len = 0, bool instanced = false,
                float inline_below_px = 0.0f);
```

`dsl_state.cpp` (~:231-245) — add the two parameters to the definition and, where the `ChildPlacement p` is filled, add:

```cpp
p.instanced = instanced;
p.inline_below_px = inline_below_px;
```

`dsl_bindings.cpp` j_placeChild (~:170-192) — parse the options object **before** the existing params branch, then thread the two values through **both** existing `placeChild` call sites (the params branch and the no-params branch):

```cpp
bool instanced = false;
double inline_px = 0.0;
if (n > 2 && JS_IsObject(a[2])) {
    JSValue vi = JS_GetPropertyStr(c, a[2], "instanced");
    instanced = JS_ToBool(c, vi) > 0;
    JS_FreeValue(c, vi);
    JSValue vp = JS_GetPropertyStr(c, a[2], "inlineBelowPx");
    if (!JS_IsUndefined(vp) && !JS_IsNull(vp)) JS_ToFloat64(c, &inline_px, vp);
    JS_FreeValue(c, vp);
    if (instanced && inline_px <= 0.0) inline_px = 64.0;   // engine default
}
```

then each call becomes e.g. `state->placeChild(mod, buf, len, instanced, (float)inline_px);` / `state->placeChild(mod, nullptr, 0, instanced, (float)inline_px);`.

`part_base.js.h` (~:35) — forward the third argument:

```js
placeChild(module, params, opts) { __dsl_placeChild(module, params, opts); }
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make -C MatterEngine3/tests run-script`
Expected: PASS (all script suite tests green, including the new one).

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/dsl_state.h MatterEngine3/src/dsl_state.cpp MatterEngine3/src/dsl_bindings.cpp MatterEngine3/src/part_base.js.h MatterEngine3/tests/script_host_tests.cpp
git commit -m "feat(dsl): placeChild third-arg options { instanced, inlineBelowPx } with 64px default"
```

---

### Task 2: Flatten-hints sidecar (save/load)

**Files:**
- Modify: `MatterEngine3/src/part_asset_v2.h`
- Modify: `MatterEngine3/src/part_asset_v2.cpp`
- Test: `MatterEngine3/tests/part_asset_v2_tests.cpp` (the run-partv2 suite source)

**Interfaces:**
- Consumes: existing `cache_path(...)` naming conventions (`parts/%016llx....`, part_asset_v2.cpp:82-100) and the text-sidecar style of `load_lod_sidecar` (:102).
- Produces:
  - `std::string part_asset::cache_path_hints(uint64_t hash);` → `"parts/<16hex>.hints"`
  - `struct part_asset::FlattenHints { std::map<uint32_t, float> child_px; };` (key = child index in bake order, value = inlineBelowPx)
  - `bool part_asset::save_flatten_hints(const std::string& path, const FlattenHints&);`
  - `bool part_asset::load_flatten_hints(const std::string& path, FlattenHints&);` (returns false if file absent; hints empty)
  - File format: one text line per entry, `"<child_index> <px>\n"`.

- [ ] **Step 1: Write the failing test**

In the run-partv2 test source, add:

```cpp
static void test_flatten_hints_round_trip() {
    const uint64_t h = 0xABCD0000ABCD0000ull;
    std::string p = std::string(kCacheRoot) + "/" + part_asset::cache_path_hints(h);
    CHECK(part_asset::cache_path_hints(h) == "parts/abcd0000abcd0000.hints");

    part_asset::FlattenHints out;
    out.child_px[1] = 64.0f;
    out.child_px[5] = 32.0f;
    CHECK(part_asset::save_flatten_hints(p, out));

    part_asset::FlattenHints in;
    CHECK(part_asset::load_flatten_hints(p, in));
    CHECK(in.child_px.size() == 2);
    CHECK(in.child_px.at(1) == 64.0f);
    CHECK(in.child_px.at(5) == 32.0f);

    part_asset::FlattenHints missing;
    CHECK(!part_asset::load_flatten_hints(p + ".nope", missing));
    CHECK(missing.child_px.empty());
}
```

(Use whatever cache-root constant/tempdir helper the suite already uses; ensure the `parts/` subdir exists — mirror the existing fixture setup.) Register in `main()`.

- [ ] **Step 2: Run test to verify it fails**

Run: `make -C MatterEngine3/tests run-partv2`
Expected: compile FAILURE — `cache_path_hints` / `FlattenHints` undeclared.

- [ ] **Step 3: Implement**

`part_asset_v2.h` — add `#include <map>` and, near the other cache_path decls:

```cpp
std::string cache_path_hints(uint64_t resolved_hash);

struct FlattenHints {
    std::map<uint32_t, float> child_px;   // child index (bake order) -> inlineBelowPx
};
bool save_flatten_hints(const std::string& path, const FlattenHints& hints);
bool load_flatten_hints(const std::string& path, FlattenHints& out);
```

`part_asset_v2.cpp` — next to the other cache_path functions (:82-100):

```cpp
std::string cache_path_hints(uint64_t resolved_hash) {
    char buf[64];
    snprintf(buf, sizeof buf, "parts/%016llx.hints",
             (unsigned long long)resolved_hash);
    return buf;
}

bool save_flatten_hints(const std::string& path, const FlattenHints& hints) {
    std::ofstream out(path);
    if (!out) return false;
    for (const auto& [idx, px] : hints.child_px)
        out << idx << " " << px << "\n";
    return (bool)out;
}

bool load_flatten_hints(const std::string& path, FlattenHints& out_hints) {
    std::ifstream in(path);
    if (!in) return false;
    uint32_t idx; float px;
    while (in >> idx >> px) out_hints.child_px[idx] = px;
    return true;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make -C MatterEngine3/tests run-partv2`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/part_asset_v2.h MatterEngine3/src/part_asset_v2.cpp MatterEngine3/tests/part_asset_v2_tests.cpp
git commit -m "feat(asset): flatten-hints sidecar (parts/<hash>.hints) save/load"
```

---

### Task 3: Bake writes hints sidecar

**Files:**
- Modify: `MatterEngine3/src/script_host.cpp` (~:1311-1335, after `r.written_path = path;`)
- Test: `MatterEngine3/tests/script_host_tests.cpp`

**Interfaces:**
- Consumes: Task 1's `ChildPlacement.instanced/.inline_below_px` via `state.children()`; Task 2's `save_flatten_hints` + `cache_path_hints`. `BakeOptions.parts_dir` (script_host.h) is where sidecars belong when set.
- Produces: after a successful `save_v2`, a `parts/<hash>.hints` file exists **iff** at least one child is instanced, containing one line per instanced child: `<bake-order index> <inline_below_px>`. Bake-order index i in `state.children()` matches the child order written into the v2 asset (same loop — verified).

- [ ] **Step 1: Write the failing test**

In `script_host_tests.cpp`, following the existing `bake_source(src, "{}", opts, ...)` end-to-end pattern (child modules registered the same way neighbouring tests do):

```cpp
static void test_bake_writes_flatten_hints() {
    const char* kid_src = "class Kid extends Part { build(p) { this.box([0,0,0],[1,1,1]); } }";
    const char* src =
        "class Root extends Part { build(p) {"
        "  this.box([0,0,0],[1,1,1]);"
        "  this.placeChild('Kid');"                                        // idx 0: plain
        "  this.placeChild('Kid', null, { instanced: true });"             // idx 1: default 64
        "  this.placeChild('Kid', null, { instanced: true, inlineBelowPx: 32 });" // idx 2: 32
        "} }";
    // ... bake via the suite's bake_source helper with a temp parts_dir,
    //     registering 'Kid' the way existing multi-part tests do ...
    uint64_t root_hash = /* from BakeResult */;
    part_asset::FlattenHints h;
    std::string hp = parts_dir + "/" + part_asset::cache_path_hints(root_hash);
    CHECK(part_asset::load_flatten_hints(hp, h));
    CHECK(h.child_px.size() == 2);
    CHECK(h.child_px.count(0) == 0);
    CHECK(h.child_px.at(1) == 64.0f);
    CHECK(h.child_px.at(2) == 32.0f);
}
```

(Adapt helper invocation to the suite's exact `bake_source(src, "{}", opts, &kid_hash, 1, &kid_module, nullptr)` signature.) Register in `main()`.

- [ ] **Step 2: Run test to verify it fails**

Run: `make -C MatterEngine3/tests run-script`
Expected: FAIL — `load_flatten_hints` returns false (no sidecar written).

- [ ] **Step 3: Implement**

`script_host.cpp`, immediately after `r.written_path = path;` in the save block (~:1311-1335):

```cpp
part_asset::FlattenHints hints;
{
    const auto& kids = state.children();
    for (size_t i = 0; i < kids.size(); ++i)
        if (kids[i].instanced)
            hints.child_px[(uint32_t)i] = kids[i].inline_below_px;
}
if (!hints.child_px.empty()) {
    std::string hpath = opts.parts_dir.empty()
        ? part_asset::cache_path_hints(hash)
        : opts.parts_dir + "/" + part_asset::cache_path_hints(hash);
    part_asset::save_flatten_hints(hpath, hints);
}
```

(Match the surrounding code's exact variable names for `state`, `opts`, `hash` — same ones used to compute `path` for save_v2.)

- [ ] **Step 4: Run test to verify it passes**

Run: `make -C MatterEngine3/tests run-script`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/script_host.cpp MatterEngine3/tests/script_host_tests.cpp
git commit -m "feat(bake): write flatten-hints sidecar for instanced placeChild children"
```

---

### Task 4: Flat artifact v6 — segment tag + inline cutover

**Files:**
- Modify: `MatterEngine3/src/part_asset_v2.h` (kFormatVersionFlat :25, FlatCluster :101, FlatInstanceRef :115-120)
- Modify: `MatterEngine3/src/part_asset_v2.cpp` (save_flat_v3 :432-467, load_flat_v3 :477-556)
- Test: extend `test_v3_round_trip` in `MatterEngine3/tests/part_flatten_tests.cpp` (:681) and the partv2 suite if it also round-trips flats

**Interfaces:**
- Consumes: existing `save_flat_v3` / `load_flat_v3` (names unchanged — only the format version constant moves to 6; auto-regen handles stale v5 files via the existing peek-mismatch path in flatten_one).
- Produces:
  - `kFormatVersionFlat = 6`
  - `FlatCluster` gains `uint32_t segment = 0;` (0 = fine, 1 = coarse; unsegmented artifacts write 0 everywhere)
  - `FlatInstanceRef` gains `float inline_cutover = 0.0f; float _pad = 0.0f;` with `static_assert(sizeof(FlatInstanceRef) == 80, "flat ref layout");` (`inline_cutover` in parent ladder-threshold units; 0 = never inline, i.e. budget-forced BOUNDARY)
  - Serialization: cluster body writes `segment` right after `aabb_max`; ref trailer appends `inline_cutover` after the transform (do **not** serialize `_pad`).

- [ ] **Step 1: Write the failing test**

Extend `test_v3_round_trip` (part_flatten_tests.cpp:681): on the written fixture, set distinct `segment` values on two clusters (0 and 1) and `inline_cutover = 0.575f` on one ref and `0.0f` on another before saving; after loading, assert all values round-trip:

```cpp
clusters[0].segment = 0;
clusters[1].segment = 1;
refs[0].inline_cutover = 0.575f;
refs[1].inline_cutover = 0.0f;
// ... save_flat_v3 / load_flat_v3 as the test already does ...
CHECK(loaded_clusters[0].segment == 0);
CHECK(loaded_clusters[1].segment == 1);
CHECK(loaded_refs[0].inline_cutover == 0.575f);
CHECK(loaded_refs[1].inline_cutover == 0.0f);
static_assert(sizeof(part_asset::FlatInstanceRef) == 80, "flat ref layout");
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make -C MatterEngine3/tests run-flatten`
Expected: compile FAILURE — no `segment` / `inline_cutover` members.

- [ ] **Step 3: Implement**

`part_asset_v2.h`:

```cpp
inline constexpr uint32_t kFormatVersionFlat = 6;

struct FlatCluster {
    float aabb_min[3];
    float aabb_max[3];
    uint32_t segment = 0;      // 0 = fine, 1 = coarse
    LodLevels lods;
};

struct FlatInstanceRef {
    uint64_t child_resolved_hash;
    float transform[16];
    float inline_cutover = 0.0f;  // parent ladder-threshold units; 0 = never inline
    float _pad = 0.0f;
};
static_assert(sizeof(FlatInstanceRef) == 80, "flat ref layout");
```

`part_asset_v2.cpp` `save_flat_v3` (:445-455): after the `put_bytes` of `aabb_max`, add `put<uint32_t>(body, c.segment);`. In the refs trailer (:460-464), after the transform bytes, add `put<float>(body, r.inline_cutover);` (skip `_pad`).

`load_flat_v3` (:513-538 / :544-554): after the aabb memcpy add `fc.segment = r.get<uint32_t>();`; after the ref transform memcpy add `ref.inline_cutover = r.get<float>();`.

- [ ] **Step 4: Run test to verify it passes**

Run: `make -C MatterEngine3/tests run-flatten && make -C MatterEngine3/tests run-partv2`
Expected: PASS on both.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/part_asset_v2.h MatterEngine3/src/part_asset_v2.cpp MatterEngine3/tests/part_flatten_tests.cpp
git commit -m "feat(asset): flat v6 — per-cluster segment tag + FlatInstanceRef.inline_cutover"
```

---

### Task 5: Cutover math helpers (header-only)

**Files:**
- Modify: `MatterEngine3/src/part_flatten.h`
- Test: `MatterEngine3/tests/part_flatten_tests.cpp`

**Interfaces:**
- Consumes: `FlattenTargets` (pixel_angle ≈ 0.00145417, pixel_budget = 1.0, radius_divisor ladder).
- Produces (header-only inlines — usable from part_store/gpu code without new link deps; add `#include <cmath>`):
  - `inline float transform_uniform_scale(const float t[16]);` — column-0 length of a row-major 4×4
  - `inline float ref_cutover_threshold(float inline_below_px, float parent_radius, float child_radius_local, float ref_scale, const FlattenTargets& t);` — spec formula `px × pa × pb × parent_radius / (child_radius_local × ref_scale)`; 0 on degenerate inputs
  - `inline int cutover_level_index(float cutover_threshold, const FlattenTargets& t);` — smallest ladder index i with `cutover ≥ pb·pa·div[i]`, else `div.size()` (i.e. L\*; levels 0..L\*−1 are fine)
  - `FlattenResult` gains `size_t fine_tris = 0; size_t coarse_input_tris = 0;`

- [ ] **Step 1: Write the failing test**

```cpp
static void test_cutover_helpers() {
    part_flatten::FlattenTargets t;   // defaults: pb=1, pa=1.047/720, div={512,256,...,2,1}? use actual defaults

    // Nominal ladder thresholds: thr[i] = pb*pa*div[i]; thr[0] ≈ 0.74453.
    CHECK(part_flatten::cutover_level_index(1.0f, t) == 0);
    CHECK(part_flatten::cutover_level_index(0.5f, t) == 1);
    CHECK(part_flatten::cutover_level_index(0.1f, t) == 3);
    CHECK(part_flatten::cutover_level_index(0.001f, t) == (int)t.radius_divisor.size());
    CHECK(part_flatten::cutover_level_index(0.0f, t) == (int)t.radius_divisor.size());

    // ref_cutover_threshold(64px, parent_r=10.7, child_r=1.732, scale=1) ≈ 0.575
    float c = part_flatten::ref_cutover_threshold(64.0f, 10.7f, 1.732f, 1.0f, t);
    CHECK(std::fabs(c - 0.575f) < 0.001f);
    // zero guards
    CHECK(part_flatten::ref_cutover_threshold(64.0f, 10.7f, 0.0f, 1.0f, t) == 0.0f);
    CHECK(part_flatten::ref_cutover_threshold(64.0f, 0.0f, 1.0f, 1.0f, t) == 0.0f);

    // uniform scale of a 0.35-scale row-major matrix
    float m[16] = {0.35f,0,0,0,  0,0.35f,0,0,  0,0,0.35f,0,  0,0,0,1};
    CHECK(std::fabs(part_flatten::transform_uniform_scale(m) - 0.35f) < 1e-6f);
}
```

Register in `main()` (part_flatten_tests.cpp:1424 area).

- [ ] **Step 2: Run test to verify it fails**

Run: `make -C MatterEngine3/tests run-flatten`
Expected: compile FAILURE — helpers undeclared.

- [ ] **Step 3: Implement**

In `part_flatten.h` (inside the `part_flatten` namespace, after `FlattenTargets`):

```cpp
inline float transform_uniform_scale(const float t[16]) {
    return std::sqrt(t[0]*t[0] + t[4]*t[4] + t[8]*t[8]);
}

// Spec: parent_cutover = px * pa * pb * parent_radius / (child_radius_local * ref_scale)
inline float ref_cutover_threshold(float inline_below_px, float parent_radius,
                                   float child_radius_local, float ref_scale,
                                   const FlattenTargets& t) {
    const float denom = child_radius_local * ref_scale;
    if (denom <= 0.0f || parent_radius <= 0.0f) return 0.0f;
    return inline_below_px * t.pixel_angle * t.pixel_budget * parent_radius / denom;
}

// Smallest ladder index whose nominal threshold is <= cutover; div.size() if none.
// Levels [0, L*) are the fine segment; [L*, end] coarse.
inline int cutover_level_index(float cutover_threshold, const FlattenTargets& t) {
    for (size_t i = 0; i < t.radius_divisor.size(); ++i)
        if (cutover_threshold >= t.pixel_budget * t.pixel_angle * t.radius_divisor[i])
            return (int)i;
    return (int)t.radius_divisor.size();
}
```

Add to `FlattenResult`:

```cpp
size_t fine_tris = 0;          // trunk-only QEM input (segmented flats)
size_t coarse_input_tris = 0;  // merged coarse-segment input
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make -C MatterEngine3/tests run-flatten`
Expected: PASS. If the exact expected indices differ (default divisor table), print the actual `pb*pa*div[i]` values once, correct the asserted constants to match the real table, and note the correction in the test comment — the *shape* (monotone mapping, 0 → coarsest) is the contract.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/part_flatten.h MatterEngine3/tests/part_flatten_tests.cpp
git commit -m "feat(flatten): cutover math helpers (scale, threshold, ladder index) + result counters"
```

---

### Task 6: Segmented flatten pipeline

**Files:**
- Modify: `MatterEngine3/src/part_flatten.cpp`
- Test: `MatterEngine3/tests/part_flatten_tests.cpp`

**Interfaces:**
- Consumes: Tasks 2/4/5 (hints load, v6 fields, helpers). Existing internals: `PartGeo` (:78), `Gatherer::load` (:367), `skeleton_gather` child loop (:254-268), `flatten_part_impl` (:612, skeleton_gather call :691), QEM ladder loop (`decimate_to_error`, `reproject_triex` via `MeshIndexed`, :813-820), threshold fill rule (:842-852), `mul16`, `xform_point`, `NormalMatF3`.
- Produces:
  - `PartGeo` gains `part_asset::FlattenHints hints;`, loaded in `Gatherer::load` after `geo->ok = true` (ignore missing-file return).
  - `Gatherer` gains `struct HintedRef { part_asset::FlatInstanceRef ref; float px; };` + `std::vector<HintedRef> hinted_refs_;` + `const std::vector<HintedRef>& hinted_refs() const`.
  - Hinted children are excluded from the trunk at **any depth** and recorded as HintedRefs (hinted wins over budget-BOUNDARY).
  - `flatten_part_impl`: if `hinted_refs()` non-empty after gather, branch to new static `flatten_segmented(...)`; the unhinted path stays byte-identical.
  - Artifact out: fine clusters (segment 0, trunk-only, ladder levels 0..L\*−1 as divisors) written **first**, then coarse clusters (segment 1, merged, divisors L\*..end); every hinted ref written with `inline_cutover` = unified (max) part cutover; budget-BOUNDARY refs keep 0.
  - Error budget: coarse level at divisor di has `eps_total = radius/div[di]`, QEM gets `eps_total − eps_child_used` (skip level if ≤ 0); coarse level 0 registered with `eps = eps_child_used` so its threshold lands ≤ cutover (continuity). Child source level per ref = `min(C, E)` where C = child level selected at the cutover px and E = coarsest child level j with `eps_child_local(j) × ref_scale ≤ eps_first_coarse / 2`.
  - `FlattenResult.fine_tris` = trunk tri count; `full_tris = coarse_input_tris` = merged tri count.

- [ ] **Step 1: Write the failing test**

Fixture (using the file's existing `save_fixture(hash, mat, lod_tri_sets, children)`, `quad_tris`, `set_translate`, `kCacheRoot` helpers): a parent `kParentHash = 0x2222000022220000` whose own geometry is a 2-tri quad trunk, with two children `kChildHash = 0x1111000011110000` (a small multi-LOD sphere-ish fixture, whatever `save_fixture` produces with 2+ LOD levels) placed at translate(+10,0,0) and translate(−10,0,0). Write a hints sidecar for the parent:

```cpp
part_asset::FlattenHints h;
h.child_px[0] = 64.0f;
h.child_px[1] = 64.0f;
part_asset::save_flatten_hints(
    std::string(kCacheRoot) + "/" + part_asset::cache_path_hints(kParentHash), h);
```

Then:

```cpp
static void test_flatten_segmented() {
    // ... fixture setup as above ...
    part_flatten::FlattenTargets t;
    auto res = part_flatten::flatten_part(kCacheRoot, kParentHash, t, ...);
    CHECK(res.ok);

    std::vector<part_asset::FlatCluster> cl; std::vector<part_asset::FlatInstanceRef> refs;
    // load_flat_v3 6-arg form into cl/refs (+ aabb, mat) ...

    bool has_fine = false, has_coarse = false;
    for (auto& c : cl) { if (c.segment == 0) has_fine = true; else has_coarse = true; }
    CHECK(has_fine && has_coarse);

    // Fine segment level 0 = trunk only = exactly the 2 quad tris.
    size_t fine_l0 = 0, coarse_l0 = 0;
    for (auto& c : cl) {
        if (c.segment == 0) fine_l0   += c.lods.levels[0].indices.size() / 3;
        else                coarse_l0 += c.lods.levels[0].indices.size() / 3;
    }
    CHECK(fine_l0 == 2);
    // Coarse level 0 = trunk + child coarse LODs: more than trunk, less than
    // trunk + 2 * child FULL-res (proves coarse source, not full-res gather).
    CHECK(coarse_l0 > 2);
    CHECK(coarse_l0 < 2 + 2 * child_full_res_tris);

    // Two refs, equal positive cutover (unified max).
    CHECK(refs.size() == 2);
    CHECK(refs[0].inline_cutover > 0.0f);
    CHECK(refs[0].inline_cutover == refs[1].inline_cutover);

    // fine/coarse counters populated
    CHECK(res.fine_tris == 2);
    CHECK(res.coarse_input_tris == coarse_l0_input /* == res.full_tris */);
}
```

Plus two guard tests:

```cpp
static void test_flatten_unhinted_unchanged() {
    // Same fixture WITHOUT the hints sidecar: artifact has zero segment-1
    // clusters and all refs inline_cutover == 0. Reuse/extend
    // test_flatten_deterministic (:203) to also cover the segmented path:
    // flatten twice with hints -> byte-identical output files.
}
```

(Exact tri counts for the coarse segment depend on QEM no-progress skips — assert **ranges**, not exact counts, except fine L0 == 2 which is structural.)

- [ ] **Step 2: Run test to verify it fails**

Run: `make -C MatterEngine3/tests run-flatten`
Expected: FAIL — no segment-1 clusters (hints ignored), refs cutover 0.

- [ ] **Step 3: Implement (part A — gather)**

`part_flatten.cpp`:

1. `PartGeo` (:78): add `part_asset::FlattenHints hints;`
2. `Gatherer::load` (:367): after `geo->ok = true;` add
   ```cpp
   part_asset::load_flatten_hints(
       cache_root_ + "/" + part_asset::cache_path_hints(hash), geo->hints);
   ```
   (return value ignored — absent sidecar means no hints). Leave the neutral-TriEx fallback block (:403-418) untouched; for NEW code below, factor a small `static TriEx neutral_triex_for(const Tri&)` helper with the same body.
3. Gatherer members:
   ```cpp
   struct HintedRef { part_asset::FlatInstanceRef ref; float px; };
   std::vector<HintedRef> hinted_refs_;
   const std::vector<HintedRef>& hinted_refs() const { return hinted_refs_; }
   ```
4. `skeleton_gather` child loop (:254-268) — switch to index-based so hints can key on child index; hinted check comes **before** the boundary check:
   ```cpp
   for (size_t ci = 0; ci < geo->children.size(); ++ci) {
       const auto& c = geo->children[ci];
       float child_world[16];
       mul16(world, c.transform, child_world);
       auto hit = geo->hints.child_px.find((uint32_t)ci);
       if (hit != geo->hints.child_px.end()) {
           HintedRef hr;
           hr.ref.child_resolved_hash = c.child_resolved_hash;
           std::memcpy(hr.ref.transform, child_world, 16 * sizeof(float));
           hr.px = hit->second;
           hinted_refs_.push_back(hr);
           continue;   // subtree excluded from trunk; hinted wins over BOUNDARY
       }
       if (self_is_boundary) { /* existing budget-BOUNDARY ref record; continue; */ }
       if (!skeleton_gather(c.child_resolved_hash, child_world, depth + 1,
                            err, aabb_min, aabb_max)) return false;
   }
   ```
   (`gather()` at :124 — the variants path — is untouched; variants and hints are mutually exclusive since variants are leaf schemas.)

- [ ] **Step 4: Implement (part B — segmented build)**

New statics in `part_flatten.cpp`:

```cpp
// lod_select.cpp:24-31 copy — lod_select.cpp is not in the FLATTEN link.
static int select_level_local(float size, const std::vector<float>& thr) {
    for (size_t i = 0; i < thr.size(); ++i)
        if (size >= thr[i]) return (int)i;
    return thr.empty() ? 0 : (int)thr.size() - 1;
}

struct ChildFlat {
    bool ok = false;
    float radius = 0.0f;
    float aabb_min[3], aabb_max[3];
    std::vector<float> thresholds;                 // merged per-level (max over clusters)
    std::vector<std::vector<Tri>>   level_tris;    // merged per-level geometry
    std::vector<std::vector<TriEx>> level_triex;
};

static bool load_child_flat(const std::string& cache_root, uint64_t child_hash,
                            const FlattenTargets& targets, ChildFlat& out,
                            std::string* err);
```

`load_child_flat`: peek the child flat; if missing or version ≠ v6, recursively `flatten_part(cache_root, child_hash, targets, ...)` first. Load via 6-arg `load_flat_v3`. If any cluster has `segment == 1` (child itself segmented), restrict the merged view to segment-1 clusters only. Level count = max over used clusters; per level `li`, concat each cluster's level `min(li, cluster_levels-1)`; threshold per level = max over clusters; synthesize neutral TriEx via `neutral_triex_for` on size mismatch. AABB/radius from **all** clusters (half diagonal).

`flatten_segmented(cache_root, root_hash, targets, g, world_aabb_min, world_aabb_max)`:

1. Load unique child flats (`std::map<uint64_t, ChildFlat>`).
2. Extend the world AABB by the 8 transformed corners of each hinted child's AABB; `radius` = half diagonal of the extended AABB.
3. Per ref: `ref_scale = transform_uniform_scale(hr.ref.transform)`; `cut = ref_cutover_threshold(hr.px, radius, child.radius, ref_scale, targets)`. Unified cutover = **max** over refs. `L* = cutover_level_index(unified, targets)`. `eps_first_coarse = (L* < (int)targets.radius_divisor.size()) ? radius / targets.radius_divisor[L*] : FLT_MAX;`
4. Per ref, child source level:
   - `C = select_level_local(hr.px * targets.pixel_angle * targets.pixel_budget, child.thresholds)`
   - `E` = coarsest child level j with `eps_child_local(j) * ref_scale <= eps_first_coarse / 2`, where `eps_child_local(j) = (j == 0) ? 0 : child.radius * targets.pixel_budget * targets.pixel_angle / child.thresholds[j-1]` (treat `thr <= 0` as huge eps → excluded)
   - `src = std::min(C, E)`; `eps_child_used = max over refs of eps_child_local(src) * ref_scale`.
5. Trunk: materialize the full gathered trunk (identity-order `materialize_range` over all gathered tris — justified: the trunk is small by construction when hints exist). `res.fine_tris = trunk.size();`
6. Merged: trunk + per ref, each tri of `child.level_tris[src]` with vertices through `xform_point(hr.ref.transform, v)` (recompute centroid) and TriEx normals through `NormalMatF3(hr.ref.transform)`. `res.full_tris = res.coarse_input_tris = merged.size();`
7. `build_segment(tris, triex, div_lo, div_hi, eps_base, segment_tag)` lambda (shares the existing blas/register_level/find_blas_idx machinery): centroids → `split_centroids` → per-cluster permute-copy slice + AABB → register raw level with `eps = eps_base` → for `di` in `[div_lo, div_hi)`: `eps_total = radius / targets.radius_divisor[di]`, `eps_qem = eps_total - eps_base`, `continue` if ≤ 0; `decimate_to_error(ctris, eps_qem, false)`; no-progress → continue; reproject TriEx via `MeshIndexed` exactly as :813-820; register with `eps_total`; stop at `min_level_tris`. Threshold fill = the existing rule (:842-852) applied over this segment's level metas. Set `FlatCluster.segment = segment_tag` on every produced cluster.
8. `build_segment(trunk, 0, L*, 0.0f, 0)` first, then `build_segment(merged, L*, div.size(), eps_child_used, 1)` — fine clusters land first in the artifact. Edge cases: `L* == 0` → fine segment = raw trunk level only; `L* == div.size()` → coarse = merged raw only.
9. Fill every hinted ref's `inline_cutover` = unified cutover; append hinted refs to the gatherer's budget-BOUNDARY `instance_refs()` (those keep cutover 0); `save_flat_v3`.

Hook: in `flatten_part_impl` (:612) after the `skeleton_gather` call (:691):

```cpp
if (!g.hinted_refs().empty())
    return flatten_segmented(cache_root, root_hash, targets, g,
                             world_aabb_min, world_aabb_max);
```

- [ ] **Step 5: Run tests**

Run: `make -C MatterEngine3/tests run-flatten`
Expected: PASS — new segmented tests, determinism test, and all existing flatten tests (unhinted path byte-identical).

- [ ] **Step 6: Commit**

```bash
git add MatterEngine3/src/part_flatten.cpp MatterEngine3/src/part_flatten.h MatterEngine3/tests/part_flatten_tests.cpp
git commit -m "feat(flatten): segmented ladder — trunk-only fine levels + child-coarse-sourced coarse levels with split error budget"
```

---

### Task 7: Loader — segments, refs, LOD table

**Files:**
- Modify: `MatterEngine3/src/render/part_store.h`
- Modify: `MatterEngine3/src/render/part_store.cpp` (load_flat v-current path :89-217; v2 legacy :290; compositional :399; get_or_load :308-321; part_lod_table :413-418)
- Modify: `MatterEngine3/src/lod_select.h` / `MatterEngine3/src/lod_select.cpp` (PartLod)
- Test: `MatterEngine3/tests/viewer_logic_tests.cpp` (pattern: test_partstore_cluster_loading :802)

**Interfaces:**
- Consumes: Task 4 v6 fields; Task 5 `transform_uniform_scale` (part_store.cpp needs `#include "part_flatten.h"`).
- Produces:
  - `LoadedPart` gains `std::vector<part_asset::FlatInstanceRef> flat_refs;` (only refs with cutover > 0), `float inline_cutover = 0.0f;` (max over refs), `uint32_t fine_cluster_count = 0;`
  - Loader invariant: clusters `stable_partition`ed fine-first before registration; `fine_cluster_count` counts non-skipped **pushed** fine clusters when segmented (the empty-cluster skip at :206 would otherwise desync), and equals `lp.clusters.size()` when unsegmented (also on the v2-legacy and compositional paths).
  - When segmented, the legacy whole-part LOD view (:128-157) is built from **segment-1 clusters only** (coarse thresholds/levels — that's the merged representation).
  - `bound_radius` computed from ALL clusters.
  - `get_or_load` recursively loads each `flat_refs` child after inserting the parent.
  - `lod_select::PartLod` gains `float inline_cutover = 0.0f; std::vector<PartLodRef> refs;` with `struct PartLodRef { uint64_t child_hash; float rel_transform[16]; float child_scale; };` — filled by `part_lod_table`.

- [ ] **Step 1: Write the failing test**

In `viewer_logic_tests.cpp` (mirror `test_partstore_cluster_loading` :802 fixture style — write a v6 flat with 1 fine cluster + 2 coarse clusters + 2 refs, one cutover 0.575 / one cutover 0):

```cpp
static void test_partstore_segmented_loading() {
    // ... write fixture flat (v6): clusters segments {0,1,1}, refs cutovers {0.575f, 0.0f},
    //     plus a trivial child flat for the cutover ref's child hash ...
    render::PartStore ps(cache_root);
    CHECK(ps.get_or_load(parent_hash));
    const auto* lp = ps.find(parent_hash);
    CHECK(lp);
    CHECK(lp->fine_cluster_count == 1);
    CHECK(lp->clusters.size() == 3);
    // fine cluster sorted first
    CHECK(lp->flat_refs.size() == 1);              // only cutover>0 ref kept
    CHECK(lp->inline_cutover == 0.575f);
    // legacy whole-part view built from coarse clusters only:
    // its level-0 threshold matches the coarse segment's, not the fine one.
    // (Fixture writes distinct thresholds: fine L0 thr 0.7445, coarse L0 thr 0.3722.)
    // child got recursively loaded:
    CHECK(ps.find(child_hash) != nullptr);

    auto table = ps.part_lod_table();
    const auto& pl = table.at(parent_hash);
    CHECK(pl.inline_cutover == 0.575f);
    CHECK(pl.refs.size() == 1);
    CHECK(pl.refs[0].child_hash == child_hash);
    CHECK(std::fabs(pl.refs[0].child_scale - 1.0f) < 1e-6f);
}
```

And an unsegmented guard: existing flats load with `fine_cluster_count == clusters.size()` and empty `flat_refs` (extend `test_partstore_cluster_loading` with those two asserts).

- [ ] **Step 2: Run test to verify it fails**

Run: `make -C MatterEngine3/tests run-viewer-logic`
Expected: compile FAILURE — no `fine_cluster_count` / `flat_refs` members.

- [ ] **Step 3: Implement**

`lod_select.h`:

```cpp
struct PartLodRef {
    uint64_t child_hash;
    float rel_transform[16];
    float child_scale;
};
// inside PartLod:
float inline_cutover = 0.0f;
std::vector<PartLodRef> refs;
```

`part_store.h` — `LoadedPart` additions as in Interfaces.

`part_store.cpp` load_flat v-current path (:89-217):
- Use the 6-arg `load_flat_v3` (:94) getting clusters + refs.
- `bool segmented = std::any_of(clusters_in.begin(), clusters_in.end(), [](const auto& c){ return c.segment == 1; });`
- `std::stable_partition(clusters_in.begin(), clusters_in.end(), [](const auto& c){ return c.segment == 0; });` before the registration loops (:116).
- Legacy whole-part view loop (:128-157): `if (segmented && cl.segment != 1) continue;` (and compute max_lods over coarse-only when segmented).
- Per-cluster push loop (:160-211): count `fine_pushed` = clusters pushed with `cl_in.segment == 0` (respecting the empty-skip at :206). After the loop: `lp.fine_cluster_count = segmented ? fine_pushed : (uint32_t)lp.clusters.size();`
- Refs: `for (const auto& ref : refs_in) if (ref.inline_cutover > 0.0f) { lp.flat_refs.push_back(ref); lp.inline_cutover = std::max(lp.inline_cutover, ref.inline_cutover); }`
- `bound_radius` stays computed over ALL clusters.

v2-legacy path (:290) and compositional path (:399): set `lp.fine_cluster_count = (uint32_t)lp.clusters.size();`.

`get_or_load` flat branch (:308-321), after the expansion assignment:

```cpp
for (const auto& ref : loaded_[part_hash].flat_refs)
    get_or_load(ref.child_resolved_hash);
```

`part_lod_table` (:413-418):

```cpp
pl.inline_cutover = lp.inline_cutover;
for (const auto& ref : lp.flat_refs) {
    lod_select::PartLodRef r;
    r.child_hash = ref.child_resolved_hash;
    std::memcpy(r.rel_transform, ref.transform, sizeof r.rel_transform);
    r.child_scale = part_flatten::transform_uniform_scale(ref.transform);
    pl.refs.push_back(r);
}
```

(add `#include "part_flatten.h"`).

- [ ] **Step 4: Run test to verify it passes**

Run: `make -C MatterEngine3/tests run-viewer-logic`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/render/part_store.h MatterEngine3/src/render/part_store.cpp MatterEngine3/src/lod_select.h MatterEngine3/src/lod_select.cpp MatterEngine3/tests/viewer_logic_tests.cpp
git commit -m "feat(loader): v6 segments (fine-first partition, coarse legacy view), flat refs + cutover into PartLod table"
```

---

### Task 8: LOD select `_ex` + resolver child expansion

**Files:**
- Modify: `MatterEngine3/src/lod_select.h` / `MatterEngine3/src/lod_select.cpp` (select_sector_lods :33-67)
- Modify: `MatterEngine3/src/provider/sector_resolver.h` (ResolvedInstance :14-18)
- Modify: `MatterEngine3/src/provider/resolvers.cpp` (SectorLodResolver::resolve :50, instance loop :66-76)
- Test: `MatterEngine3/tests/viewer_logic_tests.cpp`

**Interfaces:**
- Consumes: Task 7's `PartLod.inline_cutover/.refs`; `viewer::mul16` (resolvers.cpp needs `#include "raster_cull.h"`).
- Produces:
  - `struct LodChoice { int level; float projected_size; };`
  - `std::map<SectorCoord, std::map<uint64_t, LodChoice>> select_sector_lods_ex(...)` — same body as today's select_sector_lods (:33-67) but also storing the projected size; `select_sector_lods` becomes a thin wrapper extracting `.level`.
  - `ResolvedInstance` gains `int segment = 1;`
  - Resolver rule: `ps >= pl->inline_cutover` (and cutover > 0) → emit trunk instance (`segment = 0`) + per ref one child `ResolvedInstance` (`segment = 1`, `transform = mul16(inst.world, ref.rel_transform)`, `child_ps = ps * child_radius * ref.child_scale / parent_radius`, level via `select_level(child_ps, child.thresholds)`); else single merged instance (`segment = 1`, today's behavior).

- [ ] **Step 1: Write the failing test**

Synthetic-PartLodTable test in `viewer_logic_tests.cpp` (no PartStore needed — build the `lod_select::PartLodTable` by hand):

```cpp
static void test_resolver_cutover_expansion() {
    lod_select::PartLodTable lods;
    auto& parent = lods[0xAAAAull];
    parent.bound_radius = 10.0f;
    parent.thresholds = {0.74453f, 0.37227f, 0.0f};
    parent.inline_cutover = 1.0f;
    lod_select::PartLodRef ref;
    ref.child_hash = 0xBBBBull;
    float rel[16] = {1,0,0,5, 0,1,0,0, 0,0,1,0, 0,0,0,1};  // translate +5x
    std::memcpy(ref.rel_transform, rel, sizeof rel);
    ref.child_scale = 1.0f;
    parent.refs.push_back(ref);
    auto& child = lods[0xBBBBull];
    child.bound_radius = 1.0f;
    child.thresholds = {0.74453f, 0.37227f, 0.0f};

    // Camera at distance 5 from a radius-10 parent: ps = 10/5 * pb*... — use
    // the same projected-size formula select_sector_lods_ex uses; assert via output.
    // NEAR: parent ps ~2 >= cutover 1 -> trunk + 1 child instance.
    auto near_out = resolve_with_cam(/*cam dist*/ 5.0f, lods);   // helper wrapping SectorLodResolver
    CHECK(near_out.size() == 2);
    CHECK(near_out[0].part_hash == 0xAAAAull);
    CHECK(near_out[0].segment == 0);
    CHECK(near_out[1].part_hash == 0xBBBBull);
    CHECK(near_out[1].segment == 1);
    // child transform = parent world * rel (translate +5 in parent frame)
    // child_ps = ps * (1.0 * 1.0) / 10.0 = ps/10 ~ 0.2 -> child level 1
    CHECK(near_out[1].lod_level == 1);

    // FAR: cam dist 20 -> parent ps ~0.5 < cutover -> single merged instance, segment 1.
    auto far_out = resolve_with_cam(20.0f, lods);
    CHECK(far_out.size() == 1);
    CHECK(far_out[0].segment == 1);
}
```

(The `resolve_with_cam` helper constructs the resolver input the same way existing resolver tests in this file do — one instance of hash 0xAAAA at the origin, camera on +Z at the given distance.)

- [ ] **Step 2: Run test to verify it fails**

Run: `make -C MatterEngine3/tests run-viewer-logic`
Expected: compile FAILURE (`segment`, `select_sector_lods_ex` missing).

- [ ] **Step 3: Implement**

`lod_select.h/.cpp`:

```cpp
struct LodChoice { int level; float projected_size; };
std::map<SectorCoord, std::map<uint64_t, LodChoice>>
select_sector_lods_ex(/* same params as select_sector_lods */);
```

Body = today's :33-67 verbatim, except it stores `{level, size}`; re-implement `select_sector_lods` as a wrapper that calls `_ex` and copies `.level`.

`sector_resolver.h` `ResolvedInstance`: add `int segment = 1;`

`resolvers.cpp` — `#include "raster_cull.h"`; `SectorLodResolver::resolve` (:50) calls `_ex`; per-instance loop (:66-76) becomes:

```cpp
int lod = 0; float ps = 0.0f;
auto it = lod_for_part.find(inst.resolved_hash);
if (it != lod_for_part.end()) { lod = it->second.level; ps = it->second.projected_size; }
if (lod < 0) continue;
auto pit = lods.find(inst.resolved_hash);
const lod_select::PartLod* pl = (pit != lods.end()) ? &pit->second : nullptr;
if (pl && pl->inline_cutover > 0.0f && ps >= pl->inline_cutover) {
    ResolvedInstance r;
    r.part_hash = inst.resolved_hash; r.lod_level = lod; r.segment = 0;
    std::memcpy(r.transform, inst.world.cell, sizeof(r.transform));
    out.push_back(r);
    for (const auto& ref : pl->refs) {
        ResolvedInstance cr;
        cr.part_hash = ref.child_hash; cr.segment = 1;
        viewer::mul16(inst.world.cell, ref.rel_transform, cr.transform);
        auto cit = lods.find(ref.child_hash);
        if (cit != lods.end() && pl->bound_radius > 0.0f) {
            float child_ps = ps * cit->second.bound_radius * ref.child_scale
                             / pl->bound_radius;
            cr.lod_level = lod_select::select_level(child_ps, cit->second.thresholds);
        } else cr.lod_level = 0;
        out.push_back(cr);
    }
    continue;
}
// ... existing single-instance emit unchanged (segment stays default 1) ...
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make -C MatterEngine3/tests run-viewer-logic`
Expected: PASS (new test + all existing resolver tests, which exercise the unchanged single-instance path).

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/lod_select.h MatterEngine3/src/lod_select.cpp MatterEngine3/src/provider/sector_resolver.h MatterEngine3/src/provider/resolvers.cpp MatterEngine3/tests/viewer_logic_tests.cpp
git commit -m "feat(resolve): projected-size LOD choices + cutover-gated trunk/child instance expansion"
```

---

### Task 9: GPU culler segment gating

**Files:**
- Modify: `MatterEngine3/src/render/gpu_culler.h` (PartGpu :118-126, ExpandedInst :215-219)
- Modify: `MatterEngine3/src/render/gpu_culler.cpp` (ensure_part :421-432, fingerprint :508-511, expansion :551-554/:566-568, rec build :621-622)
- Test: `MatterEngine3/tests/gpu_cull_tests.cpp` (fixture pattern :209-304, `inject_for_test`, `make_cam`, `test_readback_stats`)

**Interfaces:**
- Consumes: Task 7's `LoadedPart.fine_cluster_count`; Task 8's `ResolvedInstance.segment`. **No shader changes.**
- Produces:
  - `PartGpu` gains `uint32_t fine_cluster_count = 0;`
  - `ExpandedInst` gains `int segment;`
  - Segmented iff `fine_cluster_count < cluster_count`. segment 0 → clusters `[cluster_start, cluster_start + fine_n)`; segment 1 → `[cluster_start + fine_n, cluster_start + count)`; unsegmented → full range.
  - Fingerprint folds `ri.segment` (segment flip with identical transform must recull).
  - 0-count records are still pushed (the fast-path `n_records` recovery at :652-660 counts `expanded_`, skipping would desync).

- [ ] **Step 1: Write the failing test**

In `gpu_cull_tests.cpp` (follow the `inject_for_test` fixture pattern at :209-304): inject a part with 2 clusters where `fine_cluster_count = 1`, both clusters trivially visible to `make_cam`. Cull with one instance `segment = 0`, read stats: emitted count == cluster-0 tri count only. Cull again with `segment = 1` (same transform): emitted == cluster-1 tri count only — this also proves the fingerprint folds segment (same transform, different output). Third: a part injected with `fine_cluster_count == cluster_count` (unsegmented) emits the full range regardless of segment.

```cpp
static void test_cull_segment_gating() {
    // inject: part with clusters {A: 10 tris, B: 30 tris}, fine_cluster_count = 1
    // instance segment 0 -> stats.emitted_tris == 10
    // same instance, segment 1 -> stats.emitted_tris == 30   (fingerprint must invalidate)
    // unsegmented part (fine == count) -> emitted == 40 either way
}
```

(Fill in with the suite's actual inject/readback helper calls.)

- [ ] **Step 2: Run test to verify it fails**

Run: `make -C MatterEngine3/tests gpu-tests && cd MatterEngine3/tests && GALLIUM_DRIVER=d3d12 ./gpu_tests`
Expected: compile FAILURE (no `segment`/`fine_cluster_count` members).

- [ ] **Step 3: Implement**

`gpu_culler.h`: add `uint32_t fine_cluster_count = 0;` to `PartGpu`; `int segment;` to `ExpandedInst`.

`gpu_culler.cpp`:

- `ensure_part`, after `pg.cluster_count` is set (:427 clusters / :431 whole-part):
  ```cpp
  // Compositional parts have lp->clusters empty but get 1 synthetic whole-part
  // cluster; copying lp->fine_cluster_count (0) would falsely mark them segmented.
  pg.fine_cluster_count = lp->clusters.empty() ? pg.cluster_count
                                               : lp->fine_cluster_count;
  ```
- Fingerprint (:508-511): add `fold(&ri.segment, sizeof ri.segment);`
- Both expansion branches (:551-554 and :566-568): `ei.segment = ri.segment;`
- Rec build (:621-622):
  ```cpp
  const uint32_t fine_n = pg.fine_cluster_count;
  if (fine_n < pg.cluster_count) {
      if (ei.segment == 0) { rec.cluster_start = pg.cluster_start; rec.cluster_count = fine_n; }
      else { rec.cluster_start = pg.cluster_start + fine_n; rec.cluster_count = pg.cluster_count - fine_n; }
  } else {
      rec.cluster_start = pg.cluster_start; rec.cluster_count = pg.cluster_count;
  }
  ```
  Do **not** skip pushing 0-count recs.

- [ ] **Step 4: Run test to verify it passes**

Run: `make -C MatterEngine3/tests gpu-tests && cd MatterEngine3/tests && GALLIUM_DRIVER=d3d12 ./gpu_tests`
Expected: PASS (new gating test + all existing GPU cull tests).

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/render/gpu_culler.h MatterEngine3/src/render/gpu_culler.cpp MatterEngine3/tests/gpu_cull_tests.cpp
git commit -m "feat(cull): CPU-side segment gating via cluster subranges; fingerprint folds segment"
```

---

### Task 10: Provider skips LOD-managed refs

**Files:**
- Modify: `MatterEngine3/src/provider/local_provider.cpp` (append_instance_refs ref loop :447-453)
- Test: `MatterEngine3/tests/viewer_logic_tests.cpp` (mirror test_local_provider_cache :147)

**Interfaces:**
- Consumes: Task 4's `FlatInstanceRef.inline_cutover`.
- Produces: `append_instance_refs` skips refs with `inline_cutover > 0` — those are resolver-expanded at runtime (Task 8), so the provider must not also emit them permanently (double-draw). Budget-BOUNDARY refs (cutover 0) keep today's behavior.

- [ ] **Step 1: Write the failing test**

Mirror `test_local_provider_cache` (:147): build a LocalProvider over a fixture cache containing a parent flat with two refs — one cutover 0 (budget boundary), one cutover 0.575 (hinted). Assert the provider's sector manifest/instances contain the budget-boundary child but **not** the hinted child:

```cpp
static void test_provider_skips_cutover_refs() {
    // ... fixture: parent flat v6 with refs {cutover 0 -> child X, cutover 0.575 -> child Y} ...
    // resolve/collect provider instances for the parent's sector
    CHECK(contains_hash(instances, child_x_hash));
    CHECK(!contains_hash(instances, child_y_hash));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make -C MatterEngine3/tests run-viewer-logic`
Expected: FAIL — both children present.

- [ ] **Step 3: Implement**

In `append_instance_refs` ref loop (:447-453), before appending:

```cpp
if (r.inline_cutover > 0.0f) continue;  // LOD-managed: resolver expands at runtime
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make -C MatterEngine3/tests run-viewer-logic`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/provider/local_provider.cpp MatterEngine3/tests/viewer_logic_tests.cpp
git commit -m "fix(provider): skip cutover>0 flat refs — resolver expands them at runtime"
```

---

### Task 11: Task #40 timeboxed root-cause (>1M instanced-tri black frame)

**Files:**
- Investigate: `MatterEngine3/src/render/gpu_culler.cpp`, `MatterEngine3/src/render/renderer.cpp`, xforms-SSBO sizing paths
- Repro: `MatterEngine3/examples/world_demo/schemas/Tree.js` at 120 branches (known: 894k tris OK, ~1.7M black incl. ImGui)

**Interfaces:**
- Consumes: nothing from earlier tasks (independent); must complete **before** Task 12 live validation.
- Produces: either a fix commit, or a written trigger-threshold note in the SDD ledger so Task 12 keeps validation scenes under it.

- [ ] **Step 1: Timebox — half a day maximum.** Set Tree.js branch count to 120 temporarily, `make windows` (clean objs), launch viewer via a self-terminating FIFO script (`GALLIUM_DRIVER=d3d12`), confirm the black frame reproduces.
- [ ] **Step 2: Root-cause hunt** (in likely order): 32-bit index/offset overflow in cull dispatch or draw-args SSBO (~1M × small factor crossing 2^31/2^32 bytes), xforms/output SSBO capacity vs geometric growth, GL draw-count limits, ImGui interaction (black *including* ImGui suggests full-framebuffer failure → check GL errors / debug output with `glEnable(GL_DEBUG_OUTPUT)` in a debug run).
- [ ] **Step 3a (if root cause found quickly): fix + test.** Add a regression assertion or guard, verify 120-branch repro is clean, revert Tree.js to 60 branches, commit:
  ```bash
  git commit -m "fix(render): <root cause> — unblocks >1M instanced tris (Task #40)"
  ```
- [ ] **Step 3b (if timebox expires): document.** Bisect the tri count to a rough trigger threshold (e.g. binary-search branch count), write it into the SDD ledger for this project, revert Tree.js, and constrain Task 12 scenes below the threshold. Commit the note.

---

### Task 12: Tree opt-in + Windows rebuild + live validation

**Files:**
- Modify: `MatterEngine3/examples/world_demo/schemas/Tree.js` (:335)
- Modify: `docs/superpowers/specs/2026-07-10-lod-aware-instanced-children-design.md` (schema-API example → 3-arg form)
- Validation scripts: `tools/` FIFO pattern (viewer_shots.sh)

**Interfaces:**
- Consumes: everything (Tasks 1–10 merged behavior); Task 11's threshold knowledge.
- Produces: world_demo Tree bakes with hinted TreeBranch children; before/after numbers for the success criteria.

- [ ] **Step 1: Opt in.** Tree.js :335:
  ```js
  this.placeChild('TreeBranch', null, { instanced: true });
  ```
- [ ] **Step 2: Annotate the spec.** Update the spec's Schema API example to the real 3-arg form (`placeChild('TreeBranch', null, { instanced: true, inlineBelowPx: 64 })`) with a one-line note that options are the third argument because the second is params. Also amend the artifact-format section: implemented as v5→v6 with **no back-compat loader** — the existing peek/auto-regen path re-flattens stale artifacts on first use (supersedes "loader continues to read v3 artifacts").
- [ ] **Step 3: Rebuild Windows binary with clean objects** (struct/header changes in this feature):
  ```bash
  # clear stale objs first (no header dep tracking), then
  make -C MatterViewer windows
  ```
- [ ] **Step 4: Capture BEFORE numbers** (on a commit prior to Tree.js opt-in, or by temporarily reverting Step 1): tree flatten wall-time (flatten log), BLAS/VRAM bytes for a forest sector (renderer stats), frame time near a forest. Use self-terminating FIFO scripts with `GALLIUM_DRIVER=d3d12`.
- [ ] **Step 5: Capture AFTER numbers** with the opt-in active (flats auto-regen to v6 on first run). Same script, same camera shots: near / mid / far positions bracketing the 64px cutover — inspect shots for a visible pop.
- [ ] **Step 6: Check success criteria** (from the spec):
  - Tree flatten input ~900k → low tens of thousands (`FlattenResult.fine_tris` / log line).
  - Forest near-LOD geometry memory down roughly ×(tree variants).
  - No visible cutover pop at default 64px (tune `inlineBelowPx` in Tree.js if there is; re-bake).
  - Watch Task #40 threshold if unfixed.
- [ ] **Step 7: Commit**
  ```bash
  git add MatterEngine3/examples/world_demo/schemas/Tree.js docs/superpowers/specs/2026-07-10-lod-aware-instanced-children-design.md
  git commit -m "feat(tree): opt TreeBranch into LOD-aware instancing; record before/after validation numbers"
  ```
  (Include the measured numbers in the commit body.)

---

### Task 13: Full-sweep final gate

**Files:** none (verification only)

- [ ] **Step 1: Full headless sweep**
  ```bash
  ./build-all.sh test
  ```
  Expected: green except the **pre-existing** failures (`run-example`, `run-graph-integration`, `run-asyncbake`+autoremesher segfault). Any *new* failure is a regression — fix before proceeding.
- [ ] **Step 2: Full GPU suite**
  ```bash
  make -C MatterEngine3/tests gpu-tests && cd MatterEngine3/tests && GALLIUM_DRIVER=d3d12 ./gpu_tests
  ```
  Expected: PASS.
- [ ] **Step 3: Confirm Windows binary is current** (`make -C MatterViewer windows` is a no-op or rebuilds cleanly).
- [ ] **Step 4: Commit any final fixes** with targeted messages; do not batch unrelated changes.
