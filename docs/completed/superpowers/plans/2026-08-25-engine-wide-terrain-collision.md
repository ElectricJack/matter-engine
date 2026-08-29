# Engine-Wide Terrain Collision Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Generate deterministic Box3D static triangle-mesh collision from the final authored terrain field inside explicit world regions, then prove RiverFloatLab crates and rafts contact the carved ravine without enabling collision for the entire streamed world.

**Architecture:** The world DSL produces a provider-neutral collision definition. A worker canonicalizes its sector union, builds or loads immutable per-sector CPU mesh artifacts from `terrain_mesher::mesh_sector_tiled`, and hands one complete candidate to the app/physics-owner thread. `PhysicsContext` transactionally replaces its private Box3D terrain runtime before the world can enter Ready. Rendering and collision use the same `FieldRuntime` and river overlay but keep independent resolutions and lifetimes.

**Tech Stack:** C++17, QuickJS-ng, Matter terrain field/CPU mesher, Box3D, Flecs, CMake/Ninja with MSVC 2022, Node.js scene tests, PowerShell QA tooling.

**Spec:** `docs/superpowers/specs/2026-08-24-engine-wide-terrain-collision-design.md`

## Global Constraints

- Use test-driven development for every production change: add a focused failing test, run it and record the expected failure, implement the smallest behavior, then rerun the focused test.
- Build only with `tools/build-windows.ps1` and run tests from `MatterEditor/build/cmake/windows-msvc/relwithdebinfo`. Do not invoke Make, GCC, g++, MinGW, MSYS/UCRT, or `collect2`.
- Keep Box3D types private to `MatterEngine3/src/ecs/physics_context.cpp`. The public world definition, artifact, and worker candidate contain only engine-native data.
- Reuse `terrain_mesher::mesh_sector_tiled`. Do not add a height-field collider, hidden boxes, a second terrain mesher, GPU readback, or a general scene `MeshCollider` component.
- One world has one collision cell size. Region overlap canonicalizes to a sorted unique sector union.
- Preserve the existing `projects/world_demo/objects/Crate.js`; RiverFloatLab gets a scene-local part.
- A declared collision definition is required gameplay data. Build, validation, cache, or install failure prevents Ready.
- Keep unrelated worktree changes and untracked files untouched. Commit only files named by the active task.
- Update `cmake/manifests/engine-core.sources`, `cmake/MatterEngine.cmake`, and the rollback-only `MatterEngine3/tests/Makefile` registrations when adding translation units or tests, but never execute the rollback Make targets.
- Use `MATTER_LOGE/W/I/D` for engine diagnostics. Keep machine-readable QA output on stdout.

## Delivery Map and Stable Interfaces

| Task | Produces | Consumed by |
|---|---|---|
| 1. Definition and DSL | Validated `TerrainCollisionDefinition`, canonical sector union and split identity | Tile builder, session coordinator, RiverFloatLab |
| 2. CPU tile artifacts | Immutable `TerrainCollisionCandidate` plus cache/build statistics | Physics installer, session diagnostics |
| 3. Box3D runtime | Transactional `replace_terrain_collision` and private lifetime owner | WorldSession publication |
| 4. Session integration | Worker build, app-thread install, Ready/failure gating, public status | Editor/playtest and automation |
| 5. RiverFloatLab | One 0.5 m region and 1.5 m scene-local crates | End-to-end acceptance |
| 6. Acceptance evidence | Cold/cache timings, memory, screenshots, final MSVC gates | User review and roadmap handoff |

The following names and ownership boundaries are fixed for all tasks:

```cpp
// MatterEngine3/include/matter/terrain_collision.h
namespace matter {

struct TerrainCollisionRegion {
    std::string id;
    Float3 min_m{};  // inclusive
    Float3 max_m{};  // exclusive
};

struct TerrainCollisionDefinition {
    float cell_size_m = 0.0f;
    std::int8_t rung = 0;
    float friction = 0.7f;
    float restitution = 0.0f;
    std::vector<TerrainCollisionRegion> regions;
};

enum class TerrainCollisionState : std::uint8_t {
    Disabled,
    Building,
    CandidateReady,
    Installed,
    Failed,
};

struct TerrainCollisionStatus {
    TerrainCollisionState state = TerrainCollisionState::Disabled;
    std::uint64_t generation_key = 0;
    std::uint64_t geometry_key = 0;
    float cell_size_m = 0.0f;
    std::int8_t rung = 0;
    std::uint32_t region_count = 0;
    std::uint32_t sector_count = 0;
    std::uint32_t non_empty_tile_count = 0;
    std::uint32_t empty_tile_count = 0;
    std::uint64_t triangle_count = 0;
    std::uint64_t unique_vertex_count = 0;
    std::uint64_t artifact_bytes = 0;
    std::uint64_t box3d_retained_bytes = 0;
    double cold_build_ms = 0.0;
    double cache_load_ms = 0.0;
    double validation_ms = 0.0;
    double install_ms = 0.0;
    std::string failure_code;
    std::string failure_message;
};

}  // namespace matter
```

```cpp
// MatterEngine3/src/terrain_collision/terrain_collision_definition.h
namespace matter::terrain_collision {

struct SectorCoordinate {
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t z = 0;
};

struct SourceIdentity {
    std::uint64_t field_hash = 0;
    std::uint64_t overlay_hash = 0;
    std::uint32_t mesher_semantic_version = 0;
    std::uint32_t geometry_format_version = 1;
};

struct CanonicalDefinition {
    float sector_size_m = 0.0f;
    float cell_size_m = 0.0f;
    std::int8_t rung = 0;
    float friction = 0.7f;
    float restitution = 0.0f;
    std::vector<TerrainCollisionRegion> regions;
    std::vector<SectorCoordinate> sectors;
    std::uint64_t geometry_key = 0;
    std::uint64_t installation_key = 0;
};

bool cell_size_to_rung(float cell_size_m, std::int8_t& out_rung) noexcept;
bool canonicalize(const TerrainCollisionDefinition& definition,
                  float sector_size_m,
                  const SourceIdentity& source,
                  CanonicalDefinition& out,
                  std::string& error);

}  // namespace matter::terrain_collision
```

`SectorCoordinate` comparison and hashing are explicit helpers; do not rely on C++20 spaceship operators. Geometry identity hashes source identity, sector size, rung, and the sorted sector union. Installation identity hashes geometry identity plus friction and restitution. Region labels and decomposition do not enter either hash.
The externally reported generation key is the accepted installation identity;
`TerrainCollisionStatus::generation_key` therefore equals the candidate's
`installation_key`.

```cpp
// MatterEngine3/src/terrain_collision/terrain_collision_artifact.h
namespace matter::terrain_collision {

struct TileCandidate {
    SectorCoordinate coordinate{};
    Float3 origin_m{};
    std::vector<Float3> vertices;
    std::vector<std::uint32_t> indices;
    std::uint64_t tile_key = 0;
    std::uint64_t digest = 0;
};

struct CandidateStats {
    std::uint32_t cache_hit_tiles = 0;
    std::uint32_t built_tiles = 0;
    std::uint32_t empty_tiles = 0;
    std::uint64_t triangle_count = 0;
    std::uint64_t unique_vertex_count = 0;
    std::uint64_t artifact_bytes = 0;
    double cold_build_ms = 0.0;
    double cache_load_ms = 0.0;
    double validation_ms = 0.0;
};

struct TerrainCollisionCandidate {
    std::uint64_t geometry_key = 0;
    std::uint64_t installation_key = 0;
    float friction = 0.7f;
    float restitution = 0.0f;
    std::vector<TileCandidate> tiles;  // sorted; includes successful empty tiles
    CandidateStats stats{};
};

using CancelCheck = std::function<bool()>;

bool load_or_build_candidate(
    const terrain_field::FieldRuntime& field,
    const CanonicalDefinition& definition,
    const std::filesystem::path& cache_root,
    const CancelCheck& cancelled,
    TerrainCollisionCandidate& out,
    std::string& error);

}  // namespace matter::terrain_collision
```

The artifact layer stores empty tiles in the manifest but the physics layer skips them. It scans each material bucket's triangle soup in emitted order, welds only bit-identical local `Float3` values, and preserves the emitted winding.

---

## Task 1: Add the Provider-Neutral Definition, DSL Builder, and Canonical Identity

**Files:**

- Create: `MatterEngine3/include/matter/terrain_collision.h`
- Create: `MatterEngine3/src/terrain_collision/terrain_collision_definition.h`
- Create: `MatterEngine3/src/terrain_collision/terrain_collision_definition.cpp`
- Create: `MatterEngine3/tests/terrain_collision_definition_tests.cpp`
- Modify: `MatterEngine3/include/matter/world_definition.h`
- Modify: `MatterEngine3/src/script/world_definition_loader.cpp`
- Modify: `MatterEngine3/src/provider/local_provider.h`
- Modify: `MatterEngine3/src/provider/local_provider.cpp`
- Modify: `MatterEngine3/src/terrain_mesher.h`
- Modify: `MatterEngine3/tests/world_definition_tests.cpp`
- Modify: `cmake/manifests/engine-core.sources`
- Modify: `cmake/MatterEngine.cmake`
- Modify: `MatterEngine3/tests/Makefile`

### 1.1 Write failing native definition tests

- [ ] Add `terrain_collision_definition_tests.cpp` with independent test cases for the exact ladder mapping: `0.25 -> 3`, `0.5 -> 2`, `1 -> 1`, `2 -> 0`, `4 -> -1`, `8 -> -2`, `16 -> -3`, `32 -> -4`, `64 -> -5`; reject zero, negative, NaN, infinity, and nearby unsupported values.
- [ ] Add a valid 64 m sector-grid case with overlapping regions in different authoring orders. Assert both results contain the same lexicographically sorted coordinates and the same geometry/installation keys.
- [ ] Assert min-inclusive/max-exclusive enumeration. A region `[-64,-64,-64]` to `[64,64,64]` must produce exactly coordinates `x,y,z in {-1,0}`.
- [ ] Assert empty ids, duplicate ids, non-finite bounds, `min >= max`, non-positive/non-finite sector size, misalignment in every axis, bad friction/restitution, and an empty region list fail with a field-specific message.
- [ ] Assert changing only region labels or overlap decomposition preserves both identities; changing field hash, overlay hash, mesher version, sector size, rung, or sector union changes geometry identity; changing only friction/restitution preserves geometry identity and changes installation identity.
- [ ] Register `terrain_collision_definition_tests` with `matter_add_engine_cpu_test` and add its source closure to the rollback Makefile without executing it.
- [ ] Run the RED gate and confirm the missing header/target failure:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target terrain_collision_definition_tests
```

### 1.2 Write failing loader/DSL tests

- [ ] Extend `world_definition_tests.cpp` with a valid `World` class using the exact imperative call shape from the spec. Assert the loader retains cell size, rung, material values, ids, and bounds in `WorldDefinition::terrain_collision`.
- [ ] Add omission coverage proving a world without `collision()` still has `std::nullopt` and its existing definition data is unchanged.
- [ ] Add one focused script for each phase guard: module scope, `field()`, `hydrology()`, `biomes()`, and `buildEntities()` must reject `terrainCollision()` with the active phase in the message.
- [ ] Add rejection cases for two builders, two `build()` calls, no region, duplicate region id, region after build, hook return without build, unsupported cell size, bad material values, invalid vectors, and misaligned bounds.
- [ ] Add one test showing `collision()` receives normal world instance state, but a return value is ignored; only `build()` publishes the definition.
- [ ] Add an adapter test proving `viewer::adapt_world_definition` preserves the optional collision definition exactly and that omission remains empty.
- [ ] Run the existing loader target and confirm the new assertions fail before implementation:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target world_definition_tests
ctest --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R "^world_definition_tests$" --output-on-failure
```

### 1.3 Implement the public definition and deterministic canonicalizer

- [ ] Add the public types shown in the stable-interface section and include `terrain_collision.h` from `world_definition.h`.
- [ ] Add `std::optional<TerrainCollisionDefinition> terrain_collision;` beside hydrology and river-network bake inputs.
- [ ] Thread that optional through `viewer::ProviderWorldDefinition`, `adapt_world_definition`, `LocalProvider::load_authored_world`, and a read-only `LocalProvider::terrain_collision()` accessor. Reset it beside `hydrology_settings_` and `river_network_` on every authored-world load.
- [ ] Implement exact float-ladder matching without rounding. Use equality against the nine representable constants after requiring finiteness.
- [ ] Canonicalize region coordinates by first validating exact sector alignment in double precision, converting aligned quotients to checked `int64_t`, enumerating `[min,max)` coordinates with checked loop increments, sorting lexicographically X/Y/Z, and erasing duplicates.
- [ ] Reject coordinate count/multiplication overflow before allocation. Set an implementation limit only if it is named, documented in the error, and covered by a boundary test.
- [ ] Use an explicit byte-wise hash writer that normalizes integer endianness and hashes float bit patterns. Never hash struct padding or `std::hash` output.
- [ ] Add `inline constexpr std::uint32_t kSemanticVersion = 1;` to `terrain_mesher.h`; this is the source of `SourceIdentity::mesher_semantic_version` in later tasks.

### 1.4 Implement the dedicated `collision()` authoring phase

- [ ] Add one collision-builder state object to `LoadCollector`: active flag, builder-created flag, built flag, settings, ordered regions, and id set.
- [ ] Install the global `terrainCollision` function only through the loader's ordinary global-binding mechanism. Its entry point must check the collision phase before parsing arguments.
- [ ] Return a QuickJS builder object with `region(id, bounds)` and `build()` methods. Parse arrays as exactly three finite numbers and attach diagnostics to `terrainCollision.region[<id>].min/max`.
- [ ] After the existing static world settings are available, invoke optional instance `collision()` in its own phase. Use `World.settings.sectorSize` for alignment validation and never infer a sector size from region bounds.
- [ ] Validate and enumerate the authored union by calling the native canonicalizer with zero source hashes, then retain only the validated public definition. The session calls it again with real field/overlay identities when it builds collision.
- [ ] Publish the completed definition into the `WorldDefinition` produced by `load_world_definition`; restore phase state and JS globals through the same RAII/error path used by hydrology. `ScriptHost::eval_world` remains the separate field-program evaluation and does not gain a duplicate collision definition.
- [ ] Ensure builder state cannot leak into `buildEntities()` or a subsequent loader invocation.

### 1.5 Verify and commit Task 1

- [ ] Build both focused targets and run their exact CTest entries:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target terrain_collision_definition_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target world_definition_tests
ctest --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R "^(terrain_collision_definition_tests|world_definition_tests)$" --output-on-failure
```

- [ ] Inspect `git diff --check` and confirm public headers contain no Box3D, renderer, Vulkan, or Flecs include.
- [ ] Commit only Task 1 files:

```powershell
git add MatterEngine3/include/matter/terrain_collision.h MatterEngine3/include/matter/world_definition.h MatterEngine3/src/terrain_collision/terrain_collision_definition.h MatterEngine3/src/terrain_collision/terrain_collision_definition.cpp MatterEngine3/src/script/world_definition_loader.cpp MatterEngine3/src/provider/local_provider.h MatterEngine3/src/provider/local_provider.cpp MatterEngine3/src/terrain_mesher.h MatterEngine3/tests/terrain_collision_definition_tests.cpp MatterEngine3/tests/world_definition_tests.cpp cmake/manifests/engine-core.sources cmake/MatterEngine.cmake MatterEngine3/tests/Makefile
git commit -m "feat: author bounded terrain collision"
```

---

## Task 2: Build, Validate, and Cache Deterministic CPU Collision Tiles

**Files:**

- Create: `MatterEngine3/src/terrain_collision/terrain_collision_artifact.h`
- Create: `MatterEngine3/src/terrain_collision/terrain_collision_artifact.cpp`
- Create: `MatterEngine3/tests/terrain_collision_artifact_tests.cpp`
- Modify: `MatterEngine3/src/terrain_collision/terrain_collision_definition.h`
- Modify: `MatterEngine3/src/terrain_collision/terrain_collision_definition.cpp`
- Modify: `cmake/manifests/engine-core.sources`
- Modify: `cmake/MatterEngine.cmake`
- Modify: `MatterEngine3/tests/Makefile`

### 2.1 Pin the candidate conversion with failing tests

- [ ] Build reusable test fields for a plane, steep slope, rounded-V ravine, cave, cliff, and overhang. Construct `FieldRuntime` directly so tests do not depend on QuickJS.
- [ ] For each fixture, call the planned `load_or_build_candidate` into a temporary cache root and assert sorted coordinates, finite local vertices, in-range indices, index count divisible by three, nonzero triangle area, and byte-repeatable digests across two cold roots.
- [ ] Assert all material buckets are included by using a field whose classification yields multiple buckets and comparing the candidate's triangle count with the sum of mesher bucket triangles.
- [ ] Assert an all-air sector and an all-solid sector succeed as empty tiles and create no vertices/indices.
- [ ] Assert a field plus `RiverHeightOverlay` produces a different tile from the base field and that the tile surface matches direct probes of the final overlaid runtime.
- [ ] Assert two equal-rung neighboring tiles share bit-identical boundary vertices at X, Y, and Z planes and have no open ownership gap.
- [ ] Add validator-only negative fixtures for NaN/Inf vertices, index overflow, non-triangle index count, repeated-index and zero-area triangles, winding inconsistent with emitted terrain normals, wrong coordinate/origin, digest mismatch, and count arithmetic overflow.
- [ ] Run the RED build and record the missing artifact interface failure:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target terrain_collision_artifact_tests
```

### 2.2 Implement deterministic tile conversion

- [ ] For each canonical sector, call `terrain_mesher::mesh_sector_tiled(field, x, y, z, rung, sector_size_m, ...)` with no seam-boundary or overlap-band product requested.
- [ ] Flatten every `SectorMesh::MaterialBucket::positions` triangle in bucket order. Validate positions and normals both have the same multiple of nine floats; reject a triangle whose geometric cross product points opposite its three finite emitted terrain normals.
- [ ] Weld only identical `Float3` bit triples with an ordered key; canonicalize neither signed zero nor NaN. Reject non-finite input before key construction.
- [ ] Assign each first-seen vertex the next `uint32_t` index and append triangle indices in emitted order. Reject any candidate that exceeds Box3D's signed 32-bit vertex or triangle counts.
- [ ] Preserve mesher winding. Reject repeated indices and exactly zero-area triangles using a double-precision cross product; do not silently drop geometry.
- [ ] Validate every local vertex lies in `[-cell_size_m, sector_size_m]` on each axis, with one ULP tolerance at the endpoints. The negative cell is the existing equal-rung bridge ring described by `mesh_sector_tiled`; anything farther outside is invalid.
- [ ] Digest the normalized little-endian coordinate, origin, float bits, and index words. Empty tiles receive a deterministic digest and remain present in the candidate manifest.
- [ ] Check cancellation before each tile, after meshing, and before artifact publication. A cancelled build returns a precise cancellation error and publishes no generation manifest.

### 2.3 Implement immutable per-tile artifacts and generation manifests

- [ ] Store tiles under `<cache_root>/terrain_collision/v1/tiles/<tile-key-16hex>.mtct` and manifests under `<cache_root>/terrain_collision/v1/generations/<installation-key-16hex>.mtcm`.
- [ ] Define MTCT v1 as a fixed little-endian header followed by packed `Float3` bit words and `uint32_t` indices. Header fields are magic, format version, mesher semantic version, source hashes, sector coordinate/origin/size/rung, counts, payload byte count, tile digest, and whole-file digest.
- [ ] Define MTCM v1 as magic/version, geometry and installation keys, material float bits, sorted tile count, and for every tile its coordinate, tile key, digest, empty flag, counts, and artifact bytes; finish with a whole-file digest.
- [ ] Write a unique sibling temporary file, flush/close it, validate the finished bytes, then rename it to the immutable destination. If another writer already published the destination, validate that file and discard the temporary file.
- [ ] On load, validate magic, version, exact file length, checked count arithmetic, source/key consistency, finite geometry, triangle validity, tile digest, and whole-file digest before exposing a tile.
- [ ] A missing or corrupt tile gets exactly one clean rebuild from the canonical field. If rebuilding or replacement validation fails, reject the candidate; never fall back to a render mesh or stale tile.
- [ ] Friction/restitution changes must load the same MTCT paths and publish a new MTCM installation manifest without invoking `mesh_sector_tiled`.
- [ ] Populate `CandidateStats` separately for cache hits, cold builds, validation time, file bytes, empty/non-empty tiles, triangles, and unique vertices.

### 2.4 Add cache and corruption tests

- [ ] Run the same definition twice. Assert the second candidate is byte-equivalent, all sectors are cache hits, `built_tiles == 0`, and the generation manifest validates.
- [ ] Change only friction and assert no tile file timestamp/content changes while installation key and manifest change.
- [ ] Expand and shrink the region union and assert unchanged sector tile keys are reused.
- [ ] Corrupt a header, truncate a payload, alter an index, alter the manifest tile digest, and remove a referenced tile. Assert one clean rebuild repairs recoverable cases and injected rebuild failure fails closed.
- [ ] Simulate a temporary-file interruption and assert no incomplete generation becomes addressable.

### 2.5 Verify and commit Task 2

- [ ] Build and run the artifact, definition, and terrain-mesher regression tests:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target terrain_collision_artifact_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target terrain_mesher_tests
ctest --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R "^(terrain_collision_artifact_tests|terrain_collision_definition_tests|terrain_mesher_tests)$" --output-on-failure
```

- [ ] Run `git diff --check` and inspect all multiplication/conversion sites for checked overflow before allocation.
- [ ] Commit only Task 2 files:

```powershell
git add MatterEngine3/src/terrain_collision/terrain_collision_artifact.h MatterEngine3/src/terrain_collision/terrain_collision_artifact.cpp MatterEngine3/src/terrain_collision/terrain_collision_definition.h MatterEngine3/src/terrain_collision/terrain_collision_definition.cpp MatterEngine3/tests/terrain_collision_artifact_tests.cpp cmake/manifests/engine-core.sources cmake/MatterEngine.cmake MatterEngine3/tests/Makefile
git commit -m "feat: cache deterministic terrain collision tiles"
```

---

## Task 3: Add the Private Transactional Box3D Terrain Runtime

**Files:**

- Create: `MatterEngine3/tests/terrain_collision_physics_tests.cpp`
- Modify: `MatterEngine3/src/ecs/physics_context.h`
- Modify: `MatterEngine3/src/ecs/physics_context.cpp`
- Modify: `cmake/MatterEngine.cmake`
- Modify: `MatterEngine3/tests/Makefile`

The internal physics seam is:

```cpp
struct TerrainCollisionPhysicsStats {
    std::uint64_t installation_key = 0;
    std::uint32_t shape_count = 0;
    std::uint64_t retained_bytes = 0;
    std::uint64_t replacements = 0;
};

bool PhysicsContext::replace_terrain_collision(
    const terrain_collision::TerrainCollisionCandidate& candidate,
    std::string& error);
void PhysicsContext::clear_terrain_collision() noexcept;
TerrainCollisionPhysicsStats
PhysicsContext::terrain_collision_stats() const noexcept;
```

This remains under `matter::physics::detail`; it is not public engine API.

### 3.1 Write failing static-mesh behavior tests

- [ ] Add a test fixture that creates a standalone `PhysicsContext`, converts small deterministic candidates, installs them, and creates ordinary ECS dynamic bodies through the existing reconciliation path.
- [ ] Drop a box and flattened raft onto flat and sloped mesh candidates; assert they settle on top of the surface without penetrating or falling through.
- [ ] Add cave ceiling and overhang candidates. Launch bodies into them and assert contacts occur, proving the implementation is a full triangle mesh rather than a height field.
- [ ] Install adjacent X, Y, and Z tiles and move continuous bodies across their shared planes. Assert no fall-through, explosive velocity, snag, or duplicate installed shape.
- [ ] Launch a continuous body at high speed into a thin terrain feature and assert no tunneling.
- [ ] Assert terrain bodies are static, all shapes use candidate friction/restitution and default collision filtering, and empty tiles create no body/shape.
- [ ] Run the RED build and record the absent method failure:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target terrain_collision_physics_tests
```

### 3.2 Implement the private lifetime owner

- [ ] Add a private `TerrainCollisionRuntime` inside `PhysicsContext::Impl`. Each non-empty tile owns converted `std::vector<b3Vec3>`, `std::vector<int32_t>`, returned `b3MeshData*`, one static `b3BodyId`, and one `b3ShapeId`, plus tile identity for diagnostics.
- [ ] Revalidate counts and every candidate index before casting to Box3D signed integers.
- [ ] Construct `b3MeshDef` with `weldVertices = false` because the worker already performed deterministic exact welding, `identifyEdges = true`, and `useMedianSplit = true` for structured terrain.
- [ ] Create a static body positioned at `tile.origin_m`, create `b3MeshData`, then create one mesh shape with unit scale. Set `shapeDef.baseMaterial.friction/restitution` from the candidate and use the engine's default filter.
- [ ] Pass a `triangleCount + 1` degenerate-index array initialized to `-1` into `b3CreateMesh`; reject the tile if Box3D writes any index or returns a null mesh. Also reject any invalid returned body/shape id. Do not install partially created tiles.
- [ ] Keep body, shape, and mesh data in a temporary replacement runtime until every non-empty tile succeeds. Swap that runtime with the active runtime once; then destroy the retired runtime.
- [ ] Destroy shapes/bodies before `b3DestroyMesh`. Make both partial-construction cleanup and final destruction idempotent.
- [ ] Require the app/physics-owner thread and reject replacement while `Impl::stepping` is true. Fixed ticks perform no terrain geometry work or allocation.
- [ ] `clear_terrain_collision()` destroys the complete active runtime and resets stats. `PhysicsContext::Impl` invokes it before destroying the Box3D world.

### 3.3 Prove replacement and failure atomicity

- [ ] Install generation A, settle a body, inject failure on generation B's second tile, and assert A's installation key, shapes, and behavior remain active.
- [ ] Successfully install generation C and assert all A handles are invalid, only C shapes remain, and replacement count increments once.
- [ ] Reinstall the same installation key and assert it is an allocation-free no-op.
- [ ] Change material-only installation identity over the same geometry and assert shapes are replaced with new material while the worker candidate geometry remains unchanged.
- [ ] Clear and destroy the context under the Box3D allocation tracker used by tests; assert no retained mesh/body/shape leaks.
- [ ] Snapshot allocation counters across steady fixed ticks and assert no terrain-runtime allocation or remeshing.

### 3.4 Verify and commit Task 3

- [ ] Run focused physics and existing physics regressions:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target terrain_collision_physics_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target physics_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target river_float_system_tests
ctest --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R "^(terrain_collision_physics_tests|physics_tests|river_float_system_tests)$" --output-on-failure
```

- [ ] Confirm `rg "box3d" MatterEngine3/include/matter/terrain_collision.h MatterEngine3/src/terrain_collision` returns no matches.
- [ ] Commit only Task 3 files:

```powershell
git add MatterEngine3/src/ecs/physics_context.h MatterEngine3/src/ecs/physics_context.cpp MatterEngine3/tests/terrain_collision_physics_tests.cpp cmake/MatterEngine.cmake MatterEngine3/tests/Makefile
git commit -m "feat: install static terrain collision in box3d"
```

---

## Task 4: Integrate Worker Build, App-Thread Installation, Ready Gating, and Diagnostics

**Files:**

- Create: `MatterEngine3/tests/terrain_collision_session_tests.cpp`
- Modify: `MatterEngine3/include/matter/world_session.h`
- Modify: `MatterEngine3/src/matter_engine.cpp`
- Modify: `cmake/MatterEngine.cmake`
- Modify: `MatterEngine3/tests/Makefile`

### 4.1 Write failing session lifecycle tests

- [ ] Add a session fixture with an in-memory/simple world field and a collision definition. Use production `install_world` and publication seams, substituting only deterministic test hooks already permitted by `WorldSession`.
- [ ] Assert collision meshing runs on the bake worker and Box3D replacement runs on the app thread before the Ready command is observable.
- [ ] Assert a world without collision clears a previously installed terrain runtime and reaches Ready with `TerrainCollisionState::Disabled`.
- [ ] Assert build/cache/validation/install failure emits one `BakeError` with phase `terrain-collision`, records Failed status and precise code/message, and never exposes Ready.
- [ ] Assert cancellation or supersession during tile N neither publishes the manifest nor alters the active physics generation.
- [ ] Assert reload A -> B atomically replaces terrain once; failed reload C leaves B active internally while the world remains Failed/not connected.
- [ ] Assert field hash, overlay hash, and authored sector size/rung are the exact values passed into canonicalization.
- [ ] Run the RED build:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target terrain_collision_session_tests
```

### 4.2 Build the candidate from the installed final field

- [ ] In `install_world`, after `FieldRuntime` is constructed with `installed_overlay` and `world_sector_size` is resolved, read `provider->terrain_collision()` from the retained first-pass `WorldDefinition` data.
- [ ] If present, construct `SourceIdentity` from `world_field->hash()`, `world_river_height_overlay ? hash() : 0`, `terrain_mesher::kSemanticVersion`, and artifact format v1.
- [ ] Canonicalize with the resolved authored base `world_sector_size`, then call `load_or_build_candidate(*world_field, canonical, cfg.cache_root, cancellation-check, ...)` on the worker thread.
- [ ] Keep the complete candidate in a generation-local `shared_ptr<const TerrainCollisionCandidate>` passed into `publish_pipeline`; do not store mutable tile geometry in the ECS or renderer. Add a small publication action with `Keep`, `Clear`, and `Replace`: full world bakes select Clear/Replace, while cone/refine publications select Keep and cannot disturb installed collision.
- [ ] If the definition is absent on a full world bake, pass an explicit Clear operation so reload cannot retain collision from the previous world.
- [ ] Update a mutex-protected `TerrainCollisionStatus` snapshot at Building, CandidateReady, Installed, Disabled, and Failed transitions. Never expose pointers through the public method.

### 4.3 Marshal installation to the physics-owner thread before Ready

- [ ] Add a non-render app-thread publication job immediately before the existing visual reset/reconcile sequence. Reuse the existing blocking app-thread job queue, name it `<pipeline>.terrain-collision`, and document that it owns app/GL/physics-thread affinity even though collision performs no GPU work.
- [ ] In the job, call `physics::detail::context(ecs_runtime.world()).replace_terrain_collision(candidate, error)` or `clear_terrain_collision()` for an omitted definition. Assert the context is not stepping.
- [ ] On failure, return false so `publish_pipeline` emits `BakeErrorCode::Internal`, phase `terrain-collision`, enqueues Failed, leaves the prior physics runtime untouched, and aborts before visual reset and Ready.
- [ ] On success, copy Box3D retained bytes and install timing into status, then continue the existing render publication. The current Ready command remains after all blocking publication barriers, so no additional Ready path is allowed.
- [ ] Check cancellation before installation and again after the app-thread job. A superseded candidate must not reach replacement.
- [ ] Keep candidate lifetime through job completion, then release worker geometry after Box3D has created and retained its private mesh data.

### 4.4 Publish diagnostics and teardown correctly

- [ ] Add `TerrainCollisionStatus WorldSession::terrain_collision_status() const;` to `world_session.h` and implement it as a locked copy.
- [ ] Log one install summary containing generation/geometry keys, rung/cell size, region/sector/empty/non-empty counts, triangles/vertices, artifact/Box3D bytes, cold/cache/validation/install milliseconds.
- [ ] Log the first failure only with a stable short code and full message; preserve it in the status until the next request starts.
- [ ] Ensure WorldSession unload destroys the physics context only after its private terrain runtime has cleared. No Box3D call may occur from the worker destructor.
- [ ] Add session tests proving a status read racing worker progress observes a self-consistent snapshot.

### 4.5 Verify and commit Task 4

- [ ] Run focused lifecycle tests plus async-bake regressions:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target terrain_collision_session_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target async_bake_tests
ctest --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R "^(terrain_collision_session_tests|async_bake_tests)$" --output-on-failure
```

- [ ] Build the complete headless CPU suite to catch source-manifest closure errors:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target matter_engine_cpu_tests
```

- [ ] Commit only Task 4 files:

```powershell
git add MatterEngine3/include/matter/world_session.h MatterEngine3/src/matter_engine.cpp MatterEngine3/tests/terrain_collision_session_tests.cpp cmake/MatterEngine.cmake MatterEngine3/tests/Makefile
git commit -m "feat: gate world readiness on terrain collision"
```

---

## Task 5: Author RiverFloatLab Collision and Smaller Scene-Local Crates

**Files:**

- Create: `projects/world_demo/scenes/RiverFloatLab/objects/RiverCrate.js`
- Modify: `projects/world_demo/scenes/RiverFloatLab/RiverFloatLab.js`
- Modify: `projects/world_demo/tests/river_float_lab_scene_tests.mjs`

### 5.1 Write the failing scene assertions

- [ ] Stub `globalThis.terrainCollision` in the Node test with a recording builder, instantiate the scene, and invoke `collision()`.
- [ ] Assert exactly one builder, one build, `cellSize: 0.5`, friction `0.72`, restitution `0.02`, and exactly one `river-gameplay` region from `[-64,-64,-64]` inclusive to `[384,128,64]` exclusive.
- [ ] Change the reference-crate expectation and every crate recipe expectation to `PartInstance.part === "RiverCrate"` and `BoxCollider.halfExtents === [0.75,0.75,0.75]`.
- [ ] Assert raft dimensions remain `[2.4,0.35,1.5]`, explicit boulder colliders remain present, and the shared `projects/world_demo/objects/Crate.js` source still describes `[1.5,1.5,1.5]` half extents.
- [ ] Load the planned `RiverCrate` part through the same dynamic test harness as `RiverRaft` and assert its emitted box is centered with half extents `[0.75,0.75,0.75]`.
- [ ] Assert equilibrium spawn Y is recomputed from the smaller height and density rather than carrying the old 3 m result.
- [ ] Run the RED Node gate and confirm the collision method/part expectations fail:

```powershell
node --experimental-default-type=module projects/world_demo/tests/river_float_lab_scene_tests.mjs
```

### 5.2 Implement the bounded region and scene-local part

- [ ] Add the exact `collision()` method:

```js
  collision() {
    const collision = terrainCollision({
      cellSize: 0.5,
      friction: 0.72,
      restitution: 0.02,
    });
    collision.region("river-gameplay", {
      min: [-64, -64, -64],
      max: [384, 128, 64],
    });
    collision.build();
  }
```

- [ ] Create `RiverCrate` using the runtime-discoverable non-ESM class convention, existing plaster/wood-like material convention, and a centered `[0.75,0.75,0.75]` box. Add mild smoothing only if the existing crate visual contract permits it; collider dimensions remain exact.
- [ ] In `bodyEntity`, map every non-raft placement to `RiverCrate` and `[0.75,0.75,0.75]`. Leave the shared Crate part and all raft/boulder code untouched.
- [ ] Recompute `bodyHeight`, surface equilibrium, probe layout, and force caps from the new half extents. Keep authored densities and continuous-reference behavior unchanged.

### 5.3 Verify and commit Task 5

- [ ] Run RiverFloatLab and adjacent JS definition tests:

```powershell
node --experimental-default-type=module projects/world_demo/tests/river_float_lab_scene_tests.mjs
node --experimental-default-type=module projects/world_demo/tests/river_hydrology_scene_tests.mjs
```

- [ ] Build the PhysX-enabled MSVC editor from the repository wrapper:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target matter_editor
```

- [ ] Commit only RiverFloatLab files:

```powershell
git add projects/world_demo/scenes/RiverFloatLab/RiverFloatLab.js projects/world_demo/scenes/RiverFloatLab/objects/RiverCrate.js projects/world_demo/tests/river_float_lab_scene_tests.mjs
git commit -m "feat: collide RiverFloatLab with its ravine"
```

---

## Task 6: Run Cold/Cache Acceptance, Capture Screenshots, and Record Evidence

**Files:**

- Create: `docs/findings/river-float-terrain-collision-acceptance-2026-08-25.md`
- Create outside git or in the established ignored evidence directory: RiverFloatLab PNG screenshots and launch logs

### 6.1 Run the full MSVC verification matrix

- [ ] Start from no running editor using the exact MSVC executable path. Confirm the binary timestamp changes after the build and no GCC runtime DLL appears beside it.
- [ ] Build the editor and CPU tests:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target matter_engine_cpu_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target matter_editor
```

- [ ] Run the complete CTest CPU label and the two Node scene suites:

```powershell
ctest --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -L cpu --output-on-failure
node --experimental-default-type=module projects/world_demo/tests/river_float_lab_scene_tests.mjs
node --experimental-default-type=module projects/world_demo/tests/river_hydrology_scene_tests.mjs
```

- [ ] Run `git diff --check` and inspect `git status --short` so unrelated user files remain untouched.

### 6.2 Measure a cold build and cache-hit build

- [ ] Identify only RiverFloatLab's terrain-collision v1 cache directory from the logged cache root. Move that exact directory to a timestamped sibling backup rather than deleting broad cache roots.
- [ ] Launch `MatterEditor/build/windows-msvc/editor.exe` from `MatterEditor/` with `MATTER_WORLD=RiverFloatLab`, wait for `bake.finished`, and save the collision summary/log.
- [ ] Record sector/non-empty/empty counts, triangles, vertices, artifact and retained bytes, cold-build/validation/install time, total bake time, and whether Ready was reached with zero errors.
- [ ] Close cleanly, relaunch without changing inputs, and record the cache-load run. Assert `built_tiles == 0`, every required tile is a cache hit, geometry/installation keys match, and total terrain-collision time drops.
- [ ] Restore or retain the timestamped backup according to the QA runbook; report exactly what was moved and whether it is recoverable.

### 6.3 Capture visible Play-mode proof

- [ ] Use `MatterEngine3/tools/drive.py` with native `py -3`, or the editor FIFO commands from `docs/agent/qa-cookbook.md`, to capture at least these views after Ready and several seconds of real-time Play:
  - upper ravine: smaller crates and a raft contacting bed/banks;
  - curved boulder section: at least one body contacting or deflecting around a boulder;
  - waterfall/pool: bodies remain bounded by terrain through the drop and pool;
  - sector crossing: a body traveling across a visible/canonical 64 m boundary;
  - lower/spillway overview: no hidden domain wall and no body falling through terrain.
- [ ] Keep the editor available for direct user observation after automated captures when practical.
- [ ] Inspect every screenshot at original resolution. Reject captures dominated by UI, clipping, empty channel, or an unhelpful camera angle and retake them.

### 6.4 Record the acceptance report

- [ ] Write `river-float-terrain-collision-acceptance-2026-08-25.md` with commit ids, exact MSVC commands, test totals, cold/cache table, memory/count diagnostics, screenshot paths, observed contacts, and remaining limitations.
- [ ] State explicitly that the water mesh remains static, shader motion remains visual, generated terrain collision is bounded to the authored union, boulders remain explicit shapes, and automated river-traversal CSV telemetry is still deferred.
- [ ] Include any failure honestly. Do not describe the feature as complete unless the final verification output is fresh and green.

### 6.5 Final review and commit

- [ ] Apply `superpowers:requesting-code-review` to the implementation against the approved spec. Resolve correctness findings with new failing tests before changes.
- [ ] Apply `superpowers:verification-before-completion` and rerun every command it identifies as required for the final claim.
- [ ] Commit the evidence report only after its recorded results match the final tree:

```powershell
git add docs/findings/river-float-terrain-collision-acceptance-2026-08-25.md
git commit -m "docs: record terrain collision acceptance"
```

- [ ] Present the user with the result first, then direct links to the report and screenshots, cold/cache timing comparison, exact test totals, and any remaining risk.
