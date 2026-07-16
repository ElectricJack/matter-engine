# Vulkan Bake Fixes + Baked Part AO Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the post-Vulkan-port bake-time regressions (full-file cache probes, double cluster streaming, dead probe bake) and bake part-local ambient occlusion into `.part` artifacts with a schema-defined quality knob.

**Architecture:** Four workstreams on `feature/rt-lighting-phase2`: (1) header-only cache probe replacing full-file hashing (loaders already validate content hashes); (2) wholesale deletion of the SH-L1 probe lighting system; (3) retain-budget single-streaming in `flatten_part_impl`; (4) a new pure CPU AO baker (`part_ao_bake`) raycasting the part's own BVH, wired into `bake_source` before `save_v2`, with a `resolve_hash` salt to force one cold rebake.

**Tech Stack:** C++17, MSL BVH (`bvh.h` `BVH`/`BVHRay`), QuickJS-ng schema statics, existing headless test harness (`MatterEngine3/tests`).

**Spec:** `docs/superpowers/specs/2026-07-15-vulkan-bake-fixes-and-part-ao-design.md`

## Global Constraints

- Work directly on branch `feature/rt-lighting-phase2` in this tree (shared D: tree needed for Windows builds).
- NEVER run test suites in parallel — each takes 12-15 GB; parallel runs OOM-crash WSL2. Chain sequentially.
- Per-task tests only; the full `./build-all.sh test` sweep is the final gate only.
- MatterSurfaceLib is read-only (genuine bug fixes only, surfaced as a scope decision). All new code goes in MatterEngine3.
- No Vulkan shader (`shaders_vk`, `shaders_gpu`) edits — no glslc on Linux. GL `viewer/shaders/*.fs` are runtime-compiled GLSL and safe to edit.
- Byte-determinism: `.part` serialization must stay deterministic (same inputs → identical bytes).
- Discover test targets with: `grep '^run-' MatterEngine3/tests/Makefile` — do this once in Task 1 and reuse.
- Keep-list for "probe" grep hits: `transform_probe.comp` + `run_transform_probe` (Vulkan matrix-contract probe), `tileset_bake_ao` (ground tileset AO), `CacheArtifactProbeStats` naming, `field_probe` comments in `iso_primitive_tests.cpp`.

---

### Task 1: Header-only cache probe

**Files:**
- Modify: `MatterEngine3/src/part_asset_v2.h` (~line 170-185: replace `is_cache_artifact_compatible` declaration)
- Modify: `MatterEngine3/src/part_asset_v2.cpp` (~line 316-399: replace implementation)
- Modify: `MatterEngine3/src/part_graph.cpp:567`
- Modify: `MatterEngine3/src/provider/local_provider.cpp:502,523,896`
- Test: `MatterEngine3/tests/part_asset_v2_tests.cpp` (~450-599), `MatterEngine3/tests/transient_tests.cpp` (~200-240)

**Interfaces:**
- Produces: `bool part_asset::is_cache_artifact_header_compatible(const std::string& path, uint64_t expected_resolved_hash, uint32_t expected_format_version, CacheArtifactProbeStats* stats = nullptr)` — identical call shape to the old probe, so call sites are a name swap.
- Deletes: `part_asset::is_cache_artifact_compatible` (all callers migrated in this task).

Background for the implementer: `load_v2` (part_asset_v2.cpp ~line 572) and `load_flat_v3` (~line 657) already read the whole file into a buffer and reject on `fnv1a64(body) != content_hash`. Fail-closed corruption detection therefore lives in the loaders already; the probe's full-file hash is pure duplication and is the dominant bake-time regression (it runs per part per cache check on a 9p filesystem).

- [ ] **Step 1: Write the failing tests.** In `part_asset_v2_tests.cpp`, add (next to the existing probe tests):

```cpp
// Header probe: accepts a valid artifact while reading only header + material
// prefix (bounded read), rejects wrong hash/version/schema, and does NOT
// detect body corruption (that is the loaders' job).
static void test_header_probe() {
    // Reuse the fixture creation used by the existing probe tests at ~line 450
    // (save_v2 to a temp path with the registered material table).
    part_asset::CacheArtifactProbeStats stats{};
    CHECK(part_asset::is_cache_artifact_header_compatible(
              v2_path, v2_hash, part_asset::kFormatVersionV2, &stats),
          "header probe accepts valid artifact");
    const size_t material_prefix =
        2 * sizeof(uint32_t) + MaterialRegistryCount() * sizeof(MaterialDef);
    CHECK(stats.body_bytes <= material_prefix,
          "header probe reads at most the material prefix past the header");
    CHECK(!part_asset::is_cache_artifact_header_compatible(
              v2_path, v2_hash + 1, part_asset::kFormatVersionV2),
          "header probe rejects wrong resolved hash");
    CHECK(!part_asset::is_cache_artifact_header_compatible(
              v2_path, v2_hash, part_asset::kFormatVersionV2 + 1),
          "header probe rejects wrong format version");
    // Corrupt one byte in the BODY PAST the material prefix: header probe must
    // still accept (it does not hash the body)...
    corrupt_byte_at(v2_path, 40 + material_prefix + 8);
    CHECK(part_asset::is_cache_artifact_header_compatible(
              v2_path, v2_hash, part_asset::kFormatVersionV2),
          "header probe ignores deep body corruption");
    // ...but the loader must reject it (existing fail-closed path).
    BLASManager blas; TLASManager tlas(4);
    std::vector<part_asset::ChildInstance> kids; part_asset::LodLevels lods;
    CHECK(!part_asset::load_v2(v2_path, v2_hash, blas, tlas, kids, lods),
          "loader rejects corrupt body");
}
```

Adapt fixture/helper names to what the surrounding tests at lines 450-599 actually use (`corrupt_byte_at` may need writing: fopen r+b, fseek, flip one byte, fclose). Also migrate the existing `is_cache_artifact_compatible` tests: identity/schema/staleness checks (lines ~457-464, 480-489, 496-507, 524-539, 558-570, 583-591) switch to `is_cache_artifact_header_compatible`; the corrupt-body expectations flip to "probe accepts, loader rejects" (material-table tampering within the prefix must still be REJECTED by the probe). In `transient_tests.cpp` (~202-240) swap the name — semantics there (stale detection, scratch-vs-cache selection) are all header-level.

- [ ] **Step 2: Run tests to verify they fail.** Discover targets: `grep '^run-' MatterEngine3/tests/Makefile`. Then `make -C MatterEngine3/tests <partasset-target-binary> && ...` — expected: compile FAILURE (`is_cache_artifact_header_compatible` not declared).

- [ ] **Step 3: Implement the header probe** in `part_asset_v2.cpp`, replacing `is_cache_artifact_compatible` (line ~316):

```cpp
bool is_cache_artifact_header_compatible(
    const std::string& path, uint64_t expected_resolved_hash,
    uint32_t expected_format_version, CacheArtifactProbeStats* stats) {
    if (stats) *stats = CacheArtifactProbeStats{};
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t header[40];
    if (std::fread(header, 1, sizeof(header), f) != sizeof(header)) {
        std::fclose(f);
        return false;
    }
    Reader reader{header, header + sizeof(header)};
    uint64_t content_hash_ignored = 0;
    if (!read_and_validate_header(reader, expected_resolved_hash,
                                  expected_format_version,
                                  content_hash_ignored)) {
        std::fclose(f);
        return false;
    }
    const size_t material_prefix_size =
        2u * sizeof(uint32_t) +
        static_cast<size_t>(MaterialRegistryCount()) * sizeof(MaterialDef);
    std::vector<uint8_t> prefix(material_prefix_size);
    const size_t got = std::fread(prefix.data(), 1, prefix.size(), f);
    std::fclose(f);
    if (stats) {
        stats->max_read_chunk = std::max(sizeof(header), got);
        stats->body_bytes = got;
        stats->retained_material_bytes = got;
    }
    if (got != material_prefix_size) return false;
    Reader material_reader{prefix.data(), prefix.data() + prefix.size()};
    const uint32_t material_schema = material_reader.get<uint32_t>();
    const uint32_t material_count = material_reader.get<uint32_t>();
    if (!material_reader.ok ||
        material_schema != MaterialRegistrySchemaVersion() ||
        material_count != static_cast<uint32_t>(MaterialRegistryCount()))
        return false;
    for (uint32_t i = 0; i < material_count; ++i) {
        const uint8_t* serialized = material_reader.take(sizeof(MaterialDef));
        if (!material_reader.ok ||
            std::memcmp(serialized, MaterialRegistryGet(static_cast<int>(i)),
                        sizeof(MaterialDef)) != 0)
            return false;
    }
    return true;
}
```

Delete `is_cache_artifact_compatible` entirely (impl + declaration). Update the header comment on `CacheArtifactProbeStats` to describe the header probe. Swap the four call sites (`part_graph.cpp:567`, `local_provider.cpp:502`, `:523`, `:896`) to the new name — arguments unchanged.

- [ ] **Step 4: Build + run the part-asset and transient suites sequentially.** Expected: PASS.
- [ ] **Step 5: Commit.** `git add -u MatterEngine3 && git commit -m "perf(bake): header-only cache probe; loaders keep fail-closed body hash"`

---

### Task 2: Delete the SH-L1 probe lighting system

**Files:**
- Delete: `MatterEngine3/src/probe_bake.{h,cpp}`, `MatterEngine3/src/probe_volume.{h,cpp}`, `MatterEngine3/src/probe_bricks.{h,cpp}`, `MatterEngine3/src/render/probe_texture.{h,cpp}`, `MatterEngine3/include/matter/probe_volume.h`, `MatterEngine3/tests/lighting_tests.cpp`, `MatterEngine3/tests/probe_brick_tests.cpp`
- KEEP `MatterEngine3/src/world_tracer.{h,cpp}` — verification found non-probe consumers: `WorldSession::raycast()`, `instance_count()`, `instance_info()` use the `tracer` member (matter_engine.cpp :294-297, :2806-2844, :3855-3904). Delete only the probe-thread's `probe_tracer` usage; the `tracer` member and `ensure_tracer()` stay.
- Modify: `MatterEngine3/src/matter_engine.cpp` (includes :19,:51-53; `probe_tex` :227-229; `update_probe_dims` :345-350; probe thread block :493-515 and definitions :2432-2600s; publish `probes` upload :1029-1120; stats :1523-1526; teardown :2972-2990; tick consume :3755-3765)
- Modify: `MatterEngine3/src/provider/local_provider.cpp` (probe bake + `.probes` cache :628-722; `try_load_cached_probes` :1146-1198; includes :8-9)
- Modify: `MatterEngine3/src/provider/world_source.h:29` (`WorldManifest::probes`)
- Modify: `MatterEngine3/src/render/raster_composer.{h,cpp}` (`set_probes`, probe texture bindings)
- Modify: `MatterEngine3/viewer/shaders/raster.fs` (uniforms :16-21, `useProbes==1` branch :69-83 — keep the flat fallback at :84 as the unconditional path)
- Modify: `MatterEngine3/src/resolve_cache.h` (probe references), `MatterEngine3/tests/viewer_logic_tests.cpp` (:618-725 probe manifest assertions), `MatterEngine3/Makefile`, `MatterEngine3/tests/Makefile` (LIGHTING_SOURCES targets, entire PROBEBRICK section)

**Interfaces:**
- Consumes: nothing from other tasks. Independent of Task 1.
- Produces: `WorldManifest` without `probes`; `raster_composer` without `set_probes`. Later tasks must not reference probes.
- Keeps: `WorldManifest::lights` / `world_lights.cpp` (Vulkan RT consumes via `renderer.set_lights`, matter_engine.cpp:1053,1092), plus everything on the Global Constraints keep-list.

- [ ] **Step 1: Delete the files listed above** (`git rm`).
- [ ] **Step 2: Strip references.** Work compiler-error-driven: `make -C MatterEngine3 2>&1 | head -40`, remove each dangling reference per the Files list. Rules: delete probe state/calls outright, do not stub them; where a conditional consumed `manifest.probes` (e.g. upload at :1063, tick at :3755) delete the whole branch; in `raster.fs` delete the uniforms and the `useProbes == 1` branch so the existing `lit = ambientColor * ao + sunColor * ndl;` fallback becomes unconditional; drop `stats.probe_dims`/`update_probe_dims` and any viewer HUD display of it (grep `probe` in `MatterViewer/` and clean, honoring the keep-list).
- [ ] **Step 3: Verify no stragglers.** `grep -rin "probe" MatterEngine3/src MatterEngine3/viewer MatterEngine3/include MatterViewer --include=*.{h,hpp,cpp,fs,vs,glsl} | grep -iv "transform_probe\|CacheArtifactProbe\|tileset"` — expected: no hits (keep-list only).
- [ ] **Step 4: Build engine + tests Makefiles; run the viewer-logic suite** (covers manifest handling). Expected: clean build, PASS.
- [ ] **Step 5: Commit.** `git add -A MatterEngine3 MatterViewer && git commit -m "refactor(render): delete SH-L1 probe lighting system (Vulkan permanent)"`

---

### Task 3: Flatten retain-budget (remove double streaming)

**Files:**
- Modify: `MatterEngine3/src/part_flatten.cpp` (`flatten_part_impl` source pass :1361-1381 and ladder materialize :1410-1416)
- Test: `MatterEngine3/tests/part_flatten_tests.cpp`

**Interfaces:**
- Consumes: `Gatherer::materialize_range(order, lo, hi, ctris, ctriex, cl_min, cl_max)` (part_flatten.cpp:447), `Cluster{first_tri, tri_count}` (part_cluster.h:10), sizeof(Tri)=64, sizeof(TriEx)=96.
- Produces: no API change; env override `MATTER_FLATTEN_RETAIN_MB` (default 512, `0` disables retention).

- [ ] **Step 1: Write the failing test** — determinism across budgets:

```cpp
// Retained-vs-streamed flatten must produce byte-identical artifacts.
static void test_flatten_retain_budget_identical() {
    // Fixture: same 200x100 grid sheet + split_clusters(16000) setup as
    // test_flatten_watertight_invariant (line ~1064).
    setenv("MATTER_FLATTEN_RETAIN_MB", "512", 1);
    // flatten to cache_root_a ... (mirror the watertight test's flatten call)
    setenv("MATTER_FLATTEN_RETAIN_MB", "0", 1);   // force re-materialization
    // flatten the SAME source to cache_root_b ...
    unsetenv("MATTER_FLATTEN_RETAIN_MB");
    CHECK(files_byte_equal(flat_path_a, flat_path_b),
          "retained and streamed flatten artifacts are byte-identical");
}
```

(`files_byte_equal`: read both files, compare vectors. Mirror the watertight test's fixture code exactly — including the resolved-hash bookkeeping — but write to two distinct temp cache roots. Read the env var fresh per `flatten_part_impl` call, NOT cached in a static, or the second flatten won't see the change.)

- [ ] **Step 2: Run to verify it fails** (env var not implemented yet → both runs retained... it will PASS trivially until Step 3 changes behavior, so first make it fail by asserting the env var exists: skip this — instead verify the test compiles and passes AFTER Step 3, and verify `MATTER_FLATTEN_RETAIN_MB=0` actually exercises the fallback by temporarily printing which path each cluster took). The real regression guard is byte-identity plus the existing `test_flatten_watertight_invariant`.

- [ ] **Step 3: Implement retention.** In `flatten_part_impl`:

```cpp
struct RetainedCluster {
    std::vector<Tri> tris;
    std::vector<TriEx> triex;
    float mn[3], mx[3];
    bool valid = false;
};
const char* retain_env = std::getenv("MATTER_FLATTEN_RETAIN_MB");
const size_t retain_budget =
    (retain_env ? static_cast<size_t>(std::atoll(retain_env))
                : size_t{512}) * 1024u * 1024u;
std::vector<RetainedCluster> retained(clusters.size());
size_t retained_bytes = 0;
```

Source pass: after `materialize_range` + `collect_source_boundary_normals`, compute `bytes = tris.size() * (sizeof(Tri) + sizeof(TriEx))`; if `retained_bytes + bytes <= retain_budget`, move the vectors + AABB into `retained[cluster_index]` (`valid = true`, `retained_bytes += bytes`). Ladder pass: if `retained[ci].valid`, move the data back out into `ctris/ctriex/cl_min/cl_max` and clear the slot; else call `materialize_range` as today. Everything downstream (`apply_canonical_boundary_normals`, the QEM ladder) is untouched — the retained copy is bit-identical to a fresh materialization because both passes materialize the identical `order` range (verified: same iteration order both passes).

- [ ] **Step 4: Run the part_flatten suite** (includes the new test + `test_flatten_watertight_invariant`). Expected: PASS.
- [ ] **Step 5: Commit.** `git commit -am "perf(flatten): retain materialized clusters across passes (budgeted)"`

---

### Task 4: AO bake core (`part_ao_bake`)

**Files:**
- Create: `MatterEngine3/src/part_ao_bake.h`, `MatterEngine3/src/part_ao_bake.cpp`
- Test: Create `MatterEngine3/tests/part_ao_tests.cpp`; add a suite target to `MatterEngine3/tests/Makefile` (copy an existing small headless suite's block, e.g. the part_flatten one); add `src/part_ao_bake.cpp` to `MatterEngine3/Makefile` sources/objs.

**Interfaces:**
- Produces (Task 5 consumes exactly this):

```cpp
// part_ao_bake.h
#pragma once
#include <cstdint>
#include <vector>
#include "precomp.h"   // Tri, TriEx, float3 (match includes used by lod_bake.h)

namespace part_ao {

struct AoBakeParams {
    float quality = 1.0f;          // 0 disables; rays/vertex = clamp(quality*32, 4, 128)
    float radius = 2.0f;           // max occlusion reach (world units)
    uint64_t max_total_rays = 8000000;  // adaptive cap: scales rays/vertex down
};

struct AoBakeStats {
    uint32_t rays_per_vertex = 0;  // after adaptive scaling
    uint64_t unique_positions = 0;
};

// Bakes ao0/ao1/ao2 in place across all groups of one part. group_tris and
// group_triex are parallel; each triex[i] is parallel to tris[i]. Geometry is
// combined into one BVH; occlusion is part-local. Deterministic: identical
// inputs yield identical ao bytes. quality <= 0 leaves triex untouched.
void bake_part_ao(const std::vector<const std::vector<Tri>*>& group_tris,
                  const std::vector<std::vector<TriEx>*>& group_triex,
                  const AoBakeParams& params, AoBakeStats* stats = nullptr);

}  // namespace part_ao
```

- Consumes: MSL `BvhMesh` (`MatterSurfaceLib/include/bvh.h:115-126` — plain struct: `tri`, `triCount`) + `BVH` (`bvh.h:83-112`). Verified facts (do not re-derive):
  - `BVH(BvhMesh*)` (bvh.cpp:80-86) allocates `bvhNode` (MALLOC64) + `triIdx` (new[]) and calls `Build()` itself — no separate `Build()` call needed. `Build()` computes triangle centroids internally (bvh.cpp:150), so the soup only needs vertex0/1/2 filled.
  - `BVHRay` (bvh.h:60-67): set `O`, `D`, **`rD` (componentwise 1/D)**, and **`ray.hit.t = 1e30f`** — the constructor does NOT initialize `hit`, and `Intersect` only writes `hit` when it finds something closer.
  - Hit distance is `ray.hit.t` (`Intersection` at bvh.h:52-57). No hit ⇒ stays 1e30f.
  - **Neither `BVH` nor `BvhMesh` has a destructor.** The baker must free manually: `FREE64(bvh.bvhNode); delete[] bvh.triIdx; FREE64(mesh.tri);` (MALLOC64/FREE64 from MSL `precomp.h`).
  - Reference for the construction pattern (persistent variant): `MatterSurfaceLib/src/blas_manager.cpp:156-183`.

- [ ] **Step 1: Write the failing tests** (`tests/part_ao_tests.cpp`, headless, no GL):

```cpp
// Helpers: quad(cx,cz,y,half) -> two Tris forming a horizontal square; TriEx
// with N=(0,1,0), ao=1. (Write these locally; keep fixtures tiny.)

static void test_open_plate_unoccluded() {
    auto [tris, triex] = quad(0, 0, /*y=*/0, /*half=*/1.0f);
    part_ao::bake_part_ao({&tris}, {&triex}, {});
    for (const TriEx& e : triex) {
        CHECK(e.ao0 > 0.95f && e.ao1 > 0.95f && e.ao2 > 0.95f,
              "open plate stays ~unoccluded");
    }
}

static void test_overhang_darkens() {
    auto [floor_t, floor_e] = quad(0, 0, 0, 1.0f);
    auto [lid_t, lid_e]     = quad(0, 0, 0.2f, 1.0f);   // lid 0.2 above floor
    part_ao::bake_part_ao({&floor_t, &lid_t}, {&floor_e, &lid_e}, {});
    CHECK(floor_e[0].ao0 < 0.5f, "floor under a close lid darkens");
}

static void test_determinism() {
    auto [t1, e1] = quad(0, 0, 0, 1.0f); auto [l1, le1] = quad(0, 0, 0.2f, 1.0f);
    auto [t2, e2] = quad(0, 0, 0, 1.0f); auto [l2, le2] = quad(0, 0, 0.2f, 1.0f);
    part_ao::bake_part_ao({&t1, &l1}, {&e1, &le1}, {});
    part_ao::bake_part_ao({&t2, &l2}, {&e2, &le2}, {});
    CHECK(std::memcmp(e1.data(), e2.data(), e1.size() * sizeof(TriEx)) == 0 &&
          std::memcmp(le1.data(), le2.data(), le1.size() * sizeof(TriEx)) == 0,
          "AO bake is byte-deterministic");
}

static void test_quality_zero_disables() {
    auto [t, e] = quad(0, 0, 0, 1.0f); auto [l, le] = quad(0, 0, 0.2f, 1.0f);
    part_ao::AoBakeParams p; p.quality = 0.0f;
    part_ao::bake_part_ao({&t, &l}, {&e, &le}, p);
    CHECK(e[0].ao0 == 1.0f && le[0].ao0 == 1.0f, "quality 0 leaves ao untouched");
}

static void test_adaptive_budget_scales_down() {
    auto [t, e] = quad(0, 0, 0, 1.0f);
    part_ao::AoBakeParams p; p.max_total_rays = 8;   // 6 verts -> ~1 ray each
    part_ao::AoBakeStats s{};
    part_ao::bake_part_ao({&t}, {&e}, p, &s);
    CHECK(s.rays_per_vertex >= 4, "rays/vertex never below floor of 4");
    part_ao::AoBakeParams q; q.quality = 4.0f;        // clamps at 128
    part_ao::bake_part_ao({&t}, {&e}, q, &s);
    CHECK(s.rays_per_vertex == 128, "quality clamps at 128 rays/vertex");
}
```

- [ ] **Step 2: Add the suite to the Makefiles, build, verify it fails to link** (`bake_part_ao` undefined).
- [ ] **Step 3: Implement** `part_ao_bake.cpp`:

```cpp
#include "part_ao_bake.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace part_ao {
namespace {

struct VertKey {                       // position+normal bits: hard edges keep
    uint32_t px, py, pz, nx, ny, nz;   // separate AO from coincident positions
    bool operator==(const VertKey& o) const {
        return px==o.px && py==o.py && pz==o.pz && nx==o.nx && ny==o.ny && nz==o.nz;
    }
};
struct VertKeyHash {
    size_t operator()(const VertKey& k) const {
        uint64_t h = 1469598103934665603ull;
        for (uint32_t w : {k.px, k.py, k.pz, k.nx, k.ny, k.nz}) {
            h ^= w; h *= 1099511628211ull;
        }
        return static_cast<size_t>(h);
    }
};
VertKey key_of(const float3& p, const float3& n) {
    VertKey k;
    std::memcpy(&k.px, &p.x, 4); std::memcpy(&k.py, &p.y, 4);
    std::memcpy(&k.pz, &p.z, 4);
    std::memcpy(&k.nx, &n.x, 4); std::memcpy(&k.ny, &n.y, 4);
    std::memcpy(&k.nz, &n.z, 4);
    return k;
}

// Deterministic ONB (Duff et al. branchless).
void onb(const float3& n, float3& t, float3& b) {
    const float sign = n.z >= 0.0f ? 1.0f : -1.0f;
    const float a = -1.0f / (sign + n.z);
    t = make_float3(1.0f + sign * n.x * n.x * a, sign * n.x * n.y * a, -sign * n.x);
    b = make_float3(n.x * n.y * a, sign + n.y * n.y * a, -n.y);
}

float ao_at(BVH& bvh, const float3& p, const float3& n,
            uint32_t rays, float radius, uint32_t seed) {
    constexpr float kGolden = 2.39996323f;            // golden-angle spiral
    const float azimuth0 = (seed & 0xFFFFu) * (6.2831853f / 65536.0f);
    float3 t, b; onb(n, t, b);
    const float eps = 1e-3f * radius;
    float occlusion = 0.0f;
    for (uint32_t i = 0; i < rays; ++i) {
        // Spherical-Fibonacci cosine-weighted hemisphere sample.
        const float u = (i + 0.5f) / rays;            // stratified in [0,1)
        const float cos_theta = std::sqrt(1.0f - u);
        const float sin_theta = std::sqrt(u);
        const float phi = azimuth0 + i * kGolden;
        const float3 d_local = make_float3(std::cos(phi) * sin_theta,
                                           std::sin(phi) * sin_theta, cos_theta);
        float3 d = t * d_local.x + b * d_local.y + n * d_local.z;
        BVHRay ray;
        ray.O = p + n * eps;
        ray.D = d;
        ray.rD = make_float3(1.0f / d.x, 1.0f / d.y, 1.0f / d.z);
        ray.hit.t = 1e30f;   // ctor leaves hit uninitialized; Intersect only lowers it
        bvh.Intersect(ray, 0);
        if (ray.hit.t < radius)
            occlusion += 1.0f - ray.hit.t / radius;    // distance-attenuated
    }
    return std::max(0.0f, 1.0f - occlusion / rays);
}

}  // namespace

void bake_part_ao(const std::vector<const std::vector<Tri>*>& group_tris,
                  const std::vector<std::vector<TriEx>*>& group_triex,
                  const AoBakeParams& params, AoBakeStats* stats) {
    if (stats) *stats = AoBakeStats{};
    if (params.quality <= 0.0f) return;

    size_t total = 0;
    for (const auto* g : group_tris) total += g->size();
    if (total == 0) return;

    // Combined part-local soup, 64-aligned for the SIMD Tri type (movaps).
    BvhMesh mesh;
    mesh.triCount = static_cast<int>(total);
    mesh.tri = static_cast<Tri*>(MALLOC64(total * sizeof(Tri)));
    {
        size_t off = 0;
        for (const auto* g : group_tris) {
            if (g->empty()) continue;
            std::memcpy(mesh.tri + off, g->data(), g->size() * sizeof(Tri));
            off += g->size();
        }
    }
    BVH bvh(&mesh);   // ctor allocates + Build()s; centroids computed inside

    const uint32_t vert_count = static_cast<uint32_t>(total * 3);
    uint32_t rays = static_cast<uint32_t>(
        std::clamp(std::lround(params.quality * 32.0f), 4l, 128l));
    while (static_cast<uint64_t>(rays) * vert_count > params.max_total_rays &&
           rays > 4)
        rays /= 2;                                     // adaptive: halve to budget
    if (stats) stats->rays_per_vertex = rays;

    std::unordered_map<VertKey, float, VertKeyHash> cache;
    cache.reserve(vert_count);
    for (size_t gi = 0; gi < group_tris.size(); ++gi) {
        const std::vector<Tri>& tris = *group_tris[gi];
        std::vector<TriEx>& triex = *group_triex[gi];
        if (triex.size() != tris.size()) continue;
        for (size_t ti = 0; ti < tris.size(); ++ti) {
            const float3 ps[3] = {tris[ti].vertex0, tris[ti].vertex1,
                                  tris[ti].vertex2};
            const float3 ns[3] = {triex[ti].N0, triex[ti].N1, triex[ti].N2};
            float* aos[3] = {&triex[ti].ao0, &triex[ti].ao1, &triex[ti].ao2};
            for (int c = 0; c < 3; ++c) {
                const VertKey k = key_of(ps[c], ns[c]);
                auto found = cache.find(k);
                if (found == cache.end()) {
                    const uint32_t seed =
                        static_cast<uint32_t>(VertKeyHash{}(k));
                    const float ao = ao_at(bvh, ps[c], ns[c], rays,
                                           params.radius, seed);
                    found = cache.emplace(k, ao).first;
                }
                *aos[c] = found->second;
            }
        }
    }
    if (stats) stats->unique_positions = cache.size();

    // BVH and BvhMesh have no destructors (bvh.h) — free what the ctor and we
    // allocated, or every part bake leaks its whole soup + node pool.
    FREE64(bvh.bvhNode);
    delete[] bvh.triIdx;
    FREE64(mesh.tri);
}

}  // namespace part_ao
```

(`MALLOC64`/`FREE64` come from MSL `precomp.h`, already included via `precomp.h` in the header.) Self-hit note: rays start at `p + n*eps`; the origin's own coplanar triangles are parallel to every hemisphere direction, so no self-hit handling beyond eps is needed.

- [ ] **Step 4: Build + run the new suite.** Expected: all 5 tests PASS.
- [ ] **Step 5: Commit.** `git add -A MatterEngine3 && git commit -m "feat(bake): deterministic part-local AO baker (part_ao_bake)"`

---

### Task 5: Wire AO into bake_source + schema knob + hash salt

**Files:**
- Modify: `MatterEngine3/src/script_host.h` / `MatterEngine3/src/script_host.cpp` (AO config eval + bake hook before `save_v2` ~line 1360; `resolve_hash` :596-623)
- Modify: `MatterEngine3/src/part_asset_v2.h` (~line 28, salt constant)
- Test: `MatterEngine3/tests/part_ao_tests.cpp` (integration cases) or the suite covering `bake_source` (whichever the target discovery in Task 1 showed builds script_host — likely the demand-bake/composition suites)

**Interfaces:**
- Consumes: `part_ao::bake_part_ao(group_tris, group_triex, params, stats)` from Task 4 (exact signature above).
- Produces: schema knob `static ao = { quality: <float> };` (absent → 1.0); `constexpr uint64_t kEngineBakeVersion = 1;` in part_asset_v2.h.

- [ ] **Step 1: Write the failing test** — bake a schema through `ScriptHost::bake_source` (mirror an existing bake_source test fixture from the demand-bake/composition tests for host setup):

```cpp
// Schema: a floor voxel slab with a lid slab close above it -> interior faces
// must darken. Same geometry with `static ao = { quality: 0 };` -> all ao == 1.
// Verbs verified against live_edit_prod_tests.cpp:79 and
// examples/world_demo/schemas/LightingGarden.js:40 (box([center],[half])).
static const char* kAoSchema = R"JS(
class AoPlates extends Part {
  build(p) {
    this.fill(1);
    this.beginVoxels(0.05);
    this.box([0, 0,    0], [1.0, 0.05, 1.0]);   // floor slab
    this.box([0, 0.3,  0], [1.0, 0.05, 1.0]);   // lid slab ~0.2 above
    this.endVoxels();
  }
}
)JS";
// quality:0 variant: identical, plus `static ao = { quality: 0 };` above build().
// bake, then load_v2 the artifact and scan all BLAS entries' tri_extra:
//   CHECK(min_ao < 0.9f, "baked part contains occluded vertices");
// bake the quality:0 variant:
//   CHECK(min_ao == 1.0f, "ao quality 0 disables the bake");
// and: resolved hashes of the two variants differ (source differs), plus
// re-baking the SAME source twice produces byte-identical .part files.
```

- [ ] **Step 2: Run to verify it fails** (min_ao stays 1.0 — no bake wired).
- [ ] **Step 3: Implement.**
  1. **Salt** (`part_asset_v2.h` next to `kFormatVersionFlat`): `constexpr uint64_t kEngineBakeVersion = 1u;  // salts resolve_hash; bump on bake-semantics changes (v1: part AO)`. In `ScriptHost::resolve_hash` (script_host.cpp:596-623): `return part_asset::compute_resolved_hash(...) ^ (part_asset::kEngineBakeVersion * 0x9E3779B97F4A7C15ull);`
  2. **Schema knob:** following the `eval_requires`/`eval_lod_budgets` pattern (script_host.cpp:126, :382-520), read the authored class's `ao` static: if it is an object with a numeric `quality` property, capture `(float)quality`, else default `1.0f`. Do this inside `bake_source` where the authored class is already evaluated (same JS context), storing `float ao_quality`.
  3. **Bake hook:** in `bake_source`, immediately before `save_v2` (~line 1360), after ALL registration paths (voxel per-cell :875, stacked :921, triangle-session :1294/:1336) have completed:

```cpp
// Part-local AO: mutate TriEx in place across all registered BLAS entries.
// get_entries() returns const unique_ptrs; the pointees are mutable.
{
    std::vector<const std::vector<Tri>*> ao_tris;
    std::vector<std::vector<TriEx>*> ao_triex;
    for (const auto& entry : blas.get_entries()) {
        if (!entry || entry->triangles.empty()) continue;
        if (entry->tri_extra.size() != entry->triangles.size()) continue;
        ao_tris.push_back(&entry->triangles);
        ao_triex.push_back(&entry->tri_extra);
    }
    part_ao::AoBakeParams ao_params;
    ao_params.quality = ao_quality;
    part_ao::bake_part_ao(ao_tris, ao_triex, ao_params);
}
```

  **Verify first** (one-time check, before relying on entry geometry being part-local): every `tlas.draw(...)` in `bake_source` paths is preceded by `tlas.load_identity()` — confirmed for :877/:923; check the :1294/:1336 triangle-session sites. If any draw uses a non-identity transform, the soup for that entry must be transformed by it before tracing (add the transform loop then; do NOT add it speculatively).
- [ ] **Step 4: Run the affected bake suite + the part-asset determinism test** sequentially. Expected: PASS (determinism test re-bakes under the new salt and must still produce byte-identical repeat bakes).
- [ ] **Step 5: Commit.** `git commit -am "feat(bake): bake part-local AO into .part; schema ao.quality knob; bake-version hash salt"`

---

### Task 6: AO through the LOD ladder + final gates

**Files:**
- Test: `MatterEngine3/tests/part_flatten_tests.cpp` (new case)
- Possibly modify: nothing expected — `reproject_triex` (MatterSurfaceLib/src/mesh_transform.cpp:162) copies the full source `TriEx` (incl. ao0/1/2) onto decimated triangles; this task PROVES it end-to-end.

**Interfaces:**
- Consumes: flatten pipeline + `load_flat_v3` from earlier tasks; no new API.

- [ ] **Step 1: Write the test.** Extend a part_flatten fixture (grid-sheet builder at part_flatten_tests.cpp:611): set a non-uniform AO pattern on the source `TriEx` (e.g. `ao0 = 0.25f` on tris in one half of the sheet, 1.0 elsewhere), flatten with enough tris to force ≥2 LOD levels, `load_flat_v3`, then for the coarsest level of each cluster assert `min(ao) < 0.5f` — i.e. decimation did not reset AO to 1.0.
- [ ] **Step 2: Run it.** Expected: PASS (reproject carries TriEx). If it FAILS, the fix belongs in `part_flatten.cpp`'s ladder around the `reproject_triex` call at :1141 (ensure the source mesh passed in still carries the ao-laden triex) — not in MSL.
- [ ] **Step 3: Full gate, sequentially:** `./build-all.sh test` (headless suites; known pre-existing failure allowed: run-asyncbake+autoremesher segfault per project memory — anything else new is a stop-the-line failure).
- [ ] **Step 4: Windows rebuild, CLEAN** (headers changed): delete all Windows object files/build dirs for engine+viewer, then `make windows` (per the always-rebuild + clean-rebuild policies). Expected: `viewer.exe` links.
- [ ] **Step 5: Commit + hand off for visual check.** `git commit -am "test(flatten): AO survives the LOD ladder"`. Then ask Jack to run the viewer (or drive the FIFO shots harness) and confirm crevice/cavity darkening on parts under Vulkan; note the salt forces a full cold rebake on first world load.
