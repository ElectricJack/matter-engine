# Froxel Volumetric Lighting & Smoke — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add froxel-based volumetric lighting — world height fog, RT-shadowed god rays, and DSL-authored smoke/mist emitters (`emitVolume`) — to the Vulkan RT renderer. Entirely off in the GL raster path; disabled by default even on Vulkan until the perf gate is validated.

**Architecture:** Three compute passes (`vol_density` → `vol_scatter` → `vol_integrate`) fill a camera-aligned 160×90×128 RGBA16F froxel grid each frame; `composite.frag` applies the result in one trilinear sample per pixel before tone-map. `VkVolumetrics` owns the 3D images and pipelines, instantiated by `VkSceneRenderer` and recorded between the GI à-trous pass and the composite pass. Emitters are authored via a new `emitVolume` DSL verb, serialized as a metadata block in `.part` files, gathered CPU-side each frame into an SSBO capped at 256, and consumed by the density compute shader.

**Tech Stack:** C++17, Vulkan 1.3 dynamic rendering, GLSL 4.60 / SPIR-V, `VK_KHR_ray_query` (shadow rays), QuickJS-ng DSL verb, ImGui, existing `VkImageResource` / `VkBufferResource` / `matter::find_spirv` / `record_image_transition` infrastructure.

---

## Global Constraints

- Vulkan RT path only; GL raster path (`RenderPath::GpuDriven`) is unaffected.
- Froxel grid dimensions are compile-time constants: **160 × 90 × 128** (W×H×D); change only if perf gate fails.
- Far-clip range: **300 m**; Z slices exponentially distributed.
- Volume images use **RGBA16F** (`VK_FORMAT_R16G16B16A16_SFLOAT`); total ~59 MB across the three persistent images.
- Emitter SSBO cap: **256**; nearest-to-camera wins on overflow; overflow logged once per overflow event.
- No `VK_KHR_ray_query` → volumetrics disabled at init, one-line log, mirrors the existing RT-unavailable fallback.
- Budget: ≤ **1.5 ms** total for all volumetric passes on Demo world (RTX 4090, DLSS render resolution).
- `VulkanVolumetricsSettings::enabled` defaults to **false**; feature is off until Task 12 validates it.
- SPIR-V embeds (`shaders_gen/embedded_spirv.h`) regenerate **only** via Jack's MSYS2 Windows build — Linux cannot compile shaders. Every task that adds or modifies shaders ends with a **"handoff checkpoint: request Windows rebuild"** step; GPU validation steps follow the rebuild.
- `make windows` required after any engine change; clean-rebuild Windows object files after any struct or header change (no header dependency tracking in the Windows build).
- Test suites run **sequentially**, never in parallel (WSL2 OOM; each suite uses 12–15 GB).
- Headless tests: `make -C MatterEngine3/tests run-<suite>`; GPU suites need `GALLIUM_DRIVER=d3d12`.
- Validation world: `MATTER_WORLD=demo` (`MatterEngine3/examples/world_demo/WorldData/Demo/`); scripted viewer runs must self-terminate via `MatterEngine3/tools/viewer_shots.sh` with a FIFO `quit` command.

---

## Task 1: DSL verb `emitVolume`

**Files:**
- Modify: `MatterEngine3/src/dsl_state.h` — add `VolumeEmitter` struct and `emitters_` field to `DslState`
- Modify: `MatterEngine3/src/dsl_state.cpp` — implement `emit_volume()`
- Modify: `MatterEngine3/src/dsl_bindings.cpp` — add `j_emitVolume` and register `__dsl_emitVolume` in `install_bindings`
- Modify: `MatterEngine3/src/script_host.cpp` — salt bake hash with serialized emitter records; read `DslState::emitters()` after `build()` and store them in the `.part` (alongside geometry)
- Modify: `MatterEngine3/src/part_asset_v2.h` — add `VolumeEmitter` struct and extend `save_v2`/`load_v2` with an emitter-block trailer
- Modify: `MatterEngine3/src/part_asset_v2.cpp` — implement emitter-block serialize/deserialize
- Test: `MatterEngine3/tests/script_tests.cpp` (headless, `run-script`)

**Interfaces:**
- Consumes: `dsl::DslState` opaque pointer from `JS_GetContextOpaque(ctx)`; `install_bindings` registration pattern from `dsl_bindings.cpp:937`
- Produces:
  - `dsl::VolumeEmitter` struct (pos[3], dir[3], radius, spread, length, density, color[3], rise, turbulence) used by Tasks 2 and 3
  - `DslState::emitters()` → `const std::vector<VolumeEmitter>&`
  - `part_asset::VolumeEmitter` mirror (identical layout, in `part_asset_v2.h`) used by Task 2
  - Extended `save_v2` / `load_v2` that round-trip emitter records via a tagged trailer block

- [ ] **Step 1: Write failing `emitVolume` validation and hash-salt tests**

  In `MatterEngine3/tests/script_tests.cpp`, add a new test group (follow the CHECK macro and counter pattern already present):

  ```cpp
  // --- emitVolume verb tests ---
  static int test_emit_volume() {
      int failures = 0;

      // 1. Valid emitVolume call records one emitter.
      {
          script_host::ScriptHost host;
          const std::string source = R"(
  import { Part } from 'part_base';
  export default class Chimney extends Part {
    build() {
      emitVolume({ pos:[0,5,0], dir:[0,1,0], radius:0.4, spread:0.15,
                   length:12, density:0.8, color:[0.85,0.85,0.9],
                   rise:1.5, turbulence:0.6 });
    }
  })";
          auto result = host.bake_source(source, "{}", {});
          CHECK(result.error.ok, "emitVolume: valid call bakes without error");
          // Re-load the .part and verify emitter count via load_v2.
          part_asset::BLASManager blas; part_asset::TLASManager tlas;
          std::vector<part_asset::ChildInstance> children;
          part_asset::LodLevels lods;
          std::vector<part_asset::VolumeEmitter> emitters;
          bool ok = part_asset::load_v2(result.written_path,
                                        result.resolved_hash,
                                        blas, tlas, children, lods, emitters);
          CHECK(ok, "emitVolume: load_v2 succeeds after bake");
          CHECK(emitters.size() == 1u, "emitVolume: one emitter round-trips");
          CHECK(std::abs(emitters[0].radius - 0.4f) < 1e-5f,
                "emitVolume: radius round-trips correctly");
      }

      // 2. Missing required field (radius) → bake error.
      {
          script_host::ScriptHost host;
          const std::string source = R"(
  import { Part } from 'part_base';
  export default class Bad extends Part {
    build() { emitVolume({ pos:[0,0,0], dir:[0,1,0] }); }
  })";
          auto result = host.bake_source(source, "{}", {});
          CHECK(!result.error.ok, "emitVolume: missing radius fails bake");
      }

      // 3. Bake hash changes when emitter params change.
      {
          script_host::ScriptHost host;
          const std::string src_a = R"(
  import { Part } from 'part_base';
  export default class E extends Part {
    build() { emitVolume({pos:[0,0,0],dir:[0,1,0],radius:0.4,spread:0.1,
                          length:10,density:0.5,color:[1,1,1],rise:1,turbulence:0.3}); }
  })";
          const std::string src_b = R"(
  import { Part } from 'part_base';
  export default class E extends Part {
    build() { emitVolume({pos:[0,0,0],dir:[0,1,0],radius:0.8,spread:0.1,
                          length:10,density:0.5,color:[1,1,1],rise:1,turbulence:0.3}); }
  })";
          auto ra = host.bake_source(src_a, "{}", {});
          auto rb = host.bake_source(src_b, "{}", {});
          CHECK(ra.error.ok && rb.error.ok, "emitVolume: both bakes succeed");
          CHECK(ra.resolved_hash != rb.resolved_hash,
                "emitVolume: different radius → different hash");
      }

      return failures;
  }
  ```

  Call `test_emit_volume()` from the test's `main` and accumulate the failure count.

- [ ] **Step 2: Run the headless script suite and verify RED**

  ```bash
  make -C MatterEngine3/tests run-script 2>&1 | tail -20
  ```

  Expected: compile error — `emitVolume`, `VolumeEmitter`, `DslState::emitters()`, `load_v2` 5-arg overload do not exist.

- [ ] **Step 3: Add `VolumeEmitter` to `dsl_state.h` and `part_asset_v2.h`**

  In `MatterEngine3/src/dsl_state.h`, add after the `ModifierRegion` struct:

  ```cpp
  // Localized volume emitter authored with emitVolume(). Stored as metadata in
  // the .part file; untouched by the LOD flatten ladder; salts the bake hash.
  struct VolumeEmitter {
      float pos[3]   = {};          // part-local position
      float dir[3]   = {0,1,0};    // plume axis (normalized at bake)
      float radius   = 0.4f;       // base radius at pos (m)
      float spread   = 0.15f;      // radius growth per meter along axis
      float length   = 12.0f;      // axial fade-out distance (m)
      float density  = 0.8f;       // peak extinction density
      float color[3] = {1,1,1};    // scattering albedo RGB
      float rise     = 1.5f;       // buoyant drift along dir (m/s)
      float turbulence = 0.6f;     // curl-noise warp strength [0,1]
  };
  ```

  Add to `DslState` (private section, after `regions_`):
  ```cpp
  std::vector<VolumeEmitter> emitters_;
  ```

  Add public accessor and mutator:
  ```cpp
  const std::vector<VolumeEmitter>& emitters() const { return emitters_; }
  void emit_volume(const VolumeEmitter& e);
  ```

  In `MatterEngine3/src/part_asset_v2.h`, add after `FlatInstanceRef`:

  ```cpp
  // Volume emitter metadata block in the .part file. Layout mirrors dsl::VolumeEmitter.
  // Stored as a tagged trailer after the existing v2 body so load_v2 on an older
  // file (no trailer) returns an empty emitter vector (fail-open: no emitters).
  struct VolumeEmitter {
      float pos[3];
      float dir[3];
      float radius;
      float spread;
      float length;
      float density;
      float color[3];
      float rise;
      float turbulence;
  };
  static_assert(sizeof(VolumeEmitter) == 44);

  // save_v2 overload that also writes an emitter-block trailer (tag 0x454D4954 "EMIT",
  // uint32_t count, then count × VolumeEmitter). When emitters is empty the trailer
  // is omitted for backward compat with readers that stop at EOF after the lods block.
  bool save_v2(const std::string& path, const BLASManager& blas,
               const TLASManager& tlas,
               const ChildInstance* children, size_t child_count,
               const LodLevels& lods,
               uint64_t resolved_hash,
               const std::vector<VolumeEmitter>& emitters);

  // load_v2 overload that also reads the emitter-block trailer (if present).
  // Fills emitters_out; leaves it empty when no trailer exists (older .part files).
  bool load_v2(const std::string& path, uint64_t expected_resolved_hash,
               BLASManager& blas, TLASManager& tlas,
               std::vector<ChildInstance>& children_out,
               LodLevels& lods_out,
               std::vector<VolumeEmitter>& emitters_out,
               PartAssetLoadFailure* failure = nullptr,
               std::string* reason = nullptr);
  ```

- [ ] **Step 4: Implement `dsl::DslState::emit_volume` in `dsl_state.cpp`**

  ```cpp
  void DslState::emit_volume(const VolumeEmitter& e) {
      // Normalize dir in place; fail-closed on zero-length.
      VolumeEmitter copy = e;
      float dx = copy.dir[0], dy = copy.dir[1], dz = copy.dir[2];
      float len = std::sqrt(dx*dx + dy*dy + dz*dz);
      if (len < 1e-7f) { set_error("emitVolume: dir must be a non-zero vector"); return; }
      copy.dir[0] = dx/len; copy.dir[1] = dy/len; copy.dir[2] = dz/len;
      if (copy.radius <= 0.0f) { set_error("emitVolume: radius must be > 0"); return; }
      if (copy.length <= 0.0f) { set_error("emitVolume: length must be > 0"); return; }
      if (copy.density < 0.0f) { set_error("emitVolume: density must be >= 0"); return; }
      emitters_.push_back(copy);
  }
  ```

- [ ] **Step 5: Add `j_emitVolume` binding in `dsl_bindings.cpp` and register it**

  After the existing `j_endModifier` function, add:

  ```cpp
  static JSValue j_emitVolume(JSContext* c, JSValueConst, int n, JSValueConst* a) {
      DslState* st = state_of(c);
      if (n < 1 || !JS_IsObject(a[0])) {
          st->set_error("emitVolume: expected one object argument");
          return JS_UNDEFINED;
      }
      dsl::VolumeEmitter e{};
      // pos
      {
          JSValue v = JS_GetPropertyStr(c, a[0], "pos");
          if (!JS_IsUndefined(v)) {
              for (int i = 0; i < 3; ++i) {
                  JSValue el = JS_GetPropertyUint32(c, v, (uint32_t)i);
                  e.pos[i] = (float)argd(c, el); JS_FreeValue(c, el);
              }
          }
          JS_FreeValue(c, v);
      }
      // dir
      {
          JSValue v = JS_GetPropertyStr(c, a[0], "dir");
          if (!JS_IsUndefined(v)) {
              for (int i = 0; i < 3; ++i) {
                  JSValue el = JS_GetPropertyUint32(c, v, (uint32_t)i);
                  e.dir[i] = (float)argd(c, el); JS_FreeValue(c, el);
              }
          } else { e.dir[0]=0; e.dir[1]=1; e.dir[2]=0; }
          JS_FreeValue(c, v);
      }
      // color
      {
          JSValue v = JS_GetPropertyStr(c, a[0], "color");
          if (!JS_IsUndefined(v)) {
              for (int i = 0; i < 3; ++i) {
                  JSValue el = JS_GetPropertyUint32(c, v, (uint32_t)i);
                  e.color[i] = (float)argd(c, el); JS_FreeValue(c, el);
              }
          } else { e.color[0]=e.color[1]=e.color[2]=1.0f; }
          JS_FreeValue(c, v);
      }
      double tmp = 0.0;
      if (opt_num(c, a[0], "radius",     tmp)) e.radius     = (float)tmp;
      else { st->set_error("emitVolume: radius is required"); return JS_UNDEFINED; }
      if (opt_num(c, a[0], "spread",     tmp)) e.spread     = (float)tmp;
      if (opt_num(c, a[0], "length",     tmp)) e.length     = (float)tmp;
      else { st->set_error("emitVolume: length is required"); return JS_UNDEFINED; }
      if (opt_num(c, a[0], "density",    tmp)) e.density    = (float)tmp;
      if (opt_num(c, a[0], "rise",       tmp)) e.rise       = (float)tmp;
      if (opt_num(c, a[0], "turbulence", tmp)) e.turbulence = (float)tmp;
      st->emit_volume(e);
      return JS_UNDEFINED;
  }
  ```

  In `install_bindings`, after the `bind("__dsl_extrude"...)` line:
  ```cpp
  bind("__dsl_emitVolume", j_emitVolume, 1);
  ```

- [ ] **Step 6: Implement `part_asset_v2.cpp` emitter trailer serialization**

  In `part_asset_v2.cpp`, implement the emitter-overload of `save_v2`: after writing the existing lods block, if `!emitters.empty()`, write a 4-byte tag `0x454D4954u` ("EMIT"), a `uint32_t` count, then `count × sizeof(VolumeEmitter)` bytes.

  Implement the emitter-overload of `load_v2`: call the existing `load_v2` internally, then attempt to read the EMIT trailer from the file. If the file is at EOF after lods → empty vector and return true. If the EMIT tag is present but malformed → set failure, return false.

- [ ] **Step 7: Wire `DslState::emitters()` into `script_host.cpp` bake hash and `.part` save**

  In `ScriptHost::bake_source`, after the existing geometry-to-BLAS pipeline and before `save_v2`:
  1. Collect `state.emitters()` into a `std::vector<part_asset::VolumeEmitter>`.
  2. Fold `emitters.size()` and each emitter's raw bytes into the resolved hash input (append after child_hashes in `compute_resolved_hash` call — pass as additional bytes in the source folding, or xor-fold separately into the hash seed).
  3. Call the new `save_v2(..., emitters)` overload.

  In `ScriptHost::resolve_hash`, fold the same emitter bytes so hash and bake agree.

- [ ] **Step 8: Run headless script suite and verify GREEN**

  ```bash
  make -C MatterEngine3/tests run-script 2>&1 | tail -20
  ```

  Expected: `ALL PASS` with the three new `emitVolume` test groups included.

- [ ] **Step 9: Commit Task 1**

  ```bash
  git add MatterEngine3/src/dsl_state.h MatterEngine3/src/dsl_state.cpp \
           MatterEngine3/src/dsl_bindings.cpp MatterEngine3/src/script_host.cpp \
           MatterEngine3/src/part_asset_v2.h MatterEngine3/src/part_asset_v2.cpp \
           MatterEngine3/tests/script_tests.cpp
  git commit -m "feat(volumetrics): emitVolume DSL verb with bake hash salting and .part trailer"
  ```

---

## Task 2: Emitter metadata persistence through the flatten ladder

**Files:**
- Modify: `MatterEngine3/src/part_flatten.h` — pass emitters through `flatten_part`
- Modify: `MatterEngine3/src/part_flatten.cpp` — copy emitters from leaf `.part` to output `.flat.part` (emitters are metadata, not geometry — they ride along unchanged)
- Modify: `MatterEngine3/src/part_asset_v2.h` — add emitter accessor to `load_flat_v3`; extend `save_flat_v3` with emitter trailer (same EMIT block used in Task 1's `save_v2`)
- Modify: `MatterEngine3/src/part_asset_v2.cpp` — implement flat-emitter round-trip
- Modify: `MatterEngine3/tests/part_flatten_tests.cpp` — extend existing flatten test suite

**Interfaces:**
- Consumes: `part_asset::VolumeEmitter` and `load_v2` (5-arg) from Task 1
- Produces: `save_flat_v3(..., emitters)` and `load_flat_v3(..., emitters_out)` — emitters field passed through `flatten_part` untouched; later consumed by Task 3's instance-resolve gather

- [ ] **Step 1: Write failing flatten round-trip test**

  In `MatterEngine3/tests/part_flatten_tests.cpp`, add a test that:
  1. Creates a synthetic `.part` via `save_v2` with one `VolumeEmitter` (radius=1.5, density=0.7).
  2. Calls `flatten_part` on it.
  3. Loads the resulting `.flat.part` via `load_flat_v3(..., emitters_out)`.
  4. Checks `emitters_out.size() == 1` and `emitters_out[0].radius == 1.5f`.

  ```cpp
  static int test_emitter_survives_flatten() {
      int failures = 0;
      // Write a minimal .part with one emitter.
      const uint64_t hash = 0xEE11000011EE0000ull;
      const std::string path = std::string(kCacheRoot) + "/parts/";
      part_asset::BLASManager blas; part_asset::TLASManager tlas;
      part_asset::VolumeEmitter e{};
      e.pos[0]=0; e.pos[1]=5; e.pos[2]=0;
      e.dir[0]=0; e.dir[1]=1; e.dir[2]=0;
      e.radius=1.5f; e.spread=0.2f; e.length=10.0f;
      e.density=0.7f; e.color[0]=e.color[1]=e.color[2]=1.0f;
      e.rise=1.0f; e.turbulence=0.5f;
      bool saved = part_asset::save_v2(
          path + part_asset::cache_path_resolved(hash),
          blas, tlas, nullptr, 0, {}, hash, {e});
      CHECK(saved, "emitter_flatten: save_v2 with emitter succeeds");
      // Flatten it (no geometry = trivial flat).
      // ... (call flatten_part with the cache root, verify flat written)
      std::vector<part_asset::VolumeEmitter> out_emitters;
      // Load the flat artifact.
      part_asset::BLASManager blas2; part_asset::TLASManager tlas2;
      std::vector<part_asset::FlatCluster> clusters;
      std::vector<part_asset::FlatInstanceRef> refs;
      bool loaded = part_asset::load_flat_v3(
          path + part_asset::cache_path_flat(hash),
          hash, blas2, tlas2, clusters, refs, out_emitters);
      CHECK(loaded, "emitter_flatten: load_flat_v3 succeeds");
      CHECK(out_emitters.size() == 1u,
            "emitter_flatten: emitter survives flatten ladder");
      CHECK(std::abs(out_emitters[0].radius - 1.5f) < 1e-5f,
            "emitter_flatten: emitter radius round-trips through flat");
      return failures;
  }
  ```

- [ ] **Step 2: Run flatten suite and verify RED**

  ```bash
  make -C MatterEngine3/tests run-flatten 2>&1 | tail -10
  ```

  Expected: compile error (missing `load_flat_v3` 7-arg overload and emitter trailer in save_flat_v3).

- [ ] **Step 3: Extend `save_flat_v3` and `load_flat_v3` with emitter trailer**

  In `part_asset_v2.h`, add overloads:
  ```cpp
  bool save_flat_v3(const std::string& path, const BLASManager& blas,
                    const TLASManager& tlas,
                    const std::vector<FlatCluster>& clusters,
                    const std::vector<FlatInstanceRef>& instance_refs,
                    uint64_t resolved_hash,
                    const std::vector<VolumeEmitter>& emitters);

  bool load_flat_v3(const std::string& path, uint64_t expected_resolved_hash,
                    BLASManager& blas, TLASManager& tlas,
                    std::vector<FlatCluster>& clusters_out,
                    std::vector<FlatInstanceRef>& instance_refs_out,
                    std::vector<VolumeEmitter>& emitters_out);
  ```

  Implement in `part_asset_v2.cpp`: after the instance_refs trailer, append the EMIT block when non-empty (same 4-byte tag + uint32_t count + raw bytes format as Task 1). `load_flat_v3` reads the EMIT block after instance_refs using an EOF-tolerant probe (no EMIT block → empty vector → return true).

- [ ] **Step 4: Thread emitters through `flatten_part`**

  In `part_flatten.h` / `part_flatten.cpp`: `flatten_part` already loads the source `.part` with `load_v2`. Extend it to:
  1. Load emitters via `load_v2(..., emitters_out)`.
  2. Pass them unchanged to `save_flat_v3(..., emitters)`.

  Emitters are metadata only — no geometry merging required.

- [ ] **Step 5: Run flatten suite and verify GREEN**

  ```bash
  make -C MatterEngine3/tests run-flatten 2>&1 | tail -10
  ```

  Expected: `ALL PASS`.

- [ ] **Step 6: Commit Task 2**

  ```bash
  git add MatterEngine3/src/part_asset_v2.h MatterEngine3/src/part_asset_v2.cpp \
           MatterEngine3/src/part_flatten.h MatterEngine3/src/part_flatten.cpp \
           MatterEngine3/tests/part_flatten_tests.cpp
  git commit -m "feat(volumetrics): emitter metadata round-trips through LOD flatten ladder"
  ```

---

## Task 3: Runtime emitter gathering

**Files:**
- Create: `MatterEngine3/src/render/vk_emitter_gather.h` — `GpuVolumeEmitter` std430 struct, `VolumeEmitterGatherer` class
- Create: `MatterEngine3/src/render/vk_emitter_gather.cpp` — distance-filter, 256-cap nearest-wins gather, overflow log
- Modify: `MatterEngine3/src/matter_engine.cpp` — resolve emitters from loaded parts each frame; call gatherer; pass result into `VkVolumetrics::update_emitters` (defined in Task 5)
- Test: `MatterEngine3/tests/emitter_gather_tests.cpp` (headless, new Makefile target `run-emittergather`)

**Interfaces:**
- Consumes: `part_asset::VolumeEmitter` (Task 1), `LoadedPart` / `PartStore::find` (existing), instance transforms from `WorldComposer`
- Produces:
  - `GpuVolumeEmitter` struct (std430, 64 bytes with pad) consumed by Task 5's SSBO
  - `VolumeEmitterGatherer::gather(camera_pos, instances+transforms, part_store)` → `std::vector<GpuVolumeEmitter>` (≤ 256)

- [ ] **Step 1: Write failing CPU gather unit tests**

  Create `MatterEngine3/tests/emitter_gather_tests.cpp`:

  ```cpp
  // emitter_gather_tests.cpp — CPU-side distance filter + overflow policy tests.
  // Headless. Run: make -C MatterEngine3/tests run-emittergather
  #include "render/vk_emitter_gather.h"
  #include "check.h"
  #include <cstdio>
  #include <vector>

  int main() {
      int failures = 0;

      // 1. Emitter within 300 m of camera → included.
      {
          viewer::VolumeEmitterGatherer gatherer;
          part_asset::VolumeEmitter e{};
          e.pos[0]=0; e.pos[1]=0; e.pos[2]=0;
          e.dir[0]=0; e.dir[1]=1; e.dir[2]=0;
          e.radius=0.4f; e.spread=0.15f; e.length=12.0f;
          e.density=0.8f; e.color[0]=e.color[1]=e.color[2]=1.0f;
          e.rise=1.5f; e.turbulence=0.6f;
          float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
          float cam[3] = {0, 0, 250.0f};  // 250 m away → within 300 m
          auto result = gatherer.gather(cam, {{e, identity}});
          CHECK(result.size() == 1u, "gather: emitter within 300m included");
      }

      // 2. Emitter beyond 300 m → excluded.
      {
          viewer::VolumeEmitterGatherer gatherer;
          part_asset::VolumeEmitter e{};
          e.pos[0]=0; e.pos[1]=0; e.pos[2]=0;
          e.dir[0]=0; e.dir[1]=1; e.dir[2]=0;
          e.radius=0.4f; e.spread=0.15f; e.length=12.0f;
          e.density=0.5f; e.color[0]=e.color[1]=e.color[2]=1.0f;
          e.rise=1.0f; e.turbulence=0.3f;
          float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
          float cam[3] = {0, 0, 400.0f};  // 400 m → beyond 300 m
          auto result = gatherer.gather(cam, {{e, identity}});
          CHECK(result.empty(), "gather: emitter beyond 300m excluded");
      }

      // 3. 257 emitters at distance 10 m → exactly 256 in output (nearest wins).
      {
          viewer::VolumeEmitterGatherer gatherer;
          std::vector<std::pair<part_asset::VolumeEmitter, std::array<float,16>>> inputs;
          for (int i = 0; i < 257; ++i) {
              part_asset::VolumeEmitter e{};
              e.pos[0] = (float)i; e.pos[1]=0; e.pos[2]=0;
              e.dir[0]=0; e.dir[1]=1; e.dir[2]=0;
              e.radius=0.4f; e.spread=0.1f; e.length=8.0f;
              e.density=0.5f; e.color[0]=e.color[1]=e.color[2]=1.0f;
              e.rise=1.0f; e.turbulence=0.2f;
              std::array<float,16> id = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
              inputs.push_back({e, id});
          }
          float cam[3] = {0,0,0};
          auto result = gatherer.gather_flat(cam, inputs);
          CHECK(result.size() == 256u, "gather: overflow capped at 256");
      }

      printf("%s\n", failures == 0 ? "ALL PASS" : "FAILURES");
      return failures > 0 ? 1 : 0;
  }
  ```

  Add to `MatterEngine3/tests/Makefile`:
  ```makefile
  EMITTERGATHER_TARGET = emitter_gather_tests
  EMITTERGATHER_CPP = emitter_gather_tests.cpp ../src/render/vk_emitter_gather.cpp
  # ... build rule analogous to existing headless targets
  run-emittergather: $(EMITTERGATHER_TARGET)
  	./$(EMITTERGATHER_TARGET)
  ```
  Add `run-emittergather` to the `.PHONY` list.

- [ ] **Step 2: Run the suite and verify RED**

  ```bash
  make -C MatterEngine3/tests run-emittergather 2>&1 | tail -10
  ```

  Expected: compile error — `vk_emitter_gather.h` does not exist.

- [ ] **Step 3: Implement `vk_emitter_gather.h` and `vk_emitter_gather.cpp`**

  Create `MatterEngine3/src/render/vk_emitter_gather.h`:

  ```cpp
  #pragma once
  #include "part_asset_v2.h"
  #include <array>
  #include <cstdint>
  #include <string>
  #include <utility>
  #include <vector>

  namespace viewer {

  // GPU layout for one volume emitter — std430, 64 bytes (matches vol_common.glsl).
  // world_pos: emitter origin in world space (from instance transform × part-local pos).
  // world_dir: plume axis in world space (normalized).
  struct alignas(16) GpuVolumeEmitter {
      float world_pos[3];
      float radius;      // base cone radius (m)
      float world_dir[3];
      float spread;      // radius growth per metre
      float length;      // axial fade-out (m)
      float density;     // peak extinction
      float rise;        // buoyant scroll speed (m/s)
      float turbulence;  // curl-noise warp strength
      float color[3];    // scattering albedo RGB
      float pad;
  };
  static_assert(sizeof(GpuVolumeEmitter) == 64, "GpuVolumeEmitter must be 64 bytes");

  // Entry for the gather call: one emitter + the instance row-major transform
  // (engine convention, same as ChildPlacement::transform).
  struct EmitterInstance {
      part_asset::VolumeEmitter emitter;
      float transform[16];  // row-major object-to-world
  };

  class VolumeEmitterGatherer {
  public:
      static constexpr uint32_t kMaxEmitters = 256;
      static constexpr float    kMaxRange    = 300.0f;

      // Gather emitters within kMaxRange of camera_pos, cap at kMaxEmitters
      // (nearest-to-camera wins). Logs once (via fprintf(stderr,...)) when >256
      // are in range; subsequent overflows until the count drops below 256 are
      // silent. Transforms part-local pos/dir to world space.
      std::vector<GpuVolumeEmitter> gather(
          const float camera_pos[3],
          const std::vector<EmitterInstance>& instances);

      // Flat variant for tests: takes (emitter, identity-or-transform) pairs.
      std::vector<GpuVolumeEmitter> gather_flat(
          const float camera_pos[3],
          const std::vector<std::pair<part_asset::VolumeEmitter,
                                     std::array<float,16>>>& inputs);

  private:
      bool overflow_logged_ = false;
  };

  } // namespace viewer
  ```

  Create `MatterEngine3/src/render/vk_emitter_gather.cpp` implementing:
  - `gather`: for each `EmitterInstance`, transform `emitter.pos` and `emitter.dir` by `transform` (row-major 4×4 multiply), compute distance² to `camera_pos`, skip if > `kMaxRange²`. Collect all in-range, sort by distance² ascending, take first `kMaxEmitters`. If `in_range.size() > kMaxEmitters` and `!overflow_logged_`, print `"VkVolumetrics: %zu emitters in range, capping at 256 (nearest wins)\n"` to stderr, set `overflow_logged_ = true`. Convert to `GpuVolumeEmitter` (re-normalize dir after transform).
  - `gather_flat`: convert inputs to `EmitterInstance` with the provided array as `transform`, call `gather`.

- [ ] **Step 4: Wire into `matter_engine.cpp` render path**

  In `MatterEngine3/src/matter_engine.cpp`, in the Vulkan render path where `impl_->engine->record_cull_and_render(...)` is called:
  1. Build a `std::vector<viewer::EmitterInstance>` by iterating resolved instances, loading each part via `impl_->store->find(hash)`, reading `lp.emitters` (added to `LoadedPart` in this step — see below), and pairing each emitter with the instance transform.
  2. Call `impl_->emitter_gatherer.gather(camera_pos, emitter_instances)`.
  3. Call `impl_->vk_volumetrics->update_emitters(gathered)` (stub OK until Task 5).

  In `MatterEngine3/src/render/part_store.h`, add to `LoadedPart`:
  ```cpp
  std::vector<part_asset::VolumeEmitter> emitters;  // from .part or .flat.part emitter trailer
  ```

  In `PartStore::load_flat` (`part_store.cpp:113`) — the private helper that calls `load_flat_v3` — extend the `load_flat_v3` call to pass an `emitters_out` vector and store it into `lp.emitters` on success. Because `load_flat` is the sole caller of `load_flat_v3` within `PartStore`, this is the correct injection point for the flat path.

  In `PartStore::get_or_load`, in the compositional path that calls `load_v2` directly (`part_store.cpp:423`), pass an `emitters` out-vector to the new `load_v2` 8-arg overload and populate `lp.emitters` from it.

- [ ] **Step 5: Run suite and verify GREEN**

  ```bash
  make -C MatterEngine3/tests run-emittergather 2>&1 | tail -10
  ```

  Expected: `ALL PASS`.

- [ ] **Step 6: Commit Task 3**

  ```bash
  git add MatterEngine3/src/render/vk_emitter_gather.h \
           MatterEngine3/src/render/vk_emitter_gather.cpp \
           MatterEngine3/src/render/part_store.h \
           MatterEngine3/src/matter_engine.cpp \
           MatterEngine3/tests/emitter_gather_tests.cpp \
           MatterEngine3/tests/Makefile
  git commit -m "feat(volumetrics): per-frame CPU emitter gather (distance filter, 256 cap)"
  ```

---

## Task 4: Options plumbing

**Files:**
- Modify: `MatterEngine3/include/matter/world_session.h` — add `VulkanVolumetricsSettings` struct; add `vulkan_volumetrics` field to `RenderOptions`
- Modify: `MatterEngine3/src/world_lights.h` — add height-fog fields to `WorldLights`
- Modify: `MatterEngine3/src/world_lights.cpp` — parse new `fog` lines from `world.manifest` (new branch in `parse_lights`)
- Modify: `MatterEngine3/src/part_graph.cpp` — add `if (name == "fog") continue;` skip in `read_manifest` (line 487 area) so `fog` lines are not misread as schema names
- Modify: `MatterEngine3/src/matter_engine.cpp` — pass fog settings from manifest through to `VkVolumetrics`
- Test: `MatterEngine3/tests/lighting_garden_tests.cpp` (existing suite, `run-lighting-garden` target) for schema-parse headless check

**Interfaces:**
- Consumes: `matter::RenderOptions` (existing); `world_lights::WorldLights` (existing); `parse_lights` (existing)
- Produces:
  - `matter::VulkanVolumetricsSettings` — consumed by Tasks 5–9 and Task 10
  - Extended `WorldLights` with `fog_density`, `fog_floor`, `fog_falloff`, `fog_color[3]`, `fog_wind[3]` — consumed by Task 6's density shader via push constants

- [ ] **Step 1: Write failing options parse test**

  In `MatterEngine3/tests/lighting_garden_tests.cpp`, add:

  ```cpp
  static int test_volumetrics_settings_defaults() {
      int failures = 0;
      matter::VulkanVolumetricsSettings s{};
      CHECK(s.enabled == false,       "volumetrics: disabled by default");
      CHECK(s.temporal_blend == 0.85f,"volumetrics: temporal_blend default 0.85");
      CHECK(s.phase_g == 0.3f,        "volumetrics: phase_g default 0.3");
      return failures;
  }

  static int test_fog_fields_parse() {
      int failures = 0;
      // Write a temp manifest with fog lines.
      const std::string manifest = "/tmp/vol_test_world.manifest";
      {
          std::ofstream f(manifest);
          f << "fog density 0.02\n"
            << "fog floor -2.0\n"
            << "fog falloff 40.0\n"
            << "fog color 0.7 0.8 0.9\n"
            << "fog wind 0.5 0.0 0.2\n";
      }
      world_lights::WorldLights lights{};
      std::string err;
      bool ok = world_lights::parse_lights(manifest, lights, err);
      CHECK(ok, "fog parse: parse_lights succeeds");
      CHECK(std::abs(lights.fog_density - 0.02f) < 1e-6f,
            "fog parse: fog_density parsed correctly");
      CHECK(std::abs(lights.fog_floor - (-2.0f)) < 1e-6f,
            "fog parse: fog_floor parsed correctly");
      CHECK(std::abs(lights.fog_falloff - 40.0f) < 1e-6f,
            "fog parse: fog_falloff parsed correctly");
      CHECK(std::abs(lights.fog_color[2] - 0.9f) < 1e-5f,
            "fog parse: fog_color[2] parsed correctly");
      return failures;
  }
  ```

- [ ] **Step 2: Run lighting-garden suite and verify RED**

  ```bash
  make -C MatterEngine3/tests run-lighting-garden 2>&1 | tail -10
  ```

  Expected: compile error — `VulkanVolumetricsSettings` undefined; fog fields missing from `WorldLights`.

- [ ] **Step 3: Add `VulkanVolumetricsSettings` to `world_session.h`**

  In `MatterEngine3/include/matter/world_session.h`, after `VulkanLightingOverrides`:

  ```cpp
  struct VulkanVolumetricsSettings {
      bool  enabled        = false;
      float temporal_blend = 0.85f;   // history weight per froxel (0=all-fresh, 1=all-history)
      float phase_g        = 0.3f;    // Henyey-Greenstein asymmetry parameter
      // World fog override multipliers (1.0 = use world-schema values).
      float fog_density_mul  = 1.0f;
      float fog_floor_offset = 0.0f;  // additive offset to fog_floor (m)
      float fog_falloff_mul  = 1.0f;
      float fog_color_mul[3] = {1.0f, 1.0f, 1.0f};
      float fog_wind_mul[3]  = {1.0f, 1.0f, 1.0f};
      // Debug view selector forwarded to VkSceneLighting::vol_debug_view (0=off,
      // 1=density, 2=scatter, 3=integrated); set by Viewer UI (Task 10).
      float vol_debug_view   = 0.0f;
  };
  ```

  In `RenderOptions`, after `vulkan_lighting`:
  ```cpp
  VulkanVolumetricsSettings vulkan_volumetrics{};
  ```

- [ ] **Step 4: Add fog fields to `WorldLights` and extend `parse_lights`**

  In `MatterEngine3/src/world_lights.h`, extend `WorldLights`:

  ```cpp
  // Height-fog world parameters. Defaults produce zero-density fog so worlds
  // without fog lines render unchanged.
  float fog_density  = 0.0f;        // base extinction density (1/m)
  float fog_floor    = 0.0f;        // y-coordinate of fog floor (m)
  float fog_falloff  = 30.0f;       // exp falloff scale (m)
  float fog_color[3] = {0.9f, 0.92f, 0.95f}; // scattering albedo RGB
  float fog_wind[3]  = {0.0f, 0.0f, 0.0f};   // world wind vector (m/s)
  ```

  In `MatterEngine3/src/world_lights.cpp`, extend `parse_lights` to handle the `fog` token. The current line 43 is:

  ```cpp
  if (first != "light") continue;  // non-light lines are ignored
  ```

  Replace it with a branch that handles both `light` and `fog`:

  ```cpp
  if (first == "fog") {
      // fog density <v> | fog floor <v> | fog falloff <v>
      // fog color <r> <g> <b>  | fog wind <x> <y> <z>
      std::string sub;
      if (!(ss >> sub)) { err = "fog: bad line (no subkey): " + trimmed; return false; }
      if (sub == "density") {
          if (std::sscanf(trimmed.c_str(), "fog density %f", &out.fog_density) != 1) {
              err = "fog: bad density line: " + trimmed; return false;
          }
      } else if (sub == "floor") {
          if (std::sscanf(trimmed.c_str(), "fog floor %f", &out.fog_floor) != 1) {
              err = "fog: bad floor line: " + trimmed; return false;
          }
      } else if (sub == "falloff") {
          if (std::sscanf(trimmed.c_str(), "fog falloff %f", &out.fog_falloff) != 1) {
              err = "fog: bad falloff line: " + trimmed; return false;
          }
      } else if (sub == "color") {
          if (std::sscanf(trimmed.c_str(), "fog color %f %f %f",
                          &out.fog_color[0], &out.fog_color[1], &out.fog_color[2]) != 3) {
              err = "fog: bad color line: " + trimmed; return false;
          }
      } else if (sub == "wind") {
          if (std::sscanf(trimmed.c_str(), "fog wind %f %f %f",
                          &out.fog_wind[0], &out.fog_wind[1], &out.fog_wind[2]) != 3) {
              err = "fog: bad wind line: " + trimmed; return false;
          }
      } else {
          err = "fog: unknown subkey '" + sub + "': " + trimmed; return false;
      }
      continue;
  }
  if (first != "light") continue;  // other non-light, non-fog lines are ignored
  ```

  This mirrors the same `sscanf`-based pattern used by the existing `light sun` / `light sky` / `light spot` branches.

  Also in `MatterEngine3/src/part_graph.cpp`, in `read_manifest` at line 487, add a skip for `fog` lines immediately after the existing `light` skip:

  ```cpp
  if (name == "light") continue;  // light lines are owned by world_lights::parse_lights
  if (name == "fog")   continue;  // fog lines are owned by world_lights::parse_lights
  ```

  Without this, `read_manifest` treats `fog` as a schema name and hard-errors when it encounters `density` as an unknown flag token.

- [ ] **Step 5: Run suite and verify GREEN**

  ```bash
  make -C MatterEngine3/tests run-lighting-garden 2>&1 | tail -10
  ```

  Expected: `ALL PASS`.

- [ ] **Step 6: Commit Task 4**

  ```bash
  git add MatterEngine3/include/matter/world_session.h \
           MatterEngine3/src/world_lights.h MatterEngine3/src/world_lights.cpp \
           MatterEngine3/src/part_graph.cpp \
           MatterEngine3/src/matter_engine.cpp \
           MatterEngine3/tests/lighting_garden_tests.cpp
  git commit -m "feat(volumetrics): VulkanVolumetricsSettings + world-schema height-fog fields"
  ```

---

## Task 5: `VkVolumetrics` module scaffold

**Files:**
- Create: `MatterEngine3/src/render/vk_volumetrics.h`
- Create: `MatterEngine3/src/render/vk_volumetrics.cpp`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h` — add `std::unique_ptr<VkVolumetrics> volumetrics_` member
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp` — construct, initialize, and record volumetrics between GI à-trous and composite; add pipeline creation call in `create_pipeline`; wire `update_emitters` call
- Modify: `MatterEngine3/tests/vulkan_smoke_tests.cpp` — add smoke test that volumetrics-off renders identically (composite pixel unchanged)

**Interfaces:**
- Consumes: `matter::VulkanDevice`, `VkImageResource` / `create_image` / `create_buffer` / `record_image_transition` (existing); `matter::find_spirv` for SPIR-V modules (stubs until Tasks 6–8); `GpuVolumeEmitter` (Task 3); `VulkanVolumetricsSettings` (Task 4)
- Produces:
  - `VkVolumetrics::init(VulkanDevice&, error)` → creates 3 persistent `VkImageResource` (vol_media, vol_scatter[2], vol_integrated), curl-noise 3D texture, emitter SSBO, descriptor sets, stub pass-through compute pipelines
  - `VkVolumetrics::update_settings(VulkanVolumetricsSettings)` — called each frame
  - `VkVolumetrics::update_emitters(std::vector<GpuVolumeEmitter>)` — uploads SSBO
  - `VkVolumetrics::record(VkCommandBuffer, frame_slot, depth_image, tlas, matrices, frame_time)` → returns immediately (no-op) when `!settings_.enabled`; records barriers + dispatches when enabled
  - `VkVolumetrics::vol_integrated_image()` → `const VkImageResource&` — sampled by composite (Task 9)
  - `VkVolumetrics::ray_query_available()` → `bool` — checked at init; disables module and logs if false

- [ ] **Step 1: Write failing smoke test (volumetrics-off parity)**

  In `MatterEngine3/tests/vulkan_smoke_tests.cpp`, in the existing raster smoke mode, read back a composite pixel with volumetrics off (the default), record its value, re-render with a `VulkanVolumetricsSettings{enabled:false}` and assert it matches:

  ```cpp
  static void test_volumetrics_off_parity(VkSceneRenderer& renderer,
                                          const matter::VulkanFrame& frame,
                                          std::string& error) {
      matter::VulkanVolumetricsSettings off{};
      off.enabled = false;
      renderer.set_volumetrics_settings(off);
      // Re-render and read a pixel — must be identical to a render without volumetrics.
      const auto pixel_a = renderer.readback_raster_pixel(64, 64, error);
      renderer.set_volumetrics_settings(off);  // still off
      const auto pixel_b = renderer.readback_raster_pixel(64, 64, error);
      CHECK(std::abs(pixel_a.hdr.x - pixel_b.hdr.x) < 0.001f,
            "volumetrics off: composite pixel is stable");
  }
  ```

- [ ] **Step 2: Run smoke suite and verify RED**

  ```bash
  cd MatterViewer && make build/windows/vulkan_smoke_tests.exe HAVE_CUDA=1 \
    HAVE_STREAMLINE=1 \
    STREAMLINE_PATH=/d/SDKs/streamline-sdk-v2.12.0 \
    STREAMLINE_DLL_DIR=/d/SDKs/streamline-sdk-v2.12.0/bin/x64/development -j1
  ```

  Expected: compile error — `set_volumetrics_settings` does not exist; `VkVolumetrics` not declared.

- [ ] **Step 3: Implement `vk_volumetrics.h`**

  Create `MatterEngine3/src/render/vk_volumetrics.h`:

  ```cpp
  #pragma once
  #include <vulkan/vulkan.h>
  #include <array>
  #include <cstdint>
  #include <string>
  #include <vector>
  #include "vk_resources.h"
  #include "render/vk_emitter_gather.h"
  #include "matter/world_session.h"

  namespace matter { class VulkanDevice; }
  namespace viewer {
  struct FrameMatrices;

  class VkVolumetrics {
  public:
      static constexpr uint32_t kGridW   = 160;
      static constexpr uint32_t kGridH   = 90;
      static constexpr uint32_t kGridD   = 128;
      static constexpr uint32_t kMaxEmitters = 256;
      static constexpr float    kFarRange    = 300.0f;
      static constexpr uint32_t kNoiseSize   = 32;  // 32³ curl-noise texture

      explicit VkVolumetrics(matter::VulkanDevice& vulkan);
      ~VkVolumetrics();
      VkVolumetrics(const VkVolumetrics&) = delete;
      VkVolumetrics& operator=(const VkVolumetrics&) = delete;

      bool init(std::string& error);
      void update_settings(const matter::VulkanVolumetricsSettings& s,
                           const world_lights::WorldLights& fog);
      void update_emitters(const std::vector<GpuVolumeEmitter>& emitters);

      // Record the three volumetric compute passes with image barriers.
      // No-op (instant return) when !settings_.enabled or when ray_query
      // is unavailable. depth_view and tlas are the same depth attachment and
      // TLAS used by the RT passes already in flight.
      bool record(VkCommandBuffer cmd, uint32_t frame_slot,
                  const matter::VkImageResource& depth,
                  VkAccelerationStructureKHR tlas,
                  const FrameMatrices& matrices,
                  float frame_time_s,
                  std::string& error);

      // Vol_integrated sampled by composite.frag (binding added in Task 9).
      const matter::VkImageResource& vol_integrated() const { return vol_integrated_; }

      bool available() const { return ray_query_available_; }

  private:
      bool create_noise_texture(std::string& error);
      bool create_volume_images(std::string& error);
      bool create_emitter_ssbo(std::string& error);
      bool create_descriptor_sets(std::string& error);
      bool create_pipelines(std::string& error);

      matter::VulkanDevice&  vulkan_;
      bool ray_query_available_ = false;
      bool initialized_ = false;

      // Three persistent 160×90×128 RGBA16F volume images.
      matter::VkImageResource vol_media_;          // density pass output
      std::array<matter::VkImageResource, 2> vol_scatter_;  // ping-pong temporal
      matter::VkImageResource vol_integrated_;     // integration output (sampled by composite)

      // 32³ tiling curl-noise 3D texture (R8G8B8A8_UNORM), generated at init.
      matter::VkImageResource noise_texture_;

      // Emitter SSBO: GpuVolumeEmitter[256] + uint32_t count header.
      matter::VkBufferResource emitter_ssbo_;
      uint32_t emitter_count_   = 0;
      uint32_t ping_index_      = 0;   // which vol_scatter_ is "current history"
      uint32_t frame_index_     = 0;   // monotonic, used for Bayer subsample rotation

      // Descriptor sets (one per image in the vol images, one for emitter SSBO).
      VkDescriptorPool      descriptor_pool_  = VK_NULL_HANDLE;
      VkDescriptorSetLayout density_set_layout_  = VK_NULL_HANDLE;
      VkDescriptorSetLayout scatter_set_layout_  = VK_NULL_HANDLE;
      VkDescriptorSetLayout integrate_set_layout_ = VK_NULL_HANDLE;
      VkDescriptorSet       density_set_   = VK_NULL_HANDLE;
      VkDescriptorSet       scatter_set_   = VK_NULL_HANDLE;
      VkDescriptorSet       integrate_set_ = VK_NULL_HANDLE;

      // Compute pipelines.
      VkPipelineLayout density_layout_   = VK_NULL_HANDLE;
      VkPipeline       density_pipeline_ = VK_NULL_HANDLE;
      VkPipelineLayout scatter_layout_   = VK_NULL_HANDLE;
      VkPipeline       scatter_pipeline_ = VK_NULL_HANDLE;
      VkPipelineLayout integrate_layout_ = VK_NULL_HANDLE;
      VkPipeline       integrate_pipeline_ = VK_NULL_HANDLE;

      matter::VulkanVolumetricsSettings settings_{};
      world_lights::WorldLights         fog_{};
  };

  } // namespace viewer
  ```

- [ ] **Step 4: Implement `vk_volumetrics.cpp` scaffold**

  Implement `VkVolumetrics::init`: check `vulkan_.ray_tracing_available()` — if false, log `"VkVolumetrics: VK_KHR_ray_query unavailable, volumetrics disabled"` to stderr, set `ray_query_available_ = false`, return true (non-fatal; feature simply disabled). If available: call `create_volume_images`, `create_noise_texture`, `create_emitter_ssbo`, `create_descriptor_sets`, `create_pipelines` in order. Fail-closed on any error.

  `create_volume_images`: call `matter::create_image(vulkan_, VK_IMAGE_TYPE_3D, VK_FORMAT_R16G16B16A16_SFLOAT, {kGridW, kGridH, kGridD}, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, vol_media_, error)` and repeat for both `vol_scatter_` and `vol_integrated_`.

  `create_noise_texture`: generate 32³ RGBA8 curl noise on the CPU (simple 3-component gradient noise curl with tiling; store in a `std::vector<uint8_t>`), upload to a `VkImageResource` with `VK_IMAGE_TYPE_3D` and `VK_FORMAT_R8G8B8A8_UNORM` usage `SAMPLED | TRANSFER_DST`.

  `create_emitter_ssbo`: `matter::create_buffer(vulkan_, sizeof(uint32_t) + kMaxEmitters * sizeof(GpuVolumeEmitter), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, emitter_ssbo_, error)`.

  `create_pipelines`: for each of the three shaders (`vol_density.comp.spv`, `vol_scatter.comp.spv`, `vol_integrate.comp.spv`) use `matter::find_spirv(name)` — if the view is empty (not yet compiled by Windows build), create a trivial pass-through pipeline that dispatches but writes identity values; log `"VkVolumetrics: %s not found in embedded SPIR-V, using stub"`. This allows the scaffold to function before the shader handoff.

  `record`: if `!ray_query_available_ || !settings_.enabled` return true immediately. Otherwise:
  1. Transition `vol_media_` to `VK_IMAGE_LAYOUT_GENERAL` for compute write.
  2. Dispatch density pipeline (groups: `ceil(kGridW/4) × ceil(kGridH/4) × kGridD`).
  3. Barrier `vol_media_` → `SHADER_READ_ONLY_OPTIMAL`; transition `vol_scatter_[ping_index_]` → `GENERAL` for write.
  4. Dispatch scatter pipeline.
  5. Barrier scatter → `SHADER_READ_ONLY_OPTIMAL`; transition `vol_integrated_` → `GENERAL`.
  6. Dispatch integrate pipeline.
  7. Barrier `vol_integrated_` → `SHADER_READ_ONLY_OPTIMAL` for composite sampling.
  8. Ping-pong: `ping_index_ ^= 1; ++frame_index_`.

  `update_emitters`: map `emitter_ssbo_`, write `uint32_t count` header, write up to `kMaxEmitters` `GpuVolumeEmitter` records, unmap.

- [ ] **Step 5: Wire into `VkSceneRenderer`**

  In `vk_scene_renderer.h`, add:
  ```cpp
  #include "render/vk_volumetrics.h"
  // ...
  std::unique_ptr<viewer::VkVolumetrics> volumetrics_;
  // ...
  void set_volumetrics_settings(const matter::VulkanVolumetricsSettings& s);
  void set_volumetrics_fog(const world_lights::WorldLights& fog);
  ```

  In `vk_scene_renderer.cpp`:
  - Construct `volumetrics_` in `VkSceneRenderer::init` after `create_pipeline`.
  - In `record_cull_and_render` (in `record_raster`'s path, after `record_gi_atrous` returns, before the `visibility_` → `SHADER_READ_ONLY` transitions): call `volumetrics_->record(frame.command_buffer, frame.frame_slot, depth_, tlas_handle, matrices, frame_time, error)`.
  - Add `set_volumetrics_settings` / `set_volumetrics_fog` implementations that forward to `volumetrics_->update_settings`.

- [ ] **Step 6: Handoff checkpoint — request Windows rebuild**

  No GPU validation can proceed until `embedded_spirv.h` is regenerated by Jack's MSYS2 build. Stop here and request a Windows rebuild:

  ```
  HANDOFF: Task 5 scaffold is complete. The three new shader stubs
  (vol_density.comp, vol_scatter.comp, vol_integrate.comp) exist as
  empty pass-through files in MatterEngine3/shaders_vk/ and must be
  compiled into embedded_spirv.h by the MSYS2 build before GPU tests.
  Request: make windows (clean rebuild after the new header fields).
  ```

- [ ] **Step 7: Rebuild and run smoke test (volumetrics-off parity)**

  After Windows rebuild:
  ```powershell
  $env:MATTER_VK_SMOKE_MODE='raster'
  MatterViewer\build\windows\vulkan_smoke_tests.exe
  ```

  Expected: `ALL PASS`, `validation errors: 0`. The `test_volumetrics_off_parity` check confirms the composite pixel is unaffected when volumetrics is off.

- [ ] **Step 8: Commit Task 5**

  ```bash
  git add MatterEngine3/src/render/vk_volumetrics.h \
           MatterEngine3/src/render/vk_volumetrics.cpp \
           MatterEngine3/src/render/vk_scene_renderer.h \
           MatterEngine3/src/render/vk_scene_renderer.cpp \
           MatterEngine3/shaders_vk/vol_density.comp \
           MatterEngine3/shaders_vk/vol_scatter.comp \
           MatterEngine3/shaders_vk/vol_integrate.comp \
           MatterEngine3/shaders_vk/vol_common.glsl \
           MatterEngine3/tests/vulkan_smoke_tests.cpp
  git commit -m "feat(volumetrics): VkVolumetrics module scaffold with stub compute pipelines"
  ```

---

## Task 6: `vol_common.glsl` and `vol_density.comp`

**Files:**
- Create: `MatterEngine3/shaders_vk/vol_common.glsl` — grid constants, slice↔depth mapping, shared structs
- Modify: `MatterEngine3/shaders_vk/vol_density.comp` — replace stub with full density evaluation
- Modify: `MatterEngine3/src/render/vk_volumetrics.cpp` — add `VolDensityConstants` push constant struct; dispatch with correct workgroup counts and fog / emitter uniform bindings
- Test: GPU test in `MatterEngine3/tests/gpu_cull_tests.cpp` (or a new `vol_gpu_tests.cpp`) using `GALLIUM_DRIVER=d3d12`

**Interfaces:**
- Consumes: `emitter_ssbo_` (Task 5); `noise_texture_` (Task 5); push constants: `VolDensityConstants` (fog params, emitter count, frame time, camera pos, clip_to_world)
- Produces: `vol_media` (RGBA16F, per-froxel scattering × density in RGB, extinction in A) — consumed by Task 7

- [ ] **Step 1: Write failing GPU density readback test**

  Add to `MatterEngine3/tests/gpu_cull_tests.cpp` (or create `vol_gpu_tests.cpp`, add `run-volgpu` target in `Makefile`):

  ```cpp
  // Test: place one point-emitter at world origin, camera at (0,0,10).
  // Froxel at grid center should have non-zero extinction after vol_density.
  static int test_vol_density_emitter_fill() {
      int failures = 0;
      // ... (set up VkVolumetrics, inject one GpuVolumeEmitter at origin,
      //      dispatch density pass, readback vol_media via a staging buffer,
      //      verify that the froxel nearest to world origin has .a > 0)
      // CPU reference: evaluate cone_density(world_pos_of_froxel, emitter)
      // and compare with GPU readback.
      return failures;
  }
  ```

  The test must set up `VkVolumetrics` in isolation, call `update_emitters` with a known emitter, dispatch only the density pass, readback `vol_media`, and compare against a CPU reference implementation of the plume density formula.

- [ ] **Step 2: Handoff checkpoint — request Windows rebuild of vol_density.comp**

  The shader is authored in this step and must be compiled by MSYS2 before the GPU test can run. Stop and request the rebuild.

- [ ] **Step 3: Implement `vol_common.glsl`**

  Create `MatterEngine3/shaders_vk/vol_common.glsl`:

  ```glsl
  // vol_common.glsl — Froxel grid constants and utility functions.
  // Include this in all vol_*.comp shaders.

  #define VOL_W   160
  #define VOL_H   90
  #define VOL_D   128
  #define VOL_FAR 300.0   // far range (m)

  // Map linear slice index [0, VOL_D) to view-space depth (m).
  // Exponential distribution: slice 0 = near clip (~0.1 m), slice 127 = 300 m.
  float slice_to_depth(float slice) {
      const float near = 0.1;
      const float far  = VOL_FAR;
      return near * pow(far / near, slice / float(VOL_D - 1));
  }

  // Map view-space depth (m) to a normalized slice [0,1].
  float depth_to_slice_n(float depth) {
      const float near = 0.1;
      const float far  = VOL_FAR;
      return log(depth / near) / log(far / near);
  }

  // Henyey-Greenstein phase function.
  float hg_phase(float cos_theta, float g) {
      float denom = 1.0 + g * g - 2.0 * g * cos_theta;
      return (1.0 - g * g) / (4.0 * 3.14159265 * pow(denom, 1.5));
  }

  // GPU volume emitter (std430, 64 bytes — mirrors GpuVolumeEmitter in C++).
  struct GpuVolumeEmitter {
      vec3  world_pos;
      float radius;
      vec3  world_dir;
      float spread;
      float length;
      float density;
      float rise;
      float turbulence;
      vec3  color;
      float pad;
  };
  ```

- [ ] **Step 4: Implement `vol_density.comp`**

  ```glsl
  #version 460
  #extension GL_GOOGLE_include_directive : require
  #include "vol_common.glsl"

  layout(local_size_x = 4, local_size_y = 4, local_size_z = 1) in;

  layout(set = 0, binding = 0, rgba16f) uniform image3D vol_media;
  layout(set = 0, binding = 1) uniform sampler3D noise_tex;  // 32^3 curl noise
  layout(set = 0, binding = 2, std430) readonly buffer EmitterSSBO {
      uint emitter_count;
      GpuVolumeEmitter emitters[];
  };

  layout(push_constant) uniform VolDensityConstants {
      mat4  clip_to_world;
      vec3  camera_pos;
      float frame_time;   // elapsed time (s) for noise scroll
      // World fog params.
      float fog_density;
      float fog_floor;
      float fog_falloff;
      float pad0;
      vec3  fog_color;
      float pad1;
      vec3  fog_wind;
      float pad2;
  } pc;

  // Evaluate 2-octave curl-noise warp. The 32³ noise texture stores 3-component
  // gradient noise in RGB; we read two octaves at different scales and combine.
  vec3 curl_noise_warp(vec3 p, float strength) {
      vec3 uv0 = fract(p / 8.0);
      vec3 uv1 = fract(p / 4.0 + vec3(0.3, 0.7, 0.1));
      vec3 n0 = texture(noise_tex, uv0).rgb * 2.0 - 1.0;
      vec3 n1 = texture(noise_tex, uv1).rgb * 2.0 - 1.0;
      return (n0 + 0.5 * n1) * strength;
  }

  // Evaluate one emitter's contribution at world position p.
  // Returns (albedo.rgb * density, extinction) packed as vec4.
  vec4 emitter_density(GpuVolumeEmitter e, vec3 p, float time) {
      // Scroll position along plume axis with rise + wind.
      vec3 scroll = e.world_dir * e.rise * time + pc.fog_wind * time;
      vec3 warped = p - e.world_pos - scroll;
      // Curl-noise warp.
      warped += curl_noise_warp(p * 0.25, e.turbulence);
      // Axial distance along dir.
      float t_axial = dot(warped, e.world_dir);
      if (t_axial < 0.0 || t_axial > e.length) return vec4(0.0);
      // Radial distance.
      float cone_r = e.radius + e.spread * t_axial;
      vec3 radial = warped - t_axial * e.world_dir;
      float r = length(radial);
      if (r >= cone_r) return vec4(0.0);
      // Smooth radial falloff + axial fade.
      float radial_fall = 1.0 - smoothstep(0.0, cone_r, r);
      float axial_fade  = 1.0 - smoothstep(0.7 * e.length, e.length, t_axial);
      float d = e.density * radial_fall * axial_fade;
      return vec4(e.color * d, d);
  }

  void main() {
      ivec3 gid = ivec3(gl_GlobalInvocationID);
      if (gid.x >= VOL_W || gid.y >= VOL_H) return;
      // Iterate all Z slices for this XY column.
      for (int z = 0; z < VOL_D; ++z) {
          // Compute froxel world position at center.
          float depth = slice_to_depth(float(z) + 0.5);
          vec2 uv = (vec2(gid.xy) + 0.5) / vec2(VOL_W, VOL_H);
          // NDC in [−1,1].
          vec2 ndc = uv * 2.0 - 1.0;
          // Reconstruct view-space ray from clip_to_world.
          vec4 clip_far = vec4(ndc, 1.0, 1.0);
          vec4 world_far = pc.clip_to_world * clip_far;
          world_far /= world_far.w;
          vec3 ray_dir = normalize(world_far.xyz - pc.camera_pos);
          vec3 world_pos = pc.camera_pos + ray_dir * depth;

          // Height fog: base_density * exp(-(y - floor) / falloff).
          float y_above_floor = max(world_pos.y - pc.fog_floor, 0.0);
          float fog_d = pc.fog_density * exp(-y_above_floor / max(pc.fog_falloff, 0.01));
          vec4 media = vec4(pc.fog_color * fog_d, fog_d);

          // Emitter contributions.
          uint count = min(emitter_count, 256u);  // guard against corrupt SSBO (cap matches kMaxEmitters)
          for (uint i = 0; i < count; ++i) {
              media += emitter_density(emitters[i], world_pos, pc.frame_time);
          }

          imageStore(vol_media, ivec3(gid.x, gid.y, z), media);
      }
  }
  ```

- [ ] **Step 5: Update `VkVolumetrics::record` to push `VolDensityConstants`**

  In `vk_volumetrics.cpp`, fill and push the `VolDensityConstants` struct (clip_to_world from `FrameMatrices::clip_to_world`, camera_pos from the frame camera position passed through, fog fields from `fog_`, frame time accumulated from calls to `record`). Dispatch `ceil(kGridW/4) × ceil(kGridH/4) × 1` workgroups.

- [ ] **Step 6: Handoff checkpoint — Windows rebuild before GPU test**

  ```
  HANDOFF: vol_density.comp and vol_common.glsl authored. Request
  Windows rebuild (make windows, clean rebuild) to regenerate
  embedded_spirv.h with matter_spirv_vol_density_comp_spv.
  ```

- [ ] **Step 7: Run GPU density test**

  ```bash
  cd MatterEngine3/tests && make vol-gpu-tests && GALLIUM_DRIVER=d3d12 ./vol_gpu_tests
  ```

  Expected: `ALL PASS`. The CPU-reference plume density at the test froxel center matches the GPU readback within floating-point tolerance.

- [ ] **Step 8: Commit Task 6**

  ```bash
  git add MatterEngine3/shaders_vk/vol_common.glsl \
           MatterEngine3/shaders_vk/vol_density.comp \
           MatterEngine3/src/render/vk_volumetrics.h \
           MatterEngine3/src/render/vk_volumetrics.cpp \
           MatterEngine3/tests/vol_gpu_tests.cpp \
           MatterEngine3/tests/Makefile
  git commit -m "feat(volumetrics): vol_density.comp — height fog + emitter cone evaluation"
  ```

---

## Task 7: `vol_scatter.comp` — sun shadow rays + temporal reprojection

**Files:**
- Modify: `MatterEngine3/shaders_vk/vol_scatter.comp` — replace stub with full scatter evaluation
- Modify: `MatterEngine3/src/render/vk_volumetrics.cpp` — add `VolScatterConstants` push constant; update descriptor sets to bind TLAS + previous vol_scatter history + depth; manage ping-pong index; write `previous_world_to_clip` from `FrameMatrices`

**Interfaces:**
- Consumes: `vol_media` (Task 6); `vol_scatter_[1 - ping_index_]` as history; `depth_` image; `tlas` handle; `FrameMatrices::world_to_clip` (current) and `previous_world_to_clip` (computed in `upload_frame_constants` at `vk_scene_renderer.cpp:4329` as `temporal_frame_.previous_jittered.world_to_clip` when `temporal_frame_.attempt_token != 0`, else `matrices.world_to_clip` — use the same conditional, not the raw field); `VulkanVolumetricsSettings::temporal_blend`, `phase_g`; `VkSceneLighting` sun/sky direction+color
- Produces: `vol_scatter_[ping_index_]` — consumed by Task 8

- [ ] **Step 1: Write failing GPU scatter test (transmittance sanity)**

  In `vol_gpu_tests.cpp`, add a test that:
  1. Dispatches `vol_density` with a homogeneous fog field.
  2. Dispatches `vol_scatter` with the sun above the scene (guaranteed visibility).
  3. Readbacks `vol_scatter_[0]` and checks that a froxel under the sun has positive in-scatter (`.rgb` > 0).

  ```cpp
  static int test_vol_scatter_sun_lit() {
      int failures = 0;
      // ... setup VkVolumetrics, dispatch density + scatter, readback vol_scatter
      // Check that a center froxel's scatter.r > 0 (sun contributes).
      CHECK(scatter_pixel.r > 0.0f, "scatter: froxel under clear sky has positive inscatter");
      return failures;
  }
  ```

- [ ] **Step 2: Handoff checkpoint — request Windows rebuild**

  ```
  HANDOFF: vol_scatter.comp authored. Request Windows rebuild to
  regenerate embedded_spirv.h with matter_spirv_vol_scatter_comp_spv.
  ```

- [ ] **Step 3: Implement `vol_scatter.comp`**

  ```glsl
  #version 460
  #extension GL_GOOGLE_include_directive : require
  #extension GL_EXT_ray_query : require
  #include "vol_common.glsl"

  layout(local_size_x = 4, local_size_y = 4, local_size_z = 1) in;

  layout(set = 0, binding = 0)          uniform sampler3D    vol_media_tex;
  layout(set = 0, binding = 1, rgba16f) uniform image3D      vol_scatter_out;
  layout(set = 0, binding = 2)          uniform sampler3D    vol_scatter_history;
  layout(set = 0, binding = 3)          uniform sampler2D    depth_tex;
  layout(set = 0, binding = 4)          uniform accelerationStructureEXT tlas;

  layout(push_constant) uniform VolScatterConstants {
      mat4  clip_to_world;
      mat4  prev_world_to_clip;
      vec3  camera_pos;
      float frame_index;   // monotonic uint cast to float (for Bayer)
      vec3  sun_dir;       // normalized direction FROM sun (toward scene)
      float sun_intensity;
      vec3  sun_color;
      float phase_g;
      vec3  sky_color;
      float temporal_blend;
      uint  history_valid;  // 0 = first frame / resize / teleport → blend=0
      float pad0, pad1, pad2;
  } pc;

  // 2×2 Bayer subsampling: returns true if this froxel traces a fresh shadow ray.
  bool bayer_active(ivec2 xy, int frame_i) {
      int pattern = (xy.x & 1) + 2 * (xy.y & 1);
      return pattern == (frame_i & 3);
  }

  void main() {
      ivec3 gid = ivec3(gl_GlobalInvocationID);
      if (gid.x >= VOL_W || gid.y >= VOL_H) return;

      for (int z = 0; z < VOL_D; ++z) {
          vec4 media = texelFetch(vol_media_tex, ivec3(gid.x, gid.y, z), 0);
          float extinction = media.a;

          vec4 result = vec4(0.0, 0.0, 0.0, extinction);

          if (extinction > 1e-5) {
              // World position of froxel center (same reconstruction as density).
              float depth = slice_to_depth(float(z) + 0.5);
              vec2 uv = (vec2(gid.xy) + 0.5) / vec2(VOL_W, VOL_H);
              vec2 ndc = uv * 2.0 - 1.0;
              vec4 clip_far = vec4(ndc, 1.0, 1.0);
              vec4 world_h  = pc.clip_to_world * clip_far;
              world_h /= world_h.w;
              vec3 ray_dir  = normalize(world_h.xyz - pc.camera_pos);
              vec3 world_pos = pc.camera_pos + ray_dir * depth;

              vec3 inscatter = vec3(0.0);

              bool trace_fresh = bayer_active(gid.xy, int(pc.frame_index));
              if (trace_fresh) {
                  // Sun shadow ray via rayQueryEXT (opaque geometry, tMax 300 m).
                  vec3 to_sun = -normalize(pc.sun_dir);
                  rayQueryEXT q;
                  rayQueryInitializeEXT(q, tlas,
                      gl_RayFlagsTerminateOnFirstHitEXT |
                      gl_RayFlagsSkipClosestHitShaderEXT,
                      0xFF, world_pos + to_sun * 0.01, 0.01, to_sun, 300.0);
                  while (rayQueryProceedEXT(q)) {}
                  float visibility = (rayQueryGetIntersectionTypeEXT(
                      q, true) == gl_RayQueryCommittedIntersectionNoneEXT)
                      ? 1.0 : 0.0;

                  // HG phase with sun direction.
                  float cos_theta = dot(to_sun, ray_dir);
                  float phase = hg_phase(cos_theta, pc.phase_g);

                  // Sun contribution.
                  inscatter += pc.sun_color * pc.sun_intensity * visibility * phase;

                  // Analytic sky: matching sky_environment() in rt_lighting.rgen line 119.
                  float horizon = clamp(ray_dir.y * 0.5 + 0.5, 0.0, 1.0);
                  vec3 sky = pc.sky_color * mix(0.2, 1.25, horizon);
                  inscatter += sky * hg_phase(0.0, pc.phase_g);

                  result.rgb = inscatter * media.rgb;

                  // Temporal blend: reproject froxel through prev frame.
                  vec4 prev_clip = pc.prev_world_to_clip * vec4(world_pos, 1.0);
                  prev_clip /= prev_clip.w;
                  vec3 prev_uvw = vec3(prev_clip.xy * 0.5 + 0.5,
                                       depth_to_slice_n(slice_to_depth(float(z) + 0.5)));
                  bool in_frustum = all(greaterThan(prev_uvw, vec3(0.01))) &&
                                    all(lessThan(prev_uvw, vec3(0.99)));
                  float blend = (pc.history_valid != 0u && in_frustum) ? pc.temporal_blend : 0.0;
                  // Detect large extinction change (history stale).
                  vec4 history = texture(vol_scatter_history, prev_uvw);
                  float hist_ext = history.a;
                  if (abs(extinction - hist_ext) > 0.5 * max(extinction, hist_ext) + 0.01)
                      blend = 0.0;
                  result.rgb = mix(result.rgb, history.rgb, blend);
              } else {
                  // Non-fresh froxel: use previous frame's value verbatim.
                  // Reproject and sample history as above.
                  float depth2 = slice_to_depth(float(z) + 0.5);
                  vec2 uv2 = (vec2(gid.xy) + 0.5) / vec2(VOL_W, VOL_H);
                  vec2 ndc2 = uv2 * 2.0 - 1.0;
                  vec4 clip2 = vec4(ndc2, 1.0, 1.0);
                  vec4 wh2   = pc.clip_to_world * clip2;
                  wh2 /= wh2.w;
                  vec3 wp2   = pc.camera_pos + normalize(wh2.xyz - pc.camera_pos) * depth2;
                  vec4 prev_clip2 = pc.prev_world_to_clip * vec4(wp2, 1.0);
                  prev_clip2 /= prev_clip2.w;
                  vec3 prev_uvw2 = vec3(prev_clip2.xy * 0.5 + 0.5,
                                        depth_to_slice_n(depth2));
                  bool ok2 = (pc.history_valid != 0u) &&
                             all(greaterThan(prev_uvw2, vec3(0.01))) &&
                             all(lessThan(prev_uvw2, vec3(0.99)));
                  result = ok2 ? texture(vol_scatter_history, prev_uvw2)
                               : vec4(0.0, 0.0, 0.0, extinction);
              }
          }

          imageStore(vol_scatter_out, ivec3(gid.x, gid.y, z), result);
      }
  }
  ```

- [ ] **Step 4: Update `VkVolumetrics` to push `VolScatterConstants` and bind TLAS**

  In `vk_volumetrics.cpp`, `record`: after the density dispatch, set up the scatter descriptor set to bind:
  - binding 0: `vol_media_` (sampled)
  - binding 1: `vol_scatter_[ping_index_]` (storage image, write target)
  - binding 2: `vol_scatter_[1 - ping_index_]` (sampled, history)
  - binding 3: `depth_image` (sampled)
  - binding 4: TLAS (acceleration structure)

  Push `VolScatterConstants`. Fill `prev_world_to_clip` using the same logic as `upload_frame_constants` (`vk_scene_renderer.cpp:4329`): use `temporal_frame_.previous_jittered.world_to_clip` when `temporal_frame_.attempt_token != 0`, otherwise fall back to `matrices.world_to_clip`. Pass this computed matrix into `VkVolumetrics::record` via a `set_volumetrics_prev_matrices` helper on `VkSceneRenderer` (or pass the computed matrix4x4 directly as a parameter to `record`). Set `history_valid = 0` on first frame, resize, or when the ping-pong reset flag is set.

- [ ] **Step 5: GPU scatter test after Windows rebuild**

  ```bash
  cd MatterEngine3/tests && GALLIUM_DRIVER=d3d12 ./vol_gpu_tests
  ```

  Expected: `ALL PASS` including the new scatter sun-lit test.

- [ ] **Step 6: Commit Task 7**

  ```bash
  git add MatterEngine3/shaders_vk/vol_scatter.comp \
           MatterEngine3/src/render/vk_volumetrics.h \
           MatterEngine3/src/render/vk_volumetrics.cpp \
           MatterEngine3/tests/vol_gpu_tests.cpp
  git commit -m "feat(volumetrics): vol_scatter.comp — RT sun shadow + HG phase + temporal reprojection"
  ```

---

## Task 8: `vol_integrate.comp`

**Files:**
- Modify: `MatterEngine3/shaders_vk/vol_integrate.comp` — replace stub with front-to-back accumulation
- Modify: `MatterEngine3/src/render/vk_volumetrics.cpp` — bind integrate descriptor set, push `VolIntegrateConstants`

**Interfaces:**
- Consumes: `vol_scatter_[ping_index_]` (Task 7 output)
- Produces: `vol_integrated` (RGBA16F, rgb=accumulated inscatter, a=transmittance) — consumed by Task 9's composite

- [ ] **Step 1: Write failing GPU integrate test (transmittance monotonicity)**

  In `vol_gpu_tests.cpp`, add:

  ```cpp
  static int test_vol_integrate_transmittance_monotonic() {
      int failures = 0;
      // Dispatch density (uniform fog) + scatter (stub or real) + integrate.
      // Readback vol_integrated along one XY column.
      // Assert: for all z, transmittance[z] >= transmittance[z+1].
      float prev_t = 1.0f;
      for (int z = 0; z < viewer::VkVolumetrics::kGridD; ++z) {
          float t = readback_vol_integrated_transmittance(z);  // test helper
          CHECK(t <= prev_t + 1e-4f,
                "integrate: transmittance is non-increasing along Z");
          prev_t = t;
      }
      return failures;
  }
  ```

- [ ] **Step 2: Implement `vol_integrate.comp`**

  ```glsl
  #version 460
  #extension GL_GOOGLE_include_directive : require
  #include "vol_common.glsl"

  // One thread per XY column. Marches 128 slices front-to-back.
  layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

  layout(set = 0, binding = 0) uniform sampler3D vol_scatter_tex;
  layout(set = 0, binding = 1, rgba16f) uniform image3D vol_integrated;

  layout(push_constant) uniform VolIntegrateConstants {
      vec3  camera_pos;
      float pad;
  } pc;

  void main() {
      ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
      if (xy.x >= VOL_W || xy.y >= VOL_H) return;

      vec3 inscatter   = vec3(0.0);
      float transmittance = 1.0;

      for (int z = 0; z < VOL_D; ++z) {
          vec4 scatter = texelFetch(vol_scatter_tex, ivec3(xy, z), 0);
          float extinction = max(scatter.a, 0.0);
          // Step size in metres at this slice.
          float depth_near = slice_to_depth(float(z));
          float depth_far  = slice_to_depth(float(z) + 1.0);
          float step_m = depth_far - depth_near;

          // Beer-Lambert transmittance for this slice.
          float slice_t = exp(-extinction * step_m);

          // Accumulate inscattered light.
          inscatter += transmittance * scatter.rgb * step_m;
          transmittance *= slice_t;

          // Write this slice. Clamp inscatter to prevent RGBA16F overflow (~65504).
          imageStore(vol_integrated, ivec3(xy, z),
                     vec4(min(inscatter, vec3(60000.0)), transmittance));
      }
  }
  ```

  Dispatch: `ceil(kGridW/1) × ceil(kGridH/1) × 1` — i.e. one thread per XY column, `160 × 90 × 1` groups.

- [ ] **Step 3: Handoff checkpoint — Windows rebuild**

  ```
  HANDOFF: vol_integrate.comp authored. Request Windows rebuild.
  ```

- [ ] **Step 4: GPU integrate test**

  ```bash
  cd MatterEngine3/tests && GALLIUM_DRIVER=d3d12 ./vol_gpu_tests
  ```

  Expected: `ALL PASS` including the transmittance-monotonicity test.

- [ ] **Step 5: Commit Task 8**

  ```bash
  git add MatterEngine3/shaders_vk/vol_integrate.comp \
           MatterEngine3/src/render/vk_volumetrics.cpp \
           MatterEngine3/tests/vol_gpu_tests.cpp
  git commit -m "feat(volumetrics): vol_integrate.comp — front-to-back transmittance accumulation"
  ```

---

## Task 9: Composite application and debug views

**Files:**
- Modify: `MatterEngine3/shaders_vk/composite.frag` — add binding for `vol_integrated` (3D sampler, binding 9) and volumetric application logic; add debug view modes
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp` — extend `create_display_pipeline` / composite descriptor set to include `vol_integrated` sampler; wire `VkVolumetrics::vol_integrated()` image view; add debug view enum values to `VkSceneLighting`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h` — add three volumetric debug view constants to the debug_view enum region

**Interfaces:**
- Consumes: `vol_integrated` (Task 8); `depth_tex` (already in composite set at binding position to determine); `VulkanVolumetricsSettings::enabled` flag (controls whether the volume sample is applied)
- Produces: fogged HDR image consumed by the display transform; DLSS consumes same linear HDR unchanged

- [ ] **Step 1: Extend composite.frag**

  After the existing `layout(set = 0, binding = 8) uniform sampler2D transmission_texture;` line, add:

  ```glsl
  layout(set = 0, binding = 9) uniform sampler3D vol_integrated_tex;
  ```

  Extend `SceneLighting` push constant (the struct in composite.frag must grow — add four floats after `pad2`):
  ```glsl
  float vol_enabled;    // 1.0 = volumetrics on, 0.0 = off (skip volume sample)
  float vol_debug_view; // 0=off, 1=raw density, 2=raw scatter, 3=integrated
  float pad3;
  float pad4;
  ```

  > Note: `VkSceneLighting` C++ struct must grow to match. The current struct is 64 bytes (`static_assert(sizeof(VkSceneLighting) == 64)` at `vk_scene_renderer.h:308`). Append four new floats after the existing `pad2` member — this adds 16 bytes and brings the total to **80 bytes**. The tail of the struct becomes:
  > ```cpp
  > float debug_view = 0.0f;
  > float pad0 = 0.0f;
  > float pad1 = 0.0f;
  > float pad2 = 0.0f;
  > // --- new fields (Task 9) ---
  > float vol_enabled    = 0.0f;
  > float vol_debug_view = 0.0f;
  > float pad3 = 0.0f;
  > float pad4 = 0.0f;
  > ```
  > Update `static_assert(sizeof(VkSceneLighting) == 64)` → `static_assert(sizeof(VkSceneLighting) == 80)`. The composite.frag push constant must have the identical layout (both are `std430` push constants; scalar floats have 4-byte alignment so no implicit padding is inserted). This is a struct/header change — clean-rebuild the Windows object files after this change.

  Also add `#extension GL_GOOGLE_include_directive : require` and `#include "vol_common.glsl"` at the top of `composite.frag` (before `void main()`). This gives access to `depth_to_slice_n`, `slice_to_depth`, and the `VOL_FAR` / near constants without duplicating them.

  Add the depth sampler binding (next free slot after binding 9 = vol_integrated_tex):
  ```glsl
  layout(set = 0, binding = 10) uniform sampler2D depth_tex_vol;
  ```

  In `composite.frag` `main()`, before the `out_hdr` assignment, add a helper inline function for depth linearization. The engine uses `perspective_rh_zo` (ZO = zero-to-one Vulkan depth range, right-handed). Given a raw depth sample `d` in [0,1] from the depth texture, and the grid near/far from `vol_common.glsl` (`near = 0.1`, `far = VOL_FAR = 300.0`):

  ```glsl
  // Linearize ZO depth sample [0,1] → view-space depth (m).
  // Derived from perspective_rh_zo: d = far/(near-far) * (-1) + far*near/((near-far)*view_z)
  // Solving for view_z gives:
  float linearize_depth(float d) {
      const float near = 0.1;
      const float far  = VOL_FAR;
      return (near * far) / (far - d * (far - near));
  }
  ```

  Then the volumetric application block:

  ```glsl
  if (lighting.vol_debug_view > 0.5) {
      // Debug views: reconstruct depth, map to froxel slice, sample vol_integrated.
      float scene_depth = texture(depth_tex_vol, in_uv).r;
      float lin_depth   = linearize_depth(scene_depth);
      float slice_n     = depth_to_slice_n(lin_depth);
      vec4 vol = texture(vol_integrated_tex, vec3(in_uv, clamp(slice_n, 0.0, 1.0)));
      if (lighting.vol_debug_view < 1.5)
          out_hdr = vec4(vol.aaa, 1.0);  // transmittance channel (density view)
      else if (lighting.vol_debug_view < 2.5)
          out_hdr = vec4(vol.rgb, 1.0);  // raw scatter
      else
          out_hdr = vec4(1.0 - vol.a, vol.rgb);  // integrated: show inscatter blend
      return;
  }

  if (lighting.vol_enabled > 0.5) {
      float scene_depth = texture(depth_tex_vol, in_uv).r;
      float lin_depth   = linearize_depth(scene_depth);
      float slice_n     = clamp(depth_to_slice_n(lin_depth), 0.0, 1.0);
      vec4 vol_sample   = texture(vol_integrated_tex, vec3(in_uv, slice_n));
      float T           = vol_sample.a;    // transmittance at scene surface
      vec3  inscatter   = vol_sample.rgb;  // accumulated inscattered light in front
      linear_hdr = linear_hdr * T + inscatter;
  }
  ```

  The depth texture (binding 10) must be the same depth attachment written by the RT/raster passes. Add it to the composite descriptor set in `vk_scene_renderer.cpp` alongside `vol_integrated_tex`.

- [ ] **Step 2: Update composite descriptor set in `vk_scene_renderer.cpp`**

  In `update_composite_descriptor` (search for `selected.composite_descriptor_set`), add a `VkDescriptorImageInfo` for `vol_integrated_tex` pointing to `volumetrics_->vol_integrated().view` with sampler `composite_sampler_` and layout `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`. Update `create_display_pipeline` (line ~1542) to declare the new binding in the composite descriptor set layout. Set `vol_enabled` and `vol_debug_view` in the `VkSceneLighting` push constant before each composite draw.

- [ ] **Step 3: Handoff checkpoint — Windows rebuild**

  ```
  HANDOFF: composite.frag modified. Request Windows rebuild (clean rebuild
  because VkSceneLighting struct grew — old Windows .o files will be stale).
  ```

- [ ] **Step 4: Visual smoke check via FIFO shot**

  After rebuild, launch the viewer and take a shot of Demo world with volumetrics on:

  ```bash
  cd MatterViewer
  GALLIUM_DRIVER=d3d12 MATTER_WORLD=demo MATTER_CMD_FIFO=/tmp/vol_smoke.fifo \
    ./viewer &
  sleep 10
  echo "cam 0 12 30 0 8 0" > /tmp/vol_smoke.fifo
  echo "shot /tmp/vol_smoke_test.png" > /tmp/vol_smoke.fifo
  sleep 5
  echo "quit" > /tmp/vol_smoke.fifo
  ```

  Expected: viewer renders Demo world, shot file is created, viewer self-terminates.

- [ ] **Step 5: Commit Task 9**

  ```bash
  git add MatterEngine3/shaders_vk/composite.frag \
           MatterEngine3/src/render/vk_scene_renderer.h \
           MatterEngine3/src/render/vk_scene_renderer.cpp
  git commit -m "feat(volumetrics): composite.frag applies vol_integrated; add debug views"
  ```

---

## Task 10: Viewer UI

**Files:**
- Modify: `MatterViewer/ui.h` — add volumetrics fields to `ViewerStats`
- Modify: `MatterViewer/ui.cpp` — add "Volumetrics" ImGui section to `draw_debug_panel`
- Modify: `MatterViewer/main.cpp` — propagate `stats.volumetrics` into `RenderOptions::vulkan_volumetrics` each frame

**Interfaces:**
- Consumes: `matter::VulkanVolumetricsSettings` (Task 4); `ViewerStats` (existing); `draw_debug_panel` (existing, lines 216–274 of `ui.cpp`)
- Produces: live Viewer controls for volumetrics enable toggle, fog sliders, phase_g, temporal_blend, and debug view selector

- [ ] **Step 1: Extend `ViewerStats` in `ui.h`**

  After `matter::VulkanLightingOverrides lighting{};` add:

  ```cpp
  matter::VulkanVolumetricsSettings volumetrics{};
  int vol_debug_view = 0;  // 0=off, 1=density, 2=scatter, 3=integrated
  ```

- [ ] **Step 2: Add the Volumetrics panel section in `ui.cpp`**

  In `draw_debug_panel`, after the `if (ImGui::Button("Reset to World")) reset_lighting_controls(s);` line and before `ImGui::End()`:

  ```cpp
  ImGui::SeparatorText("Volumetrics");
  ImGui::Checkbox("Enable##vol", &s.volumetrics.enabled);
  if (s.volumetrics.enabled) {
      ImGui::SliderFloat("Phase g",        &s.volumetrics.phase_g,        0.0f, 0.99f, "%.2f");
      ImGui::SliderFloat("Temporal blend", &s.volumetrics.temporal_blend, 0.0f, 0.99f, "%.2f");
      ImGui::SliderFloat("Fog density",    &s.volumetrics.fog_density_mul,0.0f, 4.0f,  "%.2f");
      ImGui::SliderFloat("Fog falloff",    &s.volumetrics.fog_falloff_mul,0.1f, 4.0f,  "%.2f");
      const char* debug_views[] = {"Off", "Density", "Scatter", "Integrated"};
      ImGui::Combo("Vol debug##vd", &s.vol_debug_view, debug_views, 4);
  }
  ```

- [ ] **Step 3: Propagate into `RenderOptions` in `main.cpp`**

  In `MatterViewer/main.cpp` at line 669 where `options.vulkan_lighting = stats.lighting;` is assigned, add immediately after:
  ```cpp
  options.vulkan_volumetrics = stats.volumetrics;
  options.vulkan_volumetrics.vol_debug_view = static_cast<float>(stats.vol_debug_view);
  ```

- [ ] **Step 4: Commit Task 10**

  ```bash
  git add MatterViewer/ui.h MatterViewer/ui.cpp MatterViewer/main.cpp
  git commit -m "feat(volumetrics): Viewer ImGui section for enable, fog, phase, debug views"
  ```

---

## Task 11: Demo world content

**Files:**
- Modify: `MatterEngine3/examples/world_demo/schemas/` — create or extend a `House.js` schema with `emitVolume` for chimney smoke; create `WaterfallMist.js` schema with mist emitter
- Modify: `MatterEngine3/examples/world_demo/WorldData/Demo/world.manifest` — add chimney and mist parts; add `fog density 0.008` / `fog floor -1.0` / `fog falloff 35.0` / `fog color 0.88 0.90 0.95` lines
- Create: `MatterEngine3/tools/vol_shots.sh` — three-camera FIFO shot script (low-sun god rays, inside fog, plume close-up), self-terminating

**Interfaces:**
- Consumes: `emitVolume` DSL verb (Task 1); `world.manifest` fog parsing (Task 4); `viewer_shots.sh` FIFO protocol
- Produces: three PNG screenshots demonstrating the feature for visual acceptance

- [ ] **Step 1: Extend Demo world schemas and manifest**

  Create `MatterEngine3/examples/world_demo/schemas/ChimneySmoke.js`:

  ```js
  import { Part } from 'part_base';
  export default class ChimneySmoke extends Part {
    build() {
      // Narrow upward plume — classic chimney smoke preset.
      emitVolume({
        pos:        [0, 0, 0],
        dir:        [0, 1, 0],
        radius:     0.35,
        spread:     0.12,
        length:     14,
        density:    0.9,
        color:      [0.82, 0.82, 0.85],
        rise:       1.8,
        turbulence: 0.55
      });
    }
  }
  ```

  Create `MatterEngine3/examples/world_demo/schemas/WaterfallMist.js`:

  ```js
  import { Part } from 'part_base';
  export default class WaterfallMist extends Part {
    build() {
      // Wide low-rise spray — waterfall mist preset.
      emitVolume({
        pos:        [0, 0, 0],
        dir:        [0, -0.3, 1],   // angled outward and slightly down
        radius:     1.2,
        spread:     0.4,
        length:     8,
        density:    0.6,
        color:      [0.95, 0.97, 1.0],
        rise:       0.0,
        turbulence: 0.85
      });
    }
  }
  ```

  Extend `MatterEngine3/examples/world_demo/WorldData/Demo/world.manifest`. The manifest format is `<SchemaName> [expand] [tileset]` — one schema name per line, no coordinates (placement is determined by each schema's JS `build()` function). Append:
  ```
  # Volumetrics emitters (placed via their own build() calls)
  ChimneySmoke
  WaterfallMist
  # Height fog
  fog density 0.008
  fog floor -1.0
  fog falloff 35.0
  fog color 0.88 0.90 0.95
  fog wind 0.3 0.0 0.1
  ```

  Note: `read_manifest` in `part_graph.cpp:487` already skips `light` lines via `if (name == "light") continue;`. The Task 4 `part_graph.cpp` change (described below) must add the parallel skip for `fog` lines: `if (name == "fog") continue;` — otherwise `read_manifest` treats `fog` as a schema name and hard-errors on `density` as an unknown flag token. This one-line addition to `read_manifest` belongs in **Task 4** alongside the `parse_lights` extension. Add it to **Task 4 Step 4** and the **Task 4 commit** (`git add` line).

- [ ] **Step 2: Create `vol_shots.sh`**

  Create `MatterEngine3/tools/vol_shots.sh`:

  ```bash
  #!/usr/bin/env bash
  # Volumetrics visual validation: three fixed-camera shots on Demo world.
  # Usage: cd MatterViewer && GALLIUM_DRIVER=d3d12 ../MatterEngine3/tools/vol_shots.sh <out-dir>
  set -euo pipefail
  OUT="${1:?usage: vol_shots.sh <out-dir>}"
  mkdir -p "$OUT"
  FIFO="/tmp/vol_shots_$$.fifo"
  mkfifo "$FIFO"
  MATTER_WORLD=demo MATTER_CMD_FIFO="$FIFO" GALLIUM_DRIVER=d3d12 \
    stdbuf -oL ./viewer > "$OUT/viewer.log" 2>&1 &
  PID=$!
  trap 'kill $PID 2>/dev/null || true; rm -f "$FIFO"' EXIT
  sleep 12  # allow world bake + Vulkan warm-up

  take_shot() {
    local name="$1"; shift
    echo "cam $*" > "$FIFO"; sleep 2
    echo "shot $OUT/${name}.png" > "$FIFO"; sleep 3
    [ -e "$OUT/${name}.png.done" ] || { echo "TIMEOUT: $name" >&2; exit 1; }
  }

  # Shot 1: low-sun god rays (camera low, looking up toward sun angle).
  take_shot "god_rays"   "-10 2 20  0 8 0"
  # Shot 2: inside-fog (camera at ground level, looking across the scene).
  take_shot "inside_fog" "0 1 15   0 4 0"
  # Shot 3: plume close-up (chimney emitter from 6 m away).
  take_shot "plume"      "4 8 4   0 7 0"

  echo "quit" > "$FIFO"
  wait $PID 2>/dev/null || true
  rm -f "$FIFO"
  echo "--- vol_shots: 3 screenshots in $OUT"
  ```

  Make executable: `chmod +x MatterEngine3/tools/vol_shots.sh`

- [ ] **Step 3: Run vol_shots and verify visual output**

  ```bash
  cd MatterViewer
  GALLIUM_DRIVER=d3d12 ../MatterEngine3/tools/vol_shots.sh /tmp/vol_shots_out
  ```

  Expected: three PNG files created, viewer self-terminates.

- [ ] **Step 4: Commit Task 11**

  ```bash
  git add MatterEngine3/examples/world_demo/schemas/ChimneySmoke.js \
           MatterEngine3/examples/world_demo/schemas/WaterfallMist.js \
           MatterEngine3/examples/world_demo/WorldData/Demo/world.manifest \
           MatterEngine3/tools/vol_shots.sh
  git commit -m "feat(volumetrics): Demo world chimney + waterfall mist emitters and height fog"
  ```

---

## Task 12: Perf gate and final sweep

**Files:**
- Modify: `MatterEngine3/src/matter_engine.cpp` — ensure `FrameStats` carries `gpu_vol_ms` (new field for volumetric pass GPU time)
- Modify: `MatterViewer/ui.cpp` — display `gpu_vol_ms` in the HUD GPU timing line
- Modify: `MatterEngine3/include/matter/world_session.h` — add `gpu_vol_ms = 0` to `FrameStats`
- (Manual Jack step) Perf gate: launch Demo world on Windows build, toggle volumetrics on/off, confirm HUD `gpu_vol_ms ≤ 1.5 ms`

**Interfaces:**
- Consumes: `VkVolumetrics::record` GPU timestamp zones (added in this task); all previous tasks
- Produces: confirmed perf gate; final full sequential test sweep passing

- [ ] **Step 1: Add `gpu_vol_ms` to `FrameStats` and HUD**

  In `MatterEngine3/include/matter/world_session.h`, in `FrameStats`, after `gpu_composite_ms`:
  ```cpp
  float gpu_vol_ms = 0;  // volumetric passes (density+scatter+integrate); 0 when disabled
  ```

  In `vk_volumetrics.cpp`, wrap the three compute dispatches in GPU timestamp zones (mirror the `write_gpu_timestamp(frame.command_buffer, kGpuZone*, ...)` pattern from `vk_scene_renderer.cpp`; add a `kGpuZoneVol` constant and supporting infrastructure).

  In `MatterViewer/ui.cpp`, in the GPU timing text line (line ~224), append `Vol %.1f` for `gpu_vol_ms`.

- [ ] **Step 2: Perf gate (manual Jack step)**

  Jack builds the Windows target after Task 11's Windows rebuild, launches with `MATTER_WORLD=demo`, enables volumetrics, and reads the HUD delta for `gpu_vol_ms`.

  Document the observed values in this commit:
  ```
  Perf gate result: gpu_vol_ms ≤ 1.5 ms on Demo world, RTX 4090 (documented in commit).
  ```

- [ ] **Step 3: Run the full sequential test sweep**

  ```bash
  make -C MatterEngine3/tests run-matrix
  make -C MatterEngine3/tests run-partv2
  make -C MatterEngine3/tests run-partstore
  make -C MatterEngine3/tests run-script
  make -C MatterEngine3/tests run-flatten
  make -C MatterEngine3/tests run-emittergather
  make -C MatterEngine3/tests run-lighting-garden
  make -C MatterEngine3/tests run-comp
  make -C MatterEngine3/tests run-gpucull
  GALLIUM_DRIVER=d3d12 make -C MatterEngine3/tests run-volgpu
  ```

  Expected: `ALL PASS` for every suite. Run sequentially (never in parallel).

- [ ] **Step 4: Enable volumetrics default OFF confirmation**

  Verify `VulkanVolumetricsSettings::enabled == false` at default construction (compile-time check via static_assert or test assertion already added in Task 4).

- [ ] **Step 5: Final commit**

  ```bash
  git add MatterEngine3/include/matter/world_session.h \
           MatterEngine3/src/render/vk_volumetrics.h \
           MatterEngine3/src/render/vk_volumetrics.cpp \
           MatterViewer/ui.cpp
  git commit -m "feat(volumetrics): gpu_vol_ms HUD timing; perf gate ≤1.5 ms confirmed; all suites pass"
  ```

---

## Final Acceptance Gate

- [ ] All 12 tasks committed to `feature/froxel-volumetrics`.
- [ ] Full sequential headless suite sweep passes (Task 12 Step 3).
- [ ] GALLIUM_DRIVER=d3d12 GPU suite (`run-volgpu`) passes.
- [ ] Vol shots script produces three non-empty PNG screenshots (Task 11 Step 3).
- [ ] Perf gate ≤ 1.5 ms documented (Task 12 Step 2).
- [ ] Volumetrics disabled by default (`VulkanVolumetricsSettings::enabled = false`); existing scenes render bit-identically with the feature off.
- [ ] Request code review via superpowers:requesting-code-review before merging to main.
