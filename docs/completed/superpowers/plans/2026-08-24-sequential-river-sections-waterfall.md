# Sequential River Sections and Waterfall Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and publish one static, playable river made from two independently cached 100+ metre PhysX sections, with a 12 metre waterfall before the first filled pool and a seamless broad-spillway handoff into the second section.

**Architecture:** The JS DSL supplies completed three-dimensional curves, channel profiles, section markers, and explicitly placed obstacle roots; native river code validates/resamples those inputs, carves one continuous terrain revision, and runs section-local PhysX bakes in dependency order. Each accepted section remains immutable and independently cacheable; a derived handoff artifact masks the temporary dam, meshes a spillway collar, and assembles visual/query/gameplay products into the existing single static-water publication seam.

**Tech Stack:** C++17, QuickJS world-definition DSL, MSVC 19.44/v143, CMake/Ninja, NVIDIA PhysX 5.6.1 GPU PBD, CUDA 12.8, Vulkan GPU visual meshing, MatterSurfaceLib CPU meshing, Box3D-authored frozen root transforms, PowerShell/CTest, and Python FIFO screenshot automation.

**Spec:** `docs/superpowers/specs/2026-08-24-sequential-river-sections-waterfall-design.md`

**Execution status:** Complete (2026-08-24). The implementation is recorded in
commit `3eead18e`; the accepted cold-cache evidence is under
`MatterEditor/build/baselines/msvc/physx-river-sections/20260824-section-waterfall-02`.
The task checklists below are retained as the implementation record.

## Global Constraints

- Water is baked during level construction and remains static during gameplay; players and runtime terrain do not modify it.
- PhysX owns all fluid mathematics. Matter code may marshal particles, collision geometry, emitters, sensors, and products but may not implement a fluid solver.
- PhysX runs in the editor process and remains opt-in through `MATTER_ENABLE_PHYSX`; ordinary Linux/WSL-native, authoring, and CPU-meshing builds remain PhysX/CUDA-free.
- Matter's GPU visual mesher and CPU query mesher remain the only water isosurface extractors.
- The DSL supplies authoritative 3D river curves, channel profiles, waterfalls, pools, spillways, and obstacle placement. Native code may validate and resample but may not add meander or scatter boulders.
- Curve feature positions are physical arc distances in metres, not normalized parameters.
- Build one continuous terrain revision before extracting section-local collision. Dry collars cull/validate work and are never collision walls.
- The first accepted world contains two independently baked sections with at least 100 metres per lake-to-lake reach, a 12 metre waterfall immediately before the first pool, an 8–12 metre natural spillway, and a second filled pool.
- The temporary dam is bake-only and must not appear in rendered or runtime collision products.
- A single escaped particle never aborts a bake. The strict-key escape policy defaults to `absoluteCount: 32` and `ratio: 0.0001`; a bake fails only above `max(absoluteCount, ceil(emitted * ratio))`.
- A ready network publishes atomically only after every required section and handoff artifact validates. Failed downstream work leaves accepted upstream artifacts reusable and inspectable.
- Stage only files owned by the active task before each commit; never include unrelated pre-existing worktree changes.

## File Responsibility Map

- `MatterEngine3/shared-lib/river_curve.js`: deterministic JS-side cubic/line curve sampling and physical-distance markers.
- `MatterEngine3/include/matter/river_network.h`: canonical authored curves, channel profiles, sections, waterfalls, pools, and spillways.
- `MatterEngine3/include/matter/hydrology.h`: emitter-shape, temporary-dam, escape-policy, and network-status authoring contracts.
- `MatterEngine3/src/hydrology/river_network_builder.{h,cpp}`: transactional native builder and canonical network hash.
- `MatterEngine3/src/script/world_definition_loader.cpp`: QuickJS builder objects for rivers and sections plus root fluid-collider parsing.
- `MatterEngine3/src/hydrology/river_geometry.{h,cpp}`: validation and arc-length resampling of DSL-supplied curves/profiles only.
- `MatterEngine3/src/terrain_river_overlay.{h,cpp}`: rounded-V/pool/waterfall terrain carve using authoritative curve elevation.
- `MatterEngine3/src/hydrology/authored_fluid_colliders.{h,cpp}`: convert tagged DSL root primitives and transforms into section collision surfaces.
- `MatterEngine3/src/hydrology/river_section_graph.{h,cpp}`: section lookup, dependency validation, and stable topological order.
- `MatterEngine3/src/hydrology/fluid_emitter_layout.{h,cpp}`: deterministic disc/ribbon activation offsets shared by tests and the PhysX adapter.
- `MatterEngine3/src/hydrology/spillway_handoff.{h,cpp}`: resolve accepted discharge, spillway frame, velocity, ownership planes, and downstream ribbon emitter.
- `MatterEngine3/src/hydrology/authored_fluid_request.h` and `MatterEngine3/src/provider/local_provider.cpp`: assemble one section request from the shared terrain, authored colliders, and accepted upstream handoff records.
- `MatterEngine3/src/hydrology/hydrology_artifact.{h,cpp}`: immutable section artifact schema and serialization.
- `MatterEngine3/src/hydrology/hydrology_network_artifact.{h,cpp}`: handoff references, network manifest, validation, and atomic persistence.
- `MatterEngine3/src/hydrology/river_section_coordinator.{h,cpp}`: serial dependency execution with an injected per-section executor.
- `MatterEngine3/src/hydrology/hydrology_handoff_products.{h,cpp}`: dam masking, GPU collar meshing, CPU ownership cuts, gameplay blending, and aggregate products.
- `MatterEngine3/src/matter_engine.cpp`: cancellation-safe atomic publication of one assembled network render binding.
- `projects/world_demo/scenes/RiverHydrology/RiverHydrology.js`: authoritative two-section waterfall test world and DSL-authored boulders.
- `MatterEngine3/tools/river_hydrology_sections.timeline`: matched camera set for overview, waterfall, spillway, rapids, and pools.
- `tools/run-river-hydrology-acceptance.ps1`: reproducible opt-in build, bake, trace, screenshot, and evidence summary.

---

### Task 1: Move River Curve and Channel Shape Ownership into the DSL

**Files:**
- Create: `MatterEngine3/shared-lib/river_curve.js`
- Create: `MatterEngine3/tests/shared-lib-fixtures/river_curve.probe.js`
- Modify: `MatterEngine3/include/matter/river_network.h`
- Modify: `MatterEngine3/src/hydrology/river_network_builder.h`
- Modify: `MatterEngine3/src/hydrology/river_network_builder.cpp`
- Modify: `MatterEngine3/src/script/world_definition_loader.cpp`
- Modify: `MatterEngine3/src/hydrology/river_geometry.h`
- Modify: `MatterEngine3/src/hydrology/river_geometry.cpp`
- Modify: `MatterEngine3/src/terrain_river_overlay.h`
- Modify: `MatterEngine3/src/terrain_river_overlay.cpp`
- Modify: `MatterEngine3/tests/shared_lib_tests.cpp`
- Modify: `MatterEngine3/tests/world_definition_tests.cpp`
- Modify: `MatterEngine3/tests/river_network_tests.cpp`
- Modify: `MatterEngine3/tests/river_geometry_tests.cpp`
- Modify: `MatterEngine3/tests/terrain_field_tests.cpp`
- Modify: `MatterEngine3/tests/dsl_determinism_tests.cpp`
- Modify: `cmake/MatterEngine.cmake`

**Interfaces:**
- Consumes: JS-authored line/cubic controls and channel-profile values.
- Produces: `riverCurve(...)`, `sampleRiverCurve(curve,distance)`, `RiverDefinition::curve`, `RiverDefinition::channel_profile`, `build_river_geometry(const RiverNetworkDefinition&, const std::string&, RiverGeometry&, std::string&)`, and `RiverCentrelineSample::{position_m,tangent,lateral,distance_m,width_m,depth_m,asymmetry}`.

- [ ] **Step 1: Add failing JS and native ownership tests**

Add a shared-lib probe that proves curve sampling and distance markers are deterministic:

```js
import { riverCurve } from '../../shared-lib/river_curve.js';

const builder = riverCurve([0, 10, 0], { maxSegmentLength: 0.5 });
builder.cubicTo([2, 9, 1], [4, 8, -1], [6, 7, 0]);
const lip = builder.distance();
builder.lineTo([8, -5, 0]);
const curve = builder.build();
globalThis.__probe = JSON.stringify({ lip, curve });
```

Add C++ assertions that `.curve(points)` and `.channelProfile(points)` survive world loading unchanged and that native geometry no longer changes X/Z or derives Y from `baseGrade`:

```cpp
const std::vector<matter::Float3> expected_curve = {
    {0.0f, 18.0f, 0.0f}, {30.0f, 12.0f, 8.0f}, {65.0f, 4.0f, -3.0f}};
const auto& loaded_curve = definition.river_network->rivers[0].curve;
CHECK(loaded_curve.size() == expected_curve.size() &&
          loaded_curve[1].x == expected_curve[1].x &&
          loaded_curve[1].y == expected_curve[1].y &&
          loaded_curve[1].z == expected_curve[1].z,
      "the completed DSL curve is the native source of truth");
CHECK(geometry.centreline.front().position_m.y == expected_curve.front().y &&
          geometry.centreline.back().position_m.y == expected_curve.back().y,
      "native resampling preserves authored elevation endpoints");
CHECK(definition.river_network->canonical_text.find("boulders=") ==
          std::string::npos,
      "native boulder generation is absent from the canonical contract");
```

- [ ] **Step 2: Run the focused tests and verify RED**

Run:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target shared_lib_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target river_network_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target world_definition_tests
```

Expected: compilation or test failure because `river_curve.js`, `.curve`, `.channelProfile`, and the new native fields do not exist.

- [ ] **Step 3: Implement deterministic JS curve sampling**

`river_curve.js` must use a fixed subdivision count derived from control-polygon length, never `Math.random` or adaptive recursion:

```js
export function riverCurve(start, { maxSegmentLength = 0.5 } = {}) {
  const points = [finitePoint(start)];
  let distanceM = 0;
  const append = (point) => {
    const p = finitePoint(point);
    const q = points[points.length - 1];
    distanceM += Math.hypot(p[0] - q[0], p[1] - q[1], p[2] - q[2]);
    points.push(p);
  };
  return {
    lineTo(end) { append(end); return this; },
    cubicTo(c1, c2, end) {
      const p0 = points[points.length - 1];
      const p1 = finitePoint(c1);
      const p2 = finitePoint(c2);
      const p3 = finitePoint(end);
      const polygon = segmentLength(p0, p1) + segmentLength(p1, p2) +
                      segmentLength(p2, p3);
      const steps = Math.max(1, Math.ceil(polygon / maxSegmentLength));
      for (let i = 1; i <= steps; ++i) append(cubicPoint(p0, p1, p2, p3, i / steps));
      return this;
    },
    distance() { return distanceM; },
    build() { return points.map((p) => p.slice()); },
  };
}

export function sampleRiverCurve(curve, distance) {
  if (!Array.isArray(curve) || curve.length < 2 ||
      !Number.isFinite(distance) || distance < 0) throw new TypeError('invalid curve sample');
  let travelled = 0;
  for (let i = 1; i < curve.length; ++i) {
    const a = finitePoint(curve[i - 1]);
    const b = finitePoint(curve[i]);
    const length = segmentLength(a, b);
    if (distance <= travelled + length || i + 1 === curve.length) {
      const t = Math.min(1, Math.max(0, (distance - travelled) / length));
      const tangent = [(b[0] - a[0]) / length, (b[1] - a[1]) / length,
                       (b[2] - a[2]) / length];
      const horizontal = Math.hypot(tangent[0], tangent[2]);
      return {
        position: [a[0] + (b[0] - a[0]) * t,
                   a[1] + (b[1] - a[1]) * t,
                   a[2] + (b[2] - a[2]) * t],
        tangent,
        lateral: [-tangent[2] / horizontal, 0, tangent[0] / horizontal],
        distance: Math.min(distance, travelled + length),
      };
    }
    travelled += length;
  }
  throw new RangeError('curve has no sampleable segment');
}
```

Implement `finitePoint`, `segmentLength`, and `cubicPoint` in the same module with finite-number validation and defensive copies. Reject zero-length or horizontally degenerate segments so `sampleRiverCurve` never divides by zero.

- [ ] **Step 4: Replace native spline/reach generation with curve/profile validation and resampling**

Define the canonical authored types:

```cpp
struct RiverChannelProfilePoint {
    float distance_m = 0.0f;
    float width_m = 0.0f;
    float depth_m = 0.0f;
    float asymmetry = 0.0f;
};

struct RiverDefinition {
    std::string name;
    RiverInlet inlet{};
    std::vector<Float3> curve;
    std::vector<RiverChannelProfilePoint> channel_profile;
};
```

Expose `.curve(array)` and `.channelProfile([{at,width,depth,asymmetry}])`. Remove `RiverReach`, `RiverChannel`, `RiverBoulders`, `displace_dense`, `meander_response`, `value_noise`, and `select_boulders`. Native geometry linearly interpolates the supplied profile by arc distance and rejects non-finite points, duplicate/descending profile distances, nonpositive dimensions, non-neighbour self-intersection, and excessive curvature.

`RiverHeightOverlay` consumes per-sample width/depth/asymmetry and uses `sample.position_m.y` as the thalweg. It may smooth lateral carve transitions, but it must not integrate grade or smooth along the curve across authored elevation changes.

Register the previously Make-only `shared_lib_tests`, `river_network_tests`, `river_geometry_tests`, and `dsl_determinism_tests` with `matter_add_engine_cpu_test` so every command below names a real MSVC target and carries the `cpu` label.

- [ ] **Step 5: Pass the complete curve/profile test set**

Run:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target shared_lib_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target world_definition_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target river_network_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target river_geometry_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target terrain_field_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target dsl_determinism_tests
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R 'shared_lib_tests|world_definition_tests|river_network_tests|river_geometry_tests|terrain_field_tests|dsl_determinism_tests' --output-on-failure
```

Expected: all selected tests PASS; repeated loads produce identical canonical text/hash and centreline revision.

- [ ] **Step 6: Commit the ownership migration**

```powershell
git add MatterEngine3/shared-lib/river_curve.js MatterEngine3/tests/shared-lib-fixtures/river_curve.probe.js MatterEngine3/include/matter/river_network.h MatterEngine3/src/hydrology/river_network_builder.h MatterEngine3/src/hydrology/river_network_builder.cpp MatterEngine3/src/script/world_definition_loader.cpp MatterEngine3/src/hydrology/river_geometry.h MatterEngine3/src/hydrology/river_geometry.cpp MatterEngine3/src/terrain_river_overlay.h MatterEngine3/src/terrain_river_overlay.cpp MatterEngine3/tests/shared_lib_tests.cpp MatterEngine3/tests/world_definition_tests.cpp MatterEngine3/tests/river_network_tests.cpp MatterEngine3/tests/river_geometry_tests.cpp MatterEngine3/tests/terrain_field_tests.cpp MatterEngine3/tests/dsl_determinism_tests.cpp cmake/MatterEngine.cmake
git commit -m "refactor: make river curves DSL authoritative"
```

### Task 2: Feed DSL-Authored Boulder Roots into Fluid Collision

**Files:**
- Create: `MatterEngine3/src/hydrology/authored_fluid_colliders.h`
- Create: `MatterEngine3/src/hydrology/authored_fluid_colliders.cpp`
- Create: `MatterEngine3/tests/authored_fluid_colliders_tests.cpp`
- Modify: `MatterEngine3/include/matter/world_definition.h`
- Modify: `MatterEngine3/src/script/world_definition_loader.cpp`
- Modify: `MatterEngine3/src/provider/local_provider.h`
- Modify: `MatterEngine3/src/provider/local_provider.cpp`
- Modify: `MatterEngine3/tests/world_definition_tests.cpp`
- Modify: `cmake/manifests/engine-core.sources`
- Modify: `cmake/MatterEngine.cmake`
- Modify: `MatterEngine3/tests/Makefile`

**Interfaces:**
- Consumes: `WorldRoot::{id,transform,fluid_collider}` parsed from ordinary DSL roots.
- Produces: `std::vector<hydrology::AuthoredFluidCollider>` and `build_authored_fluid_collision_surfaces(...)` for section request assembly.

- [ ] **Step 1: Write failing root parsing and collision conversion tests**

Use an ordinary rendered root with an explicit primitive collider:

```js
static roots = [{
  id: "upper-midstream-rock",
  module: "Rock",
  params: { seed: 41, size: 3.2, detail: 1.0 },
  transform: [1,0,0,52, 0,1,0,24, 0,0,1,-3, 0,0,0,1],
  fluidCollider: { shape: "sphere", radius: 2.4 },
}];
```

Assert one root produces one stable collider, identical world transform, finite triangles, and no duplicate rendered root:

```cpp
CHECK(definition.roots[0].id == "upper-midstream-rock",
      "fluid collider roots require stable DSL ids");
CHECK(definition.roots[0].fluid_collider.shape ==
          matter::WorldFluidColliderShape::Sphere,
      "root collider shape is preserved");
CHECK(output.surfaces.size() == 1u &&
          output.surfaces[0].indices.size() % 3u == 0u,
      "authored sphere becomes indexed fluid collision");
```

- [ ] **Step 2: Run and verify RED**

Run:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target world_definition_tests
```

Expected: compilation fails because `WorldRoot` has no ID or fluid-collider contract.

- [ ] **Step 3: Add root collider types and strict loader validation**

Add:

```cpp
enum class WorldFluidColliderShape : std::uint8_t { None, Sphere, Box };

struct WorldFluidCollider {
    WorldFluidColliderShape shape = WorldFluidColliderShape::None;
    float radius_m = 0.0f;
    Float3 half_extents_m{};
};
```

Extend `WorldRoot` with `std::string id` and `WorldFluidCollider fluid_collider`. Require a non-empty unique root ID whenever `fluidCollider` is present. Parse exactly `{shape:"sphere",radius}` or `{shape:"box",halfExtents:[x,y,z]}`; reject unknown shapes, nonpositive values, shear/non-finite transforms, and collider-bearing roots without IDs.

- [ ] **Step 4: Convert tagged roots to section collision surfaces**

Define:

```cpp
struct AuthoredFluidCollider {
    std::string id;
    matter::Mat4f object_to_world{};
    matter::WorldFluidCollider shape{};
    std::uint64_t revision = 0;
};

bool build_authored_fluid_collision_surfaces(
    const std::vector<AuthoredFluidCollider>& colliders,
    const matter::Aabb& selection_bounds_m,
    std::vector<FluidCollisionSurface>& surfaces,
    std::uint64_t& revision,
    FluidBakeError& error);
```

Tessellate spheres and boxes deterministically in object space, transform vertices once, reject singular transforms, select only world bounds intersecting the section collar, and hash stable ID, shape, dimensions, and canonical transform. `ProviderWorldDefinition` and `LocalProvider` retain the collider list beside roots; rendering still receives the original root exactly once.

- [ ] **Step 5: Register and pass the collider tests**

Run:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target authored_fluid_colliders_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target world_definition_tests
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R 'authored_fluid_colliders_tests|world_definition_tests' --output-on-failure
```

Expected: PASS, including enumeration-order-independent collider revision and section-bound filtering.

- [ ] **Step 6: Commit**

```powershell
git add MatterEngine3/src/hydrology/authored_fluid_colliders.h MatterEngine3/src/hydrology/authored_fluid_colliders.cpp MatterEngine3/tests/authored_fluid_colliders_tests.cpp MatterEngine3/include/matter/world_definition.h MatterEngine3/src/script/world_definition_loader.cpp MatterEngine3/src/provider/local_provider.h MatterEngine3/src/provider/local_provider.cpp MatterEngine3/tests/world_definition_tests.cpp cmake/manifests/engine-core.sources cmake/MatterEngine.cmake MatterEngine3/tests/Makefile
git commit -m "feat: use DSL roots as fluid colliders"
```

### Task 3: Add Builder-Style River Sections and a Stable Dependency Graph

**Files:**
- Create: `MatterEngine3/src/hydrology/river_section_graph.h`
- Create: `MatterEngine3/src/hydrology/river_section_graph.cpp`
- Create: `MatterEngine3/tests/river_section_graph_tests.cpp`
- Modify: `MatterEngine3/include/matter/river_network.h`
- Modify: `MatterEngine3/include/matter/hydrology.h`
- Modify: `MatterEngine3/src/hydrology/river_network_builder.h`
- Modify: `MatterEngine3/src/hydrology/river_network_builder.cpp`
- Modify: `MatterEngine3/src/script/world_definition_loader.cpp`
- Modify: `MatterEngine3/tests/world_definition_tests.cpp`
- Modify: `MatterEngine3/tests/river_network_tests.cpp`
- Modify: `MatterEngine3/tests/dsl_determinism_tests.cpp`
- Modify: `cmake/manifests/engine-core.sources`
- Modify: `cmake/MatterEngine.cmake`
- Modify: `MatterEngine3/tests/Makefile`

**Interfaces:**
- Consumes: named rivers, physical-distance section features, global PBD/quality/sensor templates, and authored emitter IDs.
- Produces: `RiverSectionDefinition`, `RiverSectionGraph`, and `build_river_section_graph(...)` with stable topological order.

- [ ] **Step 1: Add failing builder and graph tests**

The loader test must exercise the approved imperative shape:

```js
const upper = main.section("upper", { from: 0, to: 145, dryMargin: 15 })
  .emitters(["headwater"])
  .waterfall({ lipAt: 105, landingAt: 117, expectedDrop: 12 })
  .pool({ from: 117, to: 145, fillLevel: 24 })
  .spillway({ id: "pool-one", at: 145, width: 10,
              effectiveDepth: 2, overlap: 5, damOffset: 4 });
main.section("lower", { from: 145, to: 275, dryMargin: 15 })
  .after("upper")
  .fromSpillway("upper")
  .pool({ from: 255, to: 275, fillLevel: 3 })
  .spillway({ id: "pool-two", at: 275, width: 10,
              effectiveDepth: 2, overlap: 5, damOffset: 4 });
network.bakeSequential();
```

Add native tests for duplicate IDs, unknown dependencies, `after`/`fromSpillway` disagreement, cycles, out-of-range features, waterfall drop mismatch, missing terminal pool/spillway, and stable order `upper,lower`.

- [ ] **Step 2: Run and verify RED**

Run:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target river_section_graph_tests
```

Expected: target/configuration fails because the graph source and section types do not exist.

- [ ] **Step 3: Define canonical section types**

Add:

```cpp
struct RiverWaterfallDefinition {
    float lip_distance_m = 0.0f;
    float landing_distance_m = 0.0f;
    float expected_drop_m = 0.0f;
};

struct RiverPoolDefinition {
    float start_distance_m = 0.0f;
    float end_distance_m = 0.0f;
    float fill_level_m = 0.0f;
};

struct RiverSpillwayDefinition {
    std::string id;
    float distance_m = 0.0f;
    float width_m = 0.0f;
    float effective_depth_m = 0.0f;
    float overlap_m = 0.0f;
    float dam_offset_m = 0.0f;
};

struct RiverSectionDefinition {
    std::string id;
    std::string river;
    float from_m = 0.0f;
    float to_m = 0.0f;
    float dry_margin_m = 0.0f;
    std::vector<std::string> emitter_ids;
    std::vector<RiverWaterfallDefinition> waterfalls;
    std::optional<RiverPoolDefinition> terminal_pool;
    std::optional<RiverSpillwayDefinition> terminal_spillway;
    std::vector<std::string> after_section_ids;
    std::vector<std::string> upstream_spillway_section_ids;
};
```

Replace `RiverNetworkDefinition::first_section*` with `sections` and `bake_sequential`. Change `HydrologyVirtualDam` into shape-only settings `{height_m,thickness_m}` because section spillways own distance and downstream offset.

The matching DSL call is `network.virtualDam({height:8,thickness:0.5})`; the old global `distance` field is rejected. Repeated `.after(id)` and `.fromSpillway(id)` calls append unique dependency IDs so the canonical graph/manifest can represent fan-in even though this milestone executes one inherited handoff.

- [ ] **Step 4: Implement section QuickJS handles and graph validation**

Add a `MatterRiverSection` QuickJS class whose methods return `this` for chaining. Builder writes are transactional and single-assignment per feature. `build_river_section_graph` resolves stable indices by section ID, validates every physical interval against its river geometry length, verifies the curve elevation difference against `expected_drop_m` within `max(cell_size_m, 0.25f)`, and uses lexicographic ID as the tie-breaker in Kahn topological sorting.

Expose:

```cpp
struct RiverSectionGraph {
    std::vector<std::size_t> topological_order;
    std::vector<std::vector<std::size_t>> upstream;
};

bool build_river_section_graph(
    const matter::RiverNetworkDefinition& network,
    const std::vector<RiverGeometry>& geometry,
    RiverSectionGraph& graph,
    std::string& error);
```

- [ ] **Step 5: Pass DSL, graph, and canonical-hash tests**

Run:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target river_section_graph_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target river_network_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target world_definition_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target dsl_determinism_tests
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R 'river_section_graph_tests|river_network_tests|world_definition_tests|dsl_determinism_tests' --output-on-failure
```

Expected: PASS; reversing declaration order does not change graph order or canonical hash.

- [ ] **Step 6: Commit**

```powershell
git add MatterEngine3/src/hydrology/river_section_graph.h MatterEngine3/src/hydrology/river_section_graph.cpp MatterEngine3/tests/river_section_graph_tests.cpp MatterEngine3/include/matter/river_network.h MatterEngine3/include/matter/hydrology.h MatterEngine3/src/hydrology/river_network_builder.h MatterEngine3/src/hydrology/river_network_builder.cpp MatterEngine3/src/script/world_definition_loader.cpp MatterEngine3/tests/world_definition_tests.cpp MatterEngine3/tests/river_network_tests.cpp MatterEngine3/tests/dsl_determinism_tests.cpp cmake/manifests/engine-core.sources cmake/MatterEngine.cmake MatterEngine3/tests/Makefile
git commit -m "feat: author sequential river sections"
```

### Task 4: Add Spillway Records and Deterministic Ribbon Emitters

**Files:**
- Create: `MatterEngine3/src/hydrology/fluid_emitter_layout.h`
- Create: `MatterEngine3/src/hydrology/fluid_emitter_layout.cpp`
- Create: `MatterEngine3/src/hydrology/spillway_handoff.h`
- Create: `MatterEngine3/src/hydrology/spillway_handoff.cpp`
- Create: `MatterEngine3/tests/spillway_handoff_tests.cpp`
- Modify: `MatterEngine3/src/hydrology/physx_fluid_types.h`
- Modify: `MatterEngine3/src/hydrology/fluid_emission.cpp`
- Modify: `integrations/physx_adapter/physx_runtime.cpp`
- Modify: `MatterEngine3/tests/physx_adapter_contract_tests.cpp`
- Modify: `MatterEngine3/tests/physx_fluid_integration_tests.cpp`
- Modify: `cmake/manifests/engine-core.sources`
- Modify: `cmake/MatterEngine.cmake`
- Modify: `MatterEngine3/tests/Makefile`

**Interfaces:**
- Consumes: an accepted upstream section's authored discharge, terminal spillway, curve frame, particle spacing, and step budget.
- Produces: `SpillwayHandoffRecord`, `FluidEmitterShape::{Disc,Ribbon}`, `build_emitter_offsets(...)`, and `make_spillway_emitter(...)`.

- [ ] **Step 1: Write failing ribbon layout and conservation tests**

```cpp
hydrology::SpillwayHandoffRecord handoff{};
CHECK(hydrology::resolve_spillway_handoff(
          upper_section, lower_section, geometry, 600.0f, handoff, error),
      error.message.c_str());
CHECK(handoff.discharge_m3s == 600.0f,
      "handoff preserves authored discharge exactly");
CHECK(std::fabs(handoff.initial_speed_mps - 30.0f) < 1.0e-5f,
      "Q/(10m * 2m) produces a 30m/s mean ribbon speed");

std::vector<matter::Float2> offsets;
CHECK(hydrology::build_emitter_offsets(
          hydrology::FluidEmitterShape::Ribbon, 0.2f, 0.0f,
          {5.0f, 1.0f}, 10000u, offsets, error),
      error.message.c_str());
CHECK(!offsets.empty() && offsets.front().x <= offsets.back().x,
      "ribbon offsets have stable cross-stream ordering");
```

Also verify disc offsets remain byte-identical to the accepted implementation, ribbon width/depth validation, frame orthonormality, fractional emission carry, and capacity failure.

- [ ] **Step 2: Run and verify RED**

Run:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target spillway_handoff_tests
```

Expected: configuration or compilation fails because the handoff/layout files and emitter shape do not exist.

- [ ] **Step 3: Introduce shape-aware emitter contracts and shared layout code**

Add:

```cpp
enum class FluidEmitterShape : std::uint8_t { Disc, Ribbon };

struct FluidEmitter {
    std::uint32_t id = 0;
    FluidEmitterShape shape = FluidEmitterShape::Disc;
    matter::Float3 position_m{};
    matter::Float3 direction{};
    matter::Float3 lateral_axis{};
    matter::Float3 up_axis{};
    matter::Float3 initial_velocity_mps{};
    float flow_m3s = 0.0f;
    float radius_m = 0.0f;
    matter::Float2 half_extent_m{};
    std::uint32_t start_step = 0;
    std::uint32_t stop_step = 0;
};

bool build_emitter_offsets(
    FluidEmitterShape shape,
    float particle_spacing_m,
    float radius_m,
    matter::Float2 half_extent_m,
    std::uint32_t maximum_offsets,
    std::vector<matter::Float2>& offsets,
    FluidBakeError& error);
```

Move the existing disc offset loop from `physx_runtime.cpp` into `build_emitter_offsets`, returning local `Float2` coordinates where X is lateral and Y is up. Add a rectangular ribbon grid over `[-half_width,+half_width] x [-half_depth,+half_depth]`, ordered from center outward with deterministic row/column tie breaks. The adapter transforms each offset as `position + lateral_axis * offset.x + up_axis * offset.y` and retains its existing layer-backfill along `-direction`.

- [ ] **Step 4: Resolve accepted spillway metadata and downstream emitter**

Define:

```cpp
struct SpillwayHandoffRecord {
    std::string id;
    std::string upstream_section_id;
    std::string downstream_section_id;
    matter::Float3 lip_origin_m{};
    matter::Float3 tangent{};
    matter::Float3 lateral{};
    matter::Float3 up{};
    float discharge_m3s = 0.0f;
    float width_m = 0.0f;
    float effective_depth_m = 0.0f;
    float initial_speed_mps = 0.0f;
    float overlap_m = 0.0f;
    float upstream_visual_cut_m = 0.0f;
    float downstream_visual_cut_m = 0.0f;
    matter::Aabb temporary_dam_exclusion_bounds_m{};
    std::uint64_t semantic_key = 0;
};

bool resolve_spillway_handoff(
    const matter::RiverSectionDefinition& upstream,
    const matter::RiverSectionDefinition& downstream,
    const RiverGeometry& geometry,
    float accepted_discharge_m3s,
    SpillwayHandoffRecord& handoff,
    FluidBakeError& error);

bool make_spillway_emitter(
    const SpillwayHandoffRecord& handoff,
    const FluidPbdSettings& settings,
    FluidEmitter& emitter,
    FluidBakeError& error);
```

`resolve_spillway_handoff` samples the supplied curve at the terminal spillway, sets `initial_speed_mps = discharge/(width*effective_depth)`, assigns the two visual ownership cuts inside the overlap collar, and hashes every field. Section request assembly fills the exact temporary-dam exclusion bounds once terrain height and dam geometry are known and rekeys the record. `make_spillway_emitter` places a ribbon half a particle spacing below the lip, uses the curve tangent including Y, sets half extents `{width/2,effectiveDepth/2}`, and runs for the section's full bounded step window.

- [ ] **Step 5: Pass CPU and opt-in PhysX emitter tests**

Run:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target spillway_handoff_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target physx_adapter_contract_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target physx_fluid_integration_tests -EnablePhysx -PhysxRoot $env:MATTER_PHYSX_ROOT -CudaRoot $env:CUDA_PATH_V12_8
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R 'spillway_handoff_tests|physx_adapter_contract_tests|physx_fluid_integration_tests' --output-on-failure
```

Expected: PASS; the real adapter activates a broad ribbon without changing disc behavior.

- [ ] **Step 6: Commit**

```powershell
git add MatterEngine3/src/hydrology/fluid_emitter_layout.h MatterEngine3/src/hydrology/fluid_emitter_layout.cpp MatterEngine3/src/hydrology/spillway_handoff.h MatterEngine3/src/hydrology/spillway_handoff.cpp MatterEngine3/tests/spillway_handoff_tests.cpp MatterEngine3/src/hydrology/physx_fluid_types.h MatterEngine3/src/hydrology/fluid_emission.cpp integrations/physx_adapter/physx_runtime.cpp MatterEngine3/tests/physx_adapter_contract_tests.cpp MatterEngine3/tests/physx_fluid_integration_tests.cpp cmake/manifests/engine-core.sources cmake/MatterEngine.cmake MatterEngine3/tests/Makefile
git commit -m "feat: add natural spillway ribbon emission"
```

### Task 5: Assemble One Fluid Request per River Section

**Files:**
- Modify: `MatterEngine3/src/hydrology/authored_fluid_request.h`
- Modify: `MatterEngine3/src/provider/local_provider.cpp`
- Modify: `MatterEngine3/src/hydrology/physx_collision_input.h`
- Modify: `MatterEngine3/src/hydrology/physx_collision_input.cpp`
- Modify: `MatterEngine3/tests/physx_adapter_contract_tests.cpp`
- Modify: `MatterEngine3/tests/terrain_field_tests.cpp`

**Interfaces:**
- Consumes: network, one `RiverSectionDefinition`, its river geometry, shared terrain sampler/revision, authored root colliders, and zero or more accepted `SpillwayHandoffRecord` values.
- Produces: `assemble_authored_fluid_section_request(...) -> FluidBakeRequest` with section-local collision, emitters, sensor, ownership, semantic key, and cache path.

- [ ] **Step 1: Write failing section request tests**

Construct a two-section network over a fake terrain and assert:

```cpp
viewer::FluidBakeRequest upper_request{};
CHECK(viewer::assemble_authored_fluid_section_request(
          network, geometry, network.sections[0], {},
          colliders, context, cache_root, upper_request, error),
      error.message.c_str());
CHECK(upper_request.section_id == "upper" &&
          upper_request.input.emitters[0].shape ==
              hydrology::FluidEmitterShape::Disc,
      "upper section selects only its authored emitter");

viewer::FluidBakeRequest lower_request{};
CHECK(viewer::assemble_authored_fluid_section_request(
          network, geometry, network.sections[1], {handoff},
          colliders, context, cache_root, lower_request, error),
      error.message.c_str());
CHECK(lower_request.input.emitters.size() == 1u &&
          lower_request.input.emitters[0].shape ==
              hydrology::FluidEmitterShape::Ribbon,
      "downstream section receives only its inherited spillway emitter");
```

Verify the dam is at `spillway.distance + damOffset`, the sensor top matches `pool.fillLevel`, section two collision does not include section-one-only boulders, and cache paths contain sanitized section IDs plus semantic hashes.

- [ ] **Step 2: Run and verify RED**

Run:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target physx_adapter_contract_tests
```

Expected: compilation fails because only `assemble_authored_fluid_request` exists.

- [ ] **Step 3: Refactor request assembly around explicit section input**

Replace the first-section function with:

```cpp
bool assemble_authored_fluid_section_request(
    const matter::RiverNetworkDefinition& network,
    const hydrology::RiverGeometry& geometry,
    const matter::RiverSectionDefinition& section,
    const std::vector<hydrology::SpillwayHandoffRecord>& upstream,
    const std::vector<hydrology::AuthoredFluidCollider>& colliders,
    const FluidBakeRunContext& context,
    const std::string& cache_root,
    FluidBakeRequest& request,
    hydrology::FluidBakeError& error);
```

Add `section_id`, `river_id`, `from_m`, `to_m`, `upstream_handoff_keys`, and local ownership planes to `FluidBakeRequest`. Extract terrain triangles only for `[from-overlap,to+damOffset]` plus dry margin. Append intersecting authored collider surfaces, then the bake-only dam surface. Do not add bounds faces.

- [ ] **Step 4: Resolve sensor, emitters, and strict identity per section**

For the upper section, select only global authored emitters named by `section.emitter_ids`. For an inherited section, reject authored emitter IDs and create exactly one ribbon from its single accepted handoff. Keep the vector-shaped interface and manifest dependencies so tributary fan-in does not require a schema or coordinator rewrite, but reject more than one inherited handoff with the field-specific message `tributary fan-in execution is outside the two-section milestone`. Place the sensor's top at `terminal_pool.fill_level_m`, span the authored spillway width, and retain the existing spatial wet-fraction/stable-step rule.

Fold section definition, selected collision revision, sorted upstream handoff keys, dam shape/position, resolved sensor, escape policy, PBD settings, and terrain revision into the semantic key. Use `hydrology/sections/<safe-id>-<semantic-key>.mhyd`.

- [ ] **Step 5: Pass request/collision tests**

Run:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target physx_adapter_contract_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target terrain_field_tests
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R 'physx_adapter_contract_tests|terrain_field_tests' --output-on-failure
```

Expected: PASS; triangle/collider hashes are stable and no collision triangle lies on a dry-collar boundary plane.

- [ ] **Step 6: Commit**

```powershell
git add MatterEngine3/src/hydrology/authored_fluid_request.h MatterEngine3/src/provider/local_provider.cpp MatterEngine3/src/hydrology/physx_collision_input.h MatterEngine3/src/hydrology/physx_collision_input.cpp MatterEngine3/tests/physx_adapter_contract_tests.cpp MatterEngine3/tests/terrain_field_tests.cpp
git commit -m "refactor: assemble section-local fluid requests"
```

### Task 6: Version Section Artifacts, Escape Policy, and Network Manifests

**Files:**
- Create: `MatterEngine3/src/hydrology/hydrology_network_artifact.h`
- Create: `MatterEngine3/src/hydrology/hydrology_network_artifact.cpp`
- Create: `MatterEngine3/tests/hydrology_network_artifact_tests.cpp`
- Modify: `MatterEngine3/include/matter/hydrology.h`
- Modify: `MatterEngine3/src/hydrology/physx_fluid_types.h`
- Modify: `MatterEngine3/src/hydrology/physx_fluid_bake.cpp`
- Modify: `integrations/physx_adapter/physx_runtime.cpp`
- Modify: `MatterEngine3/src/hydrology/hydrology_artifact.h`
- Modify: `MatterEngine3/src/hydrology/hydrology_artifact.cpp`
- Modify: `MatterEngine3/src/hydrology/water_visual_products.h`
- Modify: `MatterEngine3/src/hydrology/water_visual_products.cpp`
- Modify: `MatterEngine3/src/hydrology/river_network_builder.h`
- Modify: `MatterEngine3/src/hydrology/river_network_builder.cpp`
- Modify: `MatterEngine3/src/script/world_definition_loader.cpp`
- Modify: `MatterEngine3/tests/hydrology_artifact_tests.cpp`
- Modify: `MatterEngine3/tests/physx_adapter_contract_tests.cpp`
- Modify: `MatterEngine3/tests/river_network_tests.cpp`
- Modify: `MatterEngine3/tests/world_definition_tests.cpp`
- Modify: `cmake/manifests/engine-core.sources`
- Modify: `cmake/MatterEngine.cmake`
- Modify: `MatterEngine3/tests/Makefile`

**Interfaces:**
- Consumes: accepted section output, strict section identity, escape policy, and cache-relative artifact names.
- Produces: versioned `HydrologyArtifact` section payloads and atomic `HydrologyNetworkArtifact` manifests.

- [ ] **Step 1: Write failing escape and serialization tests**

```cpp
const matter::HydrologyEscapePolicy policy{32u, 0.0001f};
CHECK(hydrology::fluid_escape_budget(1u, policy) == 32u,
      "one escaped particle is below the default budget");
CHECK(hydrology::fluid_escape_budget(4000000u, policy) == 400u,
      "large runs use the proportional budget");

hydrology::HydrologyArtifact section = fixture_artifact();
section.section = {"upper", "main", 0.0f, 145.0f, -5.0f, 150.0f};
std::vector<std::uint8_t> bytes;
gpu_meshing::Error artifact_error{};
CHECK(hydrology::serialize_artifact(section, bytes, artifact_error),
      artifact_error.message.c_str());
hydrology::HydrologyArtifact reopened{};
CHECK(hydrology::deserialize_artifact(bytes, reopened, artifact_error),
      artifact_error.message.c_str());
CHECK(reopened.section.section_id == "upper" &&
          reopened.section.river_id == "main" &&
          reopened.section.visual_from_m == -5.0f &&
          reopened.section.visual_to_m == 150.0f,
      "section identity and ownership survive serialization");
```

Add manifest tests for deterministic section/handoff ordering, duplicate references, missing dependencies, incomplete-not-ready rejection, corruption/truncation, atomic save/reopen, and relative-path enforcement.

- [ ] **Step 2: Run and verify RED**

Run:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target hydrology_artifact_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target hydrology_network_artifact_tests
```

Expected: compilation/configuration failure because escape policy, section identity, and the network artifact do not exist.

- [ ] **Step 3: Make escaped-particle retirement policy explicit and keyed**

Define:

```cpp
namespace matter {
struct HydrologyEscapePolicy {
    std::uint32_t absolute_count = 32u;
    float ratio = 0.0001f;
};
}

inline std::uint32_t fluid_escape_budget(
    std::uint32_t emitted,
    const matter::HydrologyEscapePolicy& policy) noexcept {
    const auto proportional = static_cast<std::uint64_t>(
        std::ceil(static_cast<double>(emitted) * policy.ratio));
    return static_cast<std::uint32_t>(std::max<std::uint64_t>(
        policy.absolute_count, proportional));
}
```

Add the policy to `HydrologyBakeLimits`, `FluidPbdSettings`, semantic hashing, and artifact validation. Expose `network.escapePolicy({absoluteCount:32,ratio:0.0001})` and canonicalize both values. Add `retired_particles` to `FluidBakeStats`; require `emitted = active + retired`, `escaped <= retired`, and fail only when `escaped > escape_budget`. Non-finite state remains an immediate failure.

- [ ] **Step 4: Version the section artifact**

Add:

```cpp
struct HydrologySectionIdentity {
    std::string section_id;
    std::string river_id;
    float from_m = 0.0f;
    float to_m = 0.0f;
    float visual_from_m = 0.0f;
    float visual_to_m = 0.0f;
};
```

Store it in `HydrologyArtifact`, bump the binary version, serialize strings with explicit byte limits, and reject invalid ownership intervals. Old single-section cache files fail closed and rebuild; no compatibility reader is required because these are generated caches.

- [ ] **Step 5: Implement the small network manifest**

Define:

```cpp
enum class HydrologyNetworkState : std::uint8_t { Incomplete, Failed, Ready };

struct HydrologyArtifactReference {
    std::string id;
    std::string relative_path;
    std::vector<std::string> dependencies;
    std::uint64_t semantic_key = 0;
    std::uint64_t payload_digest = 0;
};

struct HydrologyNetworkArtifact {
    HydrologyNetworkState state = HydrologyNetworkState::Incomplete;
    std::uint64_t network_key = 0;
    std::uint64_t terrain_revision = 0;
    std::vector<HydrologyArtifactReference> sections;
    std::vector<HydrologyArtifactReference> handoffs;
    std::vector<std::string> topological_order;
    matter::Aabb bounds_m{};
    std::uint64_t payload_digest = 0;
};
```

Provide `serialize_network_artifact`, `deserialize_network_artifact`, `save_network_artifact_atomic`, and `load_network_artifact_validated`. Ready validation requires every ordered section and handoff reference exactly once with nonzero keys/digests and cache-relative paths.

- [ ] **Step 6: Pass artifact and escape tests**

Run:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target hydrology_artifact_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target hydrology_network_artifact_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target physx_adapter_contract_tests
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R 'hydrology_artifact_tests|hydrology_network_artifact_tests|physx_adapter_contract_tests' --output-on-failure
```

Expected: PASS, including exactly 32 escapes accepted and 33 rejected for a small run.

- [ ] **Step 7: Commit**

```powershell
git add MatterEngine3/src/hydrology/hydrology_network_artifact.h MatterEngine3/src/hydrology/hydrology_network_artifact.cpp MatterEngine3/tests/hydrology_network_artifact_tests.cpp MatterEngine3/include/matter/hydrology.h MatterEngine3/src/hydrology/physx_fluid_types.h MatterEngine3/src/hydrology/physx_fluid_bake.cpp integrations/physx_adapter/physx_runtime.cpp MatterEngine3/src/hydrology/hydrology_artifact.h MatterEngine3/src/hydrology/hydrology_artifact.cpp MatterEngine3/src/hydrology/water_visual_products.h MatterEngine3/src/hydrology/water_visual_products.cpp MatterEngine3/src/hydrology/river_network_builder.h MatterEngine3/src/hydrology/river_network_builder.cpp MatterEngine3/src/script/world_definition_loader.cpp MatterEngine3/tests/hydrology_artifact_tests.cpp MatterEngine3/tests/physx_adapter_contract_tests.cpp MatterEngine3/tests/river_network_tests.cpp MatterEngine3/tests/world_definition_tests.cpp cmake/manifests/engine-core.sources cmake/MatterEngine.cmake MatterEngine3/tests/Makefile
git commit -m "feat: persist hydrology section networks"
```

### Task 7: Run Sections Serially with Reusable Upstream Results

**Files:**
- Create: `MatterEngine3/src/hydrology/river_section_coordinator.h`
- Create: `MatterEngine3/src/hydrology/river_section_coordinator.cpp`
- Create: `MatterEngine3/tests/river_section_coordinator_tests.cpp`
- Modify: `MatterEngine3/src/hydrology/hydrology_network_artifact.h`
- Modify: `cmake/manifests/engine-core.sources`
- Modify: `cmake/MatterEngine.cmake`
- Modify: `MatterEngine3/tests/Makefile`

**Interfaces:**
- Consumes: `RiverSectionGraph` and an injected `SectionBakeExecutor` that performs cache lookup or one section bake.
- Produces: ordered accepted section results, accepted spillway records, incomplete/failed state on rejection, and aggregate progress.

- [ ] **Step 1: Write failing fake-executor lifecycle tests**

```cpp
std::vector<std::string> calls;
hydrology::SectionBakeExecutor executor =
    [&](const matter::RiverSectionDefinition& section,
        const std::vector<hydrology::SpillwayHandoffRecord>& upstream,
        hydrology::SectionBakeResult& result,
        hydrology::FluidBakeError& error) {
      calls.push_back(section.id);
      if (section.id == "lower") CHECK(upstream.size() == 1u &&
                                           upstream[0].upstream_section_id == "upper",
                                       "lower receives accepted upper handoff");
      result = {};
      result.artifact.accepted = true;
      result.artifact.section.section_id = section.id;
      if (section.id == "upper") {
        hydrology::SpillwayHandoffRecord record{};
        record.id = "pool-one";
        record.upstream_section_id = "upper";
        record.downstream_section_id = "lower";
        record.semantic_key = 17u;
        result.downstream_handoff = record;
      }
      error = {};
      return true;
    };
CHECK(hydrology::run_river_section_sequence(
          network, graph, executor, {}, output, error),
      error.message.c_str());
CHECK(calls == std::vector<std::string>({"upper", "lower"}),
      "sections run serially in stable dependency order");
```

Add cases for upper cache hit, lower cache miss, lower failure preserving upper result, cancellation between sections, progress monotonicity, no executor re-entry, and manifest remaining non-ready before handoff products exist.

- [ ] **Step 2: Run and verify RED**

Run:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target river_section_coordinator_tests
```

Expected: target/configuration fails because the coordinator does not exist.

- [ ] **Step 3: Implement the pure serial coordinator**

Define:

```cpp
struct SectionBakeResult {
    HydrologyArtifact artifact;
    std::optional<SpillwayHandoffRecord> downstream_handoff;
    bool cache_hit = false;
};

using SectionBakeExecutor = std::function<bool(
    const matter::RiverSectionDefinition&,
    const std::vector<SpillwayHandoffRecord>&,
    SectionBakeResult&,
    FluidBakeError&)>;

struct RiverSectionSequenceResult {
    std::vector<SectionBakeResult> sections;
    HydrologyNetworkArtifact manifest;
};
```

`run_river_section_sequence` visits `graph.topological_order` once, passes the accepted direct-upstream handoff vector in stable section-ID order, appends validated results transactionally, and stops at the first failure. It never deletes accepted cache files. Aggregate progress is `(completed_sections + current_fraction) / total_sections`.

- [ ] **Step 4: Pass coordinator tests**

Run:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target river_section_coordinator_tests
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R river_section_coordinator_tests --output-on-failure
```

Expected: PASS; lower failure returns both the upper accepted result and a failed/incomplete manifest to the caller.

- [ ] **Step 5: Commit**

```powershell
git add MatterEngine3/src/hydrology/river_section_coordinator.h MatterEngine3/src/hydrology/river_section_coordinator.cpp MatterEngine3/tests/river_section_coordinator_tests.cpp MatterEngine3/src/hydrology/hydrology_network_artifact.h cmake/manifests/engine-core.sources cmake/MatterEngine.cmake MatterEngine3/tests/Makefile
git commit -m "feat: coordinate sequential fluid sections"
```

### Task 8: Author the Two-Section Ravine, Waterfall, Pools, and Boulders

**Files:**
- Modify: `projects/world_demo/scenes/RiverHydrology/RiverHydrology.js`
- Create: `projects/world_demo/tests/river_hydrology_scene_tests.mjs`
- Create: `MatterEngine3/tools/river_hydrology_sections.timeline`
- Modify: `MatterEngine3/tests/world_definition_tests.cpp`
- Modify: `MatterEngine3/tests/terrain_field_tests.cpp`

**Interfaces:**
- Consumes: Tasks 1–7 DSL contracts.
- Produces: the deterministic acceptance world with exact feature markers, tagged roots, and matched screenshot cameras.

- [ ] **Step 1: Write failing scene-contract tests**

Export pure scene-construction helpers from the module and use a dynamic Node import after installing the engine base-class stub:

```js
globalThis.World = class {};
const { buildRiverHydrologyDefinition } = await import(
  '../scenes/RiverHydrology/RiverHydrology.js');
const scene = buildRiverHydrologyDefinition(0x12345678);
assert.ok(scene.sections[0].length >= 100);
assert.ok(scene.sections[1].length >= 100);
assert.ok(Math.abs(scene.waterfall.drop - 12) < 0.25);
assert.ok(scene.spillway.width >= 8 && scene.spillway.width <= 12);
assert.ok(scene.roots.length >= 12);
assert.ok(scene.roots.every((root) => root.id && root.fluidCollider));
```

Add native terrain assertions that the waterfall lip-to-landing height difference is 12 ± 0.5 metres, both pool intervals remain within 0.5 metres of their authored floor elevation, and the ravine terrain—not section AABB faces—bounds cross-channel probes.

- [ ] **Step 2: Run and verify RED**

Run:

```powershell
node --experimental-default-type=module projects/world_demo/tests/river_hydrology_scene_tests.mjs
tools/build-windows.ps1 -Config RelWithDebInfo -Target terrain_field_tests
```

Expected: JS or terrain assertions fail because the scene still authors one native-meander section.

- [ ] **Step 3: Build the complete curve and physical markers in JS**

Import `riverCurve` and `sampleRiverCurve` from `shared-lib/river_curve`, then capture distances from the builder itself:

```js
const path = riverCurve([0, 72, 0], { maxSegmentLength: 0.5 });
path.cubicTo([32, 67, 18], [70, 61, -22], [106, 56, 8]);
const waterfallLip = path.distance();
path.lineTo([111, 44, 5]);
const waterfallLanding = path.distance();
path.cubicTo([121, 44, 2], [136, 44, -3], [148, 44, 0]);
const firstSpillway = path.distance();
path.cubicTo([184, 38, -24], [222, 31, 28], [258, 24, 4]);
path.cubicTo([270, 22, 1], [282, 22, -2], [294, 22, 0]);
const secondSpillway = path.distance();
const mainCurve = path.build();
```

Use profile points to vary channel width from 14 to 24 metres in the upper reach, widen the first pool to approximately 34 metres, set the natural spillway to 10 metres, vary the lower reach from 16 to 28 metres, and widen the second pool. Section distances come from captured curve distances and tests enforce the 100 metre minima.

- [ ] **Step 4: Place boulders entirely in DSL roots**

Replace `.boulders({density,...})` with a deterministic array of at least twelve explicit placement records:

```js
const boulderSpecs = [
  { id: "rock-01", at: 24, lateral: -3.0, size: 2.7, seed: 41 },
  { id: "rock-02", at: 47, lateral:  4.5, size: 4.0, seed: 42 },
  { id: "rock-03", at: 73, lateral: -5.0, size: 3.1, seed: 43 },
  { id: "rock-04", at: 92, lateral:  2.0, size: 3.5, seed: 44 },
  { id: "rock-05", at: waterfallLanding + 8, lateral: -4.0, size: 4.2, seed: 45 },
  { id: "rock-06", at: firstSpillway + 18, lateral:  3.0, size: 2.9, seed: 46 },
  { id: "rock-07", at: firstSpillway + 37, lateral: -5.5, size: 3.4, seed: 47 },
  { id: "rock-08", at: firstSpillway + 55, lateral:  6.0, size: 2.6, seed: 48 },
  { id: "rock-09", at: firstSpillway + 74, lateral: -2.0, size: 4.1, seed: 49 },
  { id: "rock-10", at: firstSpillway + 91, lateral:  4.0, size: 3.0, seed: 50 },
  { id: "rock-11", at: secondSpillway - 31, lateral: -6.0, size: 3.7, seed: 51 },
  { id: "rock-12", at: secondSpillway - 14, lateral:  2.5, size: 2.8, seed: 52 },
];

function boulderRoot(spec, curve) {
  const sample = sampleRiverCurve(curve, spec.at);
  const x = sample.position[0] + sample.lateral[0] * spec.lateral;
  const y = sample.position[1] + spec.size * 0.45;
  const z = sample.position[2] + sample.lateral[2] * spec.lateral;
  const yaw = Math.atan2(sample.tangent[2], sample.tangent[0]);
  const c = Math.cos(yaw) * spec.size;
  const s = Math.sin(yaw) * spec.size;
  return {
    id: spec.id,
    module: 'Rock',
    params: { seed: spec.seed, size: spec.size, detail: 1.0 },
    transform: [c,0,-s,x, 0,spec.size * 0.75,0,y,
                s,0,c,z, 0,0,0,1],
    fluidCollider: { shape: 'sphere', radius: spec.size * 0.72 },
  };
}
```

Map each record through `boulderRoot` to one `Rock` root with a frozen transform and matching sphere collider. Native river code receives no density or seed.

- [ ] **Step 5: Add the matched-camera timeline**

Create nine `cam`/`wait_idle`/`shot` groups named `overview`, `upper-rapids`, `waterfall-approach`, `waterfall-side`, `plunge-pool`, `spillway`, `lower-rapids`, `second-pool`, and `player-low`, followed by `quit`. Use absolute output placeholders accepted by the runner through token replacement, not hard-coded developer paths.

- [ ] **Step 6: Pass scene and dry-terrain tests**

Run:

```powershell
node --experimental-default-type=module projects/world_demo/tests/river_hydrology_scene_tests.mjs
tools/build-windows.ps1 -Config RelWithDebInfo -Target world_definition_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target terrain_field_tests
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R 'world_definition_tests|terrain_field_tests' --output-on-failure
```

Expected: PASS with the fluid backend disabled; the dry world remains buildable without PhysX.

- [ ] **Step 7: Commit**

```powershell
git add projects/world_demo/scenes/RiverHydrology/RiverHydrology.js projects/world_demo/tests/river_hydrology_scene_tests.mjs MatterEngine3/tools/river_hydrology_sections.timeline MatterEngine3/tests/world_definition_tests.cpp MatterEngine3/tests/terrain_field_tests.cpp
git commit -m "feat: author the sectional waterfall ravine"
```

### Task 9: Build the Spillway Seam and Aggregate Static Water Products

**Files:**
- Create: `MatterEngine3/src/hydrology/hydrology_handoff_products.h`
- Create: `MatterEngine3/src/hydrology/hydrology_handoff_products.cpp`
- Create: `MatterEngine3/tests/hydrology_handoff_products_tests.cpp`
- Modify: `MatterEngine3/src/hydrology/hydrology_network_artifact.h`
- Modify: `MatterEngine3/src/hydrology/hydrology_network_artifact.cpp`
- Modify: `MatterEngine3/src/hydrology/fluid_gameplay_field.h`
- Modify: `MatterEngine3/src/hydrology/fluid_gameplay_field.cpp`
- Modify: `MatterEngine3/tests/gpu_visual_mesher_cpu_tests.cpp`
- Modify: `MatterEngine3/tests/gpu_visual_mesher_vk_tests.cpp`
- Modify: `MatterEngine3/tests/hydrology_artifact_tests.cpp`
- Modify: `cmake/manifests/engine-core.sources`
- Modify: `cmake/MatterEngine.cmake`
- Modify: `MatterEngine3/tests/Makefile`

**Interfaces:**
- Consumes: two accepted section artifacts, one `SpillwayHandoffRecord`, and the existing GPU visual mesher callback.
- Produces: `HydrologyHandoffArtifact`, `HydrologyNetworkProducts::{visual_mesh,coarse_cpu_mesh,gameplay_layout,gameplay_field}`, and `validate_handoff_products(...)`.

- [ ] **Step 1: Write failing synthetic seam tests**

Create upstream particles that include a vertical dam-contact sheet, downstream ribbon particles, and simple meshes on both sides of `x=0`:

```cpp
hydrology::HydrologyHandoffArtifact handoff{};
hydrology::HydrologyNetworkProducts products{};
CHECK(hydrology::build_handoff_artifact(
          upstream, downstream, spillway, settings,
          synthetic_visual_mesher, handoff, products, error),
      error.message.c_str());
CHECK(hydrology::validate_handoff_products(
          handoff, products, spillway, error),
      error.message.c_str());
bool contains_dam_curtain = false;
for (std::size_t i = 0; i + 2u < products.visual_mesh.positions.size(); i += 3u) {
    const float x = products.visual_mesh.positions[i];
    const float y = products.visual_mesh.positions[i + 1u];
    if (x >= 4.75f && x <= 5.25f && y >= 0.5f) contains_dam_curtain = true;
}
CHECK(!contains_dam_curtain,
      "aggregate visual product excludes the temporary-dam curtain");
```

Add gameplay assertions for slow upstream pool velocity, increasing lip velocity, downstream ownership, dry-sample preservation, and deterministic aggregate digest.

- [ ] **Step 2: Run and verify RED**

Run:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target hydrology_handoff_products_tests
```

Expected: target/configuration fails because the handoff product builder does not exist.

- [ ] **Step 3: Implement deterministic ownership clipping**

Define:

```cpp
struct HydrologyHandoffArtifact {
    std::string id;
    SpillwayHandoffRecord handoff{};
    std::uint64_t semantic_key = 0;
    std::uint64_t upstream_payload_digest = 0;
    std::uint64_t downstream_payload_digest = 0;
    gpu_meshing::MeshResult visual_mesh;
    std::uint64_t payload_digest = 0;
};

struct HydrologyNetworkProducts {
    gpu_meshing::MeshResult visual_mesh;
    gpu_meshing::MeshResult coarse_cpu_mesh;
    GameplayFieldLayout gameplay_layout{};
    std::vector<GameplaySample> gameplay_field;
};

struct HandoffProductSettings {
    gpu_meshing::ParticleJob visual_job{};
    float particle_radius_m = 0.0f;
    GameplayFieldLayout gameplay_layout{};
};

bool build_handoff_artifact(
    const HydrologyArtifact& upstream,
    const HydrologyArtifact& downstream,
    const SpillwayHandoffRecord& handoff,
    const HandoffProductSettings& settings,
    const PhysxFluidBake::VisualMesher& visual_mesher,
    HydrologyHandoffArtifact& artifact,
    HydrologyNetworkProducts& products,
    FluidBakeError& error);

bool validate_handoff_products(
    const HydrologyHandoffArtifact& artifact,
    const HydrologyNetworkProducts& products,
    const SpillwayHandoffRecord& handoff,
    FluidBakeError& error);
```

Use signed distance `dot(world_position - lip_origin, tangent)`. Mask upstream triangles with centroid beyond the upstream cut before the dam, downstream triangles before the downstream cut, and gather particles from both artifacts inside `[-overlap,+overlap]`. Run the existing GPU mesher over that union, then clip the patch to the collar planes. Recompute normals/content digests after clipping. `validate_handoff_products` counts undirected visual boundary edges inside the spillway collar, canonicalizes CPU triangles to detect duplicate ownership, validates indices/normals/digests, and rejects any aggregate vertex inside the recorded temporary-dam exclusion slab.

- [ ] **Step 4: Assemble CPU and gameplay products without duplicate ownership**

CPU query output takes upstream triangles on the negative side and downstream triangles on the positive side; triangles intersecting the plane are clipped and retriangulated once. Build one aggregate X/Z gameplay layout over network bounds, resample section fields, and blend in the collar with:

```cpp
const float t = std::clamp(
    0.5f + signed_distance_m / (2.0f * handoff.overlap_m), 0.0f, 1.0f);
sample.velocity_x_mps = upstream.velocity_x_mps * (1.0f - t) +
                        downstream.velocity_x_mps * t;
sample.velocity_y_mps = upstream.velocity_y_mps * (1.0f - t) +
                        downstream.velocity_y_mps * t;
sample.velocity_z_mps = upstream.velocity_z_mps * (1.0f - t) +
                        downstream.velocity_z_mps * t;
```

Concatenate masked section visual meshes plus the patch into one world-space mesh for the existing render binding. Serialize the handoff artifact separately and reference it from the network manifest.

- [ ] **Step 5: Pass CPU and Vulkan seam tests**

Run:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target hydrology_handoff_products_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target gpu_visual_mesher_cpu_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R 'hydrology_handoff_products_tests|gpu_visual_mesher_cpu_tests|vulkan_smoke_tests' --output-on-failure
```

Expected: PASS; the synthetic dam sheet is absent and the patch digest is stable across repeated runs.

- [ ] **Step 6: Commit**

```powershell
git add MatterEngine3/src/hydrology/hydrology_handoff_products.h MatterEngine3/src/hydrology/hydrology_handoff_products.cpp MatterEngine3/tests/hydrology_handoff_products_tests.cpp MatterEngine3/src/hydrology/hydrology_network_artifact.h MatterEngine3/src/hydrology/hydrology_network_artifact.cpp MatterEngine3/src/hydrology/fluid_gameplay_field.h MatterEngine3/src/hydrology/fluid_gameplay_field.cpp MatterEngine3/tests/gpu_visual_mesher_cpu_tests.cpp MatterEngine3/tests/gpu_visual_mesher_vk_tests.cpp MatterEngine3/tests/hydrology_artifact_tests.cpp cmake/manifests/engine-core.sources cmake/MatterEngine.cmake MatterEngine3/tests/Makefile
git commit -m "feat: stitch static water section handoffs"
```

### Task 10: Integrate Sequential Baking and Atomic Network Publication

**Files:**
- Modify: `MatterEngine3/src/provider/local_provider.h`
- Modify: `MatterEngine3/src/provider/local_provider.cpp`
- Modify: `MatterEngine3/src/matter_engine.cpp`
- Modify: `MatterEngine3/include/matter/hydrology.h`
- Modify: `MatterEngine3/tests/async_bake_tests.cpp`
- Modify: `MatterEngine3/tests/physx_adapter_contract_tests.cpp`
- Modify: `MatterEngine3/tests/hydrology_artifact_tests.cpp`
- Modify: `cmake/MatterEngine.cmake`

**Interfaces:**
- Consumes: the graph, section request assembler, coordinator, artifact cache, handoff builder, renderer mesher callback, and cancellation token.
- Produces: `HydrologyNetworkBakeResult`, one committed ready manifest, one assembled renderer mesh, and retained debug water on failure.

- [ ] **Step 1: Write failing provider/publication tests**

Extend the fake backend so upper and lower return distinct accepted snapshots. Assert:

```cpp
hydrology::HydrologyNetworkBakeResult result{};
CHECK(provider.run_authored_fluid_bake(context, status, error, result),
      error.message.c_str());
CHECK(result.manifest.state == hydrology::HydrologyNetworkState::Ready &&
          result.sections.size() == 2u && result.handoffs.size() == 1u,
      "provider publishes only a complete two-section network");
CHECK(status.completed_sections == 2u && status.total_sections == 2u,
      "status reports section-level completion");
```

Add cases for upper cache hit/lower run, lower failure with upper cache retained, cancellation before lower creation, cancellation after assembly but before publication, stale token not replacing a prior ready binding, failed debug visual containing accepted upper plus failed lower particles, and one ready event for the whole network.

- [ ] **Step 2: Run and verify RED**

Run:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target physx_adapter_contract_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target async_bake_tests
```

Expected: compilation fails because provider still returns one `HydrologyArtifact`.

- [ ] **Step 3: Refactor LocalProvider around the section coordinator**

Define:

```cpp
struct HydrologyNetworkBakeResult {
    HydrologyNetworkArtifact manifest;
    std::vector<HydrologyArtifact> sections;
    std::vector<HydrologyHandoffArtifact> handoffs;
    HydrologyNetworkProducts products;
    gpu_meshing::MeshResult failed_debug_visual;
};
```

Build all river geometry once, then let the coordinator's executor perform section cache lookup, lazy PhysX backend creation, simulation, renderer-thread product building, atomic section save/reopen, and spillway resolution. Release each backend before GPU visual meshing, preserving the existing CUDA/Vulkan ownership rule. Build/save/reopen handoff artifacts after both adjacent sections exist, assemble products, write a Ready manifest last, and validate the full dependency chain before returning success.

- [ ] **Step 4: Preserve the existing single render binding and publish atomically**

Change `WorldSession::run_authored_fluid_bake_after_world_load` to build `AuthoredFluidRenderBinding` from `result.products.visual_mesh`. Store the complete network result in `LocalProvider`, then release-publish the binding and `accepted_fluid_artifact` flag under the existing generation mutex. On failure, combine accepted upstream visual meshes with the failed section's finite debug mesh into `failed_fluid_debug_binding`; do not set Ready or install CPU/gameplay products.

Extend `HydrologyStatus` with:

```cpp
std::uint32_t current_section = 0;
std::uint32_t completed_sections = 0;
std::uint32_t total_sections = 0;
std::string current_section_id;
```

Aggregate progress monotonically across sections and emit one final hydrology completion event only after the ready manifest commits.

Register the existing `async_bake_tests.cpp` as the MSVC `async_bake_tests` CPU target if it is still absent from `cmake/MatterEngine.cmake`; link it to `matter_engine_headless` through `matter_add_engine_cpu_test` rather than duplicating engine sources.

- [ ] **Step 5: Pass provider, async, cache, and publication tests**

Run:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target physx_adapter_contract_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target async_bake_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target hydrology_artifact_tests
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R 'physx_adapter_contract_tests|async_bake_tests|hydrology_artifact_tests' --output-on-failure
```

Expected: PASS; no stale generation publishes a partial network and a downstream-only edit reuses the upper section.

- [ ] **Step 6: Commit**

```powershell
git add MatterEngine3/src/provider/local_provider.h MatterEngine3/src/provider/local_provider.cpp MatterEngine3/src/matter_engine.cpp MatterEngine3/include/matter/hydrology.h MatterEngine3/tests/async_bake_tests.cpp MatterEngine3/tests/physx_adapter_contract_tests.cpp MatterEngine3/tests/hydrology_artifact_tests.cpp cmake/MatterEngine.cmake
git commit -m "feat: publish sequential river networks"
```

### Task 11: Prove the Real Two-Section PhysX Bake and Capture Evidence

**Files:**
- Create: `tools/run-river-hydrology-acceptance.ps1`
- Modify: `MatterEngine3/tests/physx_fluid_integration_tests.cpp`
- Modify: `projects/world_demo/scenes/RiverHydrology/RiverHydrology.js`
- Modify: `docs/superpowers/specs/2026-08-24-sequential-river-sections-waterfall-design.md`
- Modify: `docs/superpowers/plans/2026-08-24-sequential-river-sections-waterfall.md`
- Modify: `docs/README.md`

**Interfaces:**
- Consumes: the complete opt-in MSVC editor, external pinned PhysX checkout, CUDA 12.8, acceptance scene, camera timeline, and trace hooks.
- Produces: a validated Ready network, per-stage timing JSON, logs, particle/mesh diagnostics, and at least nine matched screenshots under one run directory.

- [ ] **Step 1: Add a failing guarded real-PhysX acceptance case**

The test runs only when PhysX is enabled and requires two sections. It asserts:

```cpp
CHECK(result.manifest.state == hydrology::HydrologyNetworkState::Ready,
      "real network reaches Ready");
CHECK(result.sections.size() == 2u && result.handoffs.size() == 1u,
      "real network accepts both sections and one spillway");
CHECK(result.sections[0].sensor.complete && result.sections[1].sensor.complete,
      "both terminal pools satisfy their fill sensors");
CHECK(result.sections[0].stats.non_finite_particles == 0u &&
          result.sections[1].stats.non_finite_particles == 0u,
      "both snapshots remain finite");
CHECK(result.sections[0].stats.escaped_particles <=
          result.sections[0].stats.escape_budget &&
      result.sections[1].stats.escaped_particles <=
          result.sections[1].stats.escape_budget,
      "both sections satisfy the authored escape policy");
```

Also assert nonempty visual/query/gameplay products, no handoff open boundary, and no vertex in the known temporary-dam exclusion slab.

- [ ] **Step 2: Run and verify RED against the complete world**

Run:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target physx_fluid_integration_tests -EnablePhysx -PhysxRoot $env:MATTER_PHYSX_ROOT -CudaRoot $env:CUDA_PATH_V12_8
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R physx_fluid_integration_tests --output-on-failure
```

Expected: the new two-section case fails until real runtime tuning and scene capacity are correct; retain its diagnostic output and do not weaken topology or finite-state assertions.

- [ ] **Step 3: Implement the reproducible acceptance runner**

`run-river-hydrology-acceptance.ps1` must:

```powershell
param(
    [Parameter(Mandatory=$true)][string]$PhysxRoot,
    [Parameter(Mandatory=$true)][string]$CudaRoot,
    [string]$RunId = (Get-Date -Format 'yyyyMMdd-HHmmss')
)
$outputRoot = Join-Path $PSScriptRoot "..\MatterEditor\build\baselines\msvc\physx-river-sections\$RunId"
```

It validates absolute dependency paths, builds `matter_editor` with `-EnablePhysx`, creates a fresh run directory, points `MATTER_HYDROLOGY_TRACE_DIR` at per-section trace subdirectories, expands timeline output tokens, launches `MatterEngine3/tools/drive.py --world RiverHydrology`, requires every PNG and `.done` sidecar, checks editor exit code, parses Ready/section/sensor counters, and writes `summary.json`. It may remove only the exact RiverHydrology cache directory after resolving and verifying that it is beneath `projects/world_demo/.cache`.

- [ ] **Step 4: Tune only authored/quality/capacity values until the real bake passes**

Retain the accepted starting quality (`particleSpacing:0.20`, `particleRadius:0.13`, `visualVoxel:0.15`, `visualBlendWidth:0.10`) unless measured capacity requires a documented change. Adjust section-specific maximum steps, max particles, emitter duration, pool sensor placement, and collider clearance from captured diagnostics. Do not lower river dimensions, remove the waterfall, add domain walls, loosen non-finite checks, or accept a visible dam curtain.

For every rejected run, retain `metadata.txt`, section particle bounds, sensor history, escaped/retired counts, failed debug mesh, timings, log, and the applicable screenshot subset.

- [ ] **Step 5: Capture and inspect the full evidence set**

Run:

```powershell
tools/run-river-hydrology-acceptance.ps1 -PhysxRoot $env:MATTER_PHYSX_ROOT -CudaRoot $env:CUDA_PATH_V12_8
```

Expected: exit 0 and nine nonempty PNGs covering overview, upper rapids, waterfall approach/side, plunge pool, spillway seam, lower rapids, second pool, and player-low view. Inspect each image for terrain containment, visible free fall/impact, adequate water volume, hidden emitter/dam, smooth surface, and no spillway gap.

- [ ] **Step 6: Record separated timing and final counts**

Require `summary.json` fields:

```json
{
  "networkState": "Ready",
  "sections": [
    {"id":"upper","setupMs":0,"physxInitMs":0,"simulateMs":0,"gpuMeshMs":0,"cpuMeshMs":0,"particles":0,"escaped":0},
    {"id":"lower","setupMs":0,"physxInitMs":0,"simulateMs":0,"gpuMeshMs":0,"cpuMeshMs":0,"particles":0,"escaped":0}
  ],
  "handoffMeshMs": 0,
  "serializeMs": 0,
  "totalWallMs": 0,
  "visualVertices": 0,
  "visualTriangles": 0
}
```

The runner replaces zeros with parsed measurements and fails if any required measurement or screenshot is absent. Record the accepted run ID and measured values in the design spec; do not commit build outputs.

- [ ] **Step 7: Run final CPU, GPU, packaging, and WSL entry-point gates**

Run:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -L cpu --output-on-failure
tools/build-windows.ps1 -Config RelWithDebInfo -Target physx_fluid_integration_tests -EnablePhysx -PhysxRoot $env:MATTER_PHYSX_ROOT -CudaRoot $env:CUDA_PATH_V12_8
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -L 'gpu|physx' --output-on-failure
tools/build-windows.ps1 -Config RelWithDebInfo -Target matter_dist -EnablePhysx -PhysxRoot $env:MATTER_PHYSX_ROOT -CudaRoot $env:CUDA_PATH_V12_8
```

From WSL:

```bash
./tools/build-windows-from-wsl.sh RelWithDebInfo matter_engine_headless
```

Expected: all commands exit 0; default CPU build remains PhysX-free, opt-in GPU tests pass, packaged editor contains required PhysX runtime files, and WSL can drive the MSVC build entry point.

- [ ] **Step 8: Update documentation and commit final evidence metadata**

Change the design status to implemented only after all gates pass. Add the design and plan to `docs/README.md`, record the accepted run directory (as an untracked build artifact), screenshots names, timings, counts, and any authored tuning changes.

```powershell
git add tools/run-river-hydrology-acceptance.ps1 MatterEngine3/tests/physx_fluid_integration_tests.cpp projects/world_demo/scenes/RiverHydrology/RiverHydrology.js docs/superpowers/specs/2026-08-24-sequential-river-sections-waterfall-design.md docs/superpowers/plans/2026-08-24-sequential-river-sections-waterfall.md docs/README.md
git commit -m "test: accept the sectional waterfall river"
```
