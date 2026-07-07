# autoremesher integration — design spec

**Date**: 2026-07-07
**Status**: Design, pre-implementation
**Author**: brainstorming session, Jack Kern + Claude

## Summary

Integrate [huxingyi/autoremesher](https://github.com/huxingyi/autoremesher) (MIT-licensed
quad-dominant retopology tool built on geogram + Instant-Meshes-style parameterization)
as an **opt-in, per-part** stage in the bake pipeline. Retopo runs on the whole-part
flatten mesh, produces a cleaner-topology triangulation, and feeds the QEM LOD
ladder and cluster split unchanged downstream.

The integration also lands two supporting structural changes in MatterSurfaceLib:

1. A shared `MeshIndexed` interchange type + `MeshTransform` interface used by both
   the existing QEM simplifier and the new retopo module.
2. A new vendored library `Libraries/autoremesher_core/` holding the cherry-picked
   headless subset of autoremesher (no Qt), plus its BSD/MPL/Apache thirdparty deps.

## Motivation

The current QEM ladder produces jagged silhouettes and stretched triangles on organic
parts (rocks, trunks) at coarse LODs; even at moderate density, QEM's edge-collapse
output has irregular vertex distribution and noisy shading normals. Autoremesher's
quad-dominant retopo produces clean edge loops and even vertex distribution, which
improves:

- Silhouette quality at LOD1/LOD2
- Smoothness of baked shading normals and per-vertex AO
- Base topology for UV / chart pipeline work
- Manifold/watertight properties (as a byproduct of the parameterization)

The user's stated intent is "all of the above" — a general-purpose optional stage
that lights up across multiple quality axes, not a targeted point-fix.

## Non-goals

- Not a runtime feature. Retopo is bake-time only, and slow (seconds to minutes per part).
- Not a per-cluster remesh. Cluster meshes share edges with neighbors; independent
  per-cluster retopo drifts rim vertices and creates seams. This design sidesteps the
  problem by running retopo on the merged whole-part flatten mesh *before* cluster split.
- Not a QEM replacement. QEM stays the LOD ladder engine. Retopo produces LOD0; QEM
  decimates LOD1/LOD2 from the retopo'd base.
- Not a subprocess isolation model. The library is linked in-process. Crashes in
  the retopo path are treated as bugs to fix, not to isolate against.
- Not a mesher-format refactor. Meshers continue emitting `Tri`; the `MeshIndexed`
  boundary lives at MSL's entrance. Pushing indexed-native down through the mesher
  layer is a natural follow-up, out of scope here.

## Architecture and pipeline placement

```
part generation → raw Tri + TriEx (materialId, tint authored;
                                    AO=1.0 unbaked; N=face-normal fallback)
        │
        ▼
part_flatten → .flat.part                           [existing;
                                                     per-part decision per
                                                     ROADMAP OOM plan]
        │
        ▼
    ┌───┴────────────────────────────┐
    │ IF part.retopo.enabled:        │   NEW STAGE
    │   MSL::retopo(.flat.part) →    │
    │     .retopo.part               │
    │   reproject materialId + tint  │
    │     via reproject_triex        │
    │     (nearest source centroid)  │
    │ ELSE: pass through             │
    └───┬────────────────────────────┘
        │  AO and shading normals still at unbaked defaults
        ▼
per-vertex AO + shading-normal bake                 [existing:
                                                     MSL::vertex_ao,
                                                     samples retopo'd geometry]
        │
        ▼
bake_lods → QEM ladder                              [existing;
                                                     LOD1/LOD2 QEM'd from
                                                     retopo'd LOD0]
        │
        ▼
cluster split → per-cell mesh assignments           [existing]
        │
        ▼
part BLAS registered
        │
        ▼
world assembly → TLAS of instanced parts            [existing]
        │
        ▼
tileset Wang-tile PBR bake                          [existing:
                                                     tileset_bake_ao;
                                                     sees retopo'd geometry
                                                     by assembly ordering]
        │
        ▼
runtime
```

### Key architectural properties

1. **Retopo is a leaf in the pipeline graph** — one mesh in, one mesh out, no
   knowledge of clusters, LODs, or instancing.
2. **Instance-boundary safety comes for free** — the ROADMAP's per-part flatten
   decision already stops flattening at parts whose subtree exceeds budget.
   Retopo runs on each `.flat.part` individually. Meadow.flat.part is small
   because Tree.flat.part is small; Tree gets retopo'd once and instanced 50k
   times, and no code path tries to retopo a merged 5B-tri monolith.
3. **Per-cluster seam problem sidestepped by ordering** — cluster split runs
   *after* retopo on the merged flatten mesh. Rim vertices are chosen once and
   both sides of every cell boundary share them by construction.
4. **Both lighting bakes see retopo'd geometry**:
   - `MSL::vertex_ao` (per-vertex TriEx AO + shading normals) runs *after*
     retopo and samples the retopo'd surface directly.
   - Tileset Wang-tile PBR bake runs on the assembled world TLAS, which is
     downstream of per-part retopo.
5. **QEM stays the ladder engine** — retopo produces LOD0 only; LOD1 and LOD2
   are QEM decimations of the retopo'd LOD0. No change to `mesh_simplifier`
   or `bake_lods`'s ladder loop.
6. **New artifact**: `.retopo.part`, sibling to `.flat.part`. Keyed on
   `(flat_part_hash, retopo_settings_hash, autoremesher_core_version)`.

## Library / project layout

MSL was previously in a read-only stance (memory: "MatterSurfaceLib read-only has
a genuine-bug exception"). This design deliberately reopens MSL for a substantial
new subsystem, aligned with MSL's stated mission of demonstrating mesh work.
Approved as a per-feature scope decision on 2026-07-07.

### `Libraries/autoremesher_core/` (new vendored library)

```
Libraries/autoremesher_core/
├── LICENSE                          # MIT (from upstream)
├── UPSTREAM.md                      # pinned commit hash, cherry-pick list,
│                                    # extraction notes, upstream URL
├── Makefile                         # builds static lib libautoremesher_core.a
├── include/
│   └── autoremesher/
│       └── remesh.h                 # single public C++ header (see API section)
├── src/
│   ├── remesh.cpp                   # our thin driver
│   ├── mesh_sanitize.cpp            # from upstream: dedup, non-manifold cleanup
│   ├── param_hdc.cpp                # from upstream: HDC parameterization
│   ├── quad_extract.cpp             # from upstream: quad-dominant extraction
│   └── quad_to_tri.cpp              # from upstream: triangulate quads
└── thirdparty/                      # pinned upstream subtrees, unchanged sources
    ├── geogram/                     # BSD-3 core only (no tetgen / GPL modules)
    ├── isotropicremesher/           # Instant-Meshes-derivative
    ├── eigen/                       # MPL2, headers only
    └── tbb/                         # Apache2
```

**Extraction rules** (documented in UPSTREAM.md):

- Pin to a specific upstream commit hash. Every sync is a deliberate cherry-pick.
- Zero Qt files. Anything under `resources/`, `shaders/`, `*.ui`, `main.cpp`,
  `mainwindow.*`, or including `<Q...>` is out.
- Keep only the pipeline files enumerated above. Non-pipeline utilities don't
  come in.
- Preserve upstream copyright notices on every cherry-picked file (MIT requires it).

**Build integration**:

- `Libraries/autoremesher_core/Makefile` produces `libautoremesher_core.a` and
  exposes headers under `include/autoremesher/`.
- MSL links via `-L../Libraries/autoremesher_core -lautoremesher_core -I../Libraries/autoremesher_core/include`.
- `build-all.sh` builds `Libraries/autoremesher_core` before `MatterSurfaceLib`.
- Windows cross-build (per the "always rebuild windows" memory constraint)
  picks up the same static lib built with the mingw toolchain.

**Open item to nail down during extraction (not a design decision)**: geogram's
native build is CMake. Options are (a) a small CMake shim invoked from our
Makefile, or (b) hand-writing a Makefile that compiles the specific geogram
core sources directly. Decision depends on how much of geogram is actually
pulled in.

### MatterSurfaceLib additions

```
MatterSurfaceLib/
├── include/
│   ├── mesh_indexed.hpp             # NEW: shared indexed mesh + TriEx sidecar
│   ├── mesh_transform.hpp           # NEW: interface + reproject_triex helper
│   ├── mesh_retopo.hpp              # NEW: mirrors mesh_simplifier.hpp
│   ├── mesh_simplifier.hpp          # existing, extended (see below)
│   └── (bvh.h, surface.h, vertex_ao.h, cluster.h — unchanged)
├── src/
│   ├── mesh_indexed.cpp             # NEW: weld/unweld, TriEx handling
│   ├── mesh_retopo.cpp              # NEW: thin driver over autoremesher_core
│   ├── mesh_simplifier.cpp          # refactor: add MeshIndexed overload,
│   │                                 keep raylib::Mesh overload as a shim
│   └── (existing files unchanged)
└── tests/
    ├── mesh_indexed_tests.cpp       # NEW: weld/unweld correctness
    ├── mesh_retopo_tests.cpp        # NEW: determinism, options, failure fallback
    └── (existing tests unchanged)
```

### MatterEngine3 wiring

`MatterEngine3` does not link `autoremesher_core` directly. It calls MSL, which
in turn calls the vendored library. Same dependency shape as today's simplifier
call chain.

- `MatterEngine3/src/lod_bake.cpp` — refactor to use `MeshIndexed` at MSL's
  boundary instead of building non-indexed raylib meshes. Removes the current
  double round-trip (`tris_to_mesh` → indexed simplify output → `mesh_to_tris`).
- `MatterEngine3/src/part_flatten.cpp` (or a new small `retopo.cpp` in
  MatterEngine3) — inserts the retopo call between flatten and QEM ladder.
- `MatterEngine3/src/dsl_bindings.cpp` — new `retopo` block on the part
  definition, alongside `FlattenTargets`.
- Cache: new `.retopo.part` artifact, sibling to `.flat.part` in the same
  cache dir, same serialization format.

## API surface

### `Libraries/autoremesher_core/include/autoremesher/remesh.h`

```cpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace autoremesher {

struct Mesh {
    std::vector<float>    positions;  // xyz, xyz, ...  (size = 3 * vertexCount)
    std::vector<uint32_t> indices;    // 3 per triangle
};

struct Options {
    // Target output density, relative to input tri count. Clamped to (0, 4.0].
    float    target_ratio    = 1.0f;
    // Retopo iteration count (upstream default 3).
    int      iterations      = 3;
    // Deterministic RNG seed. Same seed + same input + same options = same output.
    uint32_t seed            = 0;
    // Wall-clock cap; 0 = no limit. Retopo can hang on pathological input.
    int      timeout_seconds = 60;
    // Thread count pinned for determinism (>=1). Default 1.
    int      threads         = 1;
};

struct Result {
    bool        ok = false;
    Mesh        mesh;
    std::string err;
    double      elapsed_seconds = 0.0;
};

// Never throws. Never modifies `in`. Deterministic given (in, opts).
Result remesh(const Mesh& in, const Options& opts);

} // namespace autoremesher
```

### `MatterSurfaceLib/include/mesh_indexed.hpp`

```cpp
#pragma once
#include "bvh.h"  // Tri, TriEx, float3
#include <cstdint>
#include <vector>

// Shared indexed mesh format for MSL's mesh-transformation pipeline.
// Both mesh_simplifier and mesh_retopo consume and produce this.
// Non-indexed Tri is used only at the BLAS boundary.
struct MeshIndexed {
    std::vector<float3>   positions;
    std::vector<uint32_t> indices;      // 3 per triangle
    std::vector<TriEx>    triex;        // optional; parallel to triangles.
                                         // empty vector = TriEx not attached.
};

// Weld tolerance for from_tri. Default matches mesh_simplifier's existing
// internal weld (1e-4 world units).
struct WeldOptions { float epsilon = 1e-4f; };

MeshIndexed from_tri(const std::vector<Tri>& tris,
                     const std::vector<TriEx>* triex,
                     const WeldOptions& opts = {});

void to_tri(const MeshIndexed& in,
            std::vector<Tri>& tris_out,
            std::vector<TriEx>& triex_out);
```

### `MatterSurfaceLib/include/mesh_transform.hpp`

```cpp
#pragma once
#include "mesh_indexed.hpp"

// Contract shared by every MSL mesh transformation:
//   - never modifies input
//   - never throws
//   - deterministic given (input, options)
//   - empty input → empty output
//   - preserves triex.size() * 3 == indices.size() when triex is present
//     (via nearest-source reprojection when triangle count changes)

// Reprojection helper used by any transform that changes the triangle set.
// Nearest-source-centroid; identical semantics to today's
// lod_bake::reproject_triex, moved here for shared use.
void reproject_triex(const MeshIndexed& source, MeshIndexed& target);
```

### `MatterSurfaceLib/include/mesh_retopo.hpp`

```cpp
#pragma once
#include "mesh_indexed.hpp"
#include <cstdint>
#include <string>

struct RetopoOptions {
    float    target_ratio    = 1.0f;
    int      iterations      = 3;
    uint32_t seed            = 0;
    int      timeout_seconds = 60;
    int      threads         = 1;
};

struct RetopoResult {
    MeshIndexed mesh;              // retopo'd; TriEx repopulated via reproject_triex
    bool        ok = false;
    std::string err;
    double      elapsed_seconds = 0.0;
};

// Retopo transform. Wraps autoremesher_core.
// - MeshIndexed ↔ autoremesher::Mesh format adaptation lives here.
// - materialId/tint reprojected via reproject_triex.
// - AO and shading normals left at defaults; vertex_ao runs downstream.
// - On failure, ok=false and err is populated; caller uses input unchanged.
RetopoResult retopo(const MeshIndexed& in, const RetopoOptions& opts);
```

### Engine-side integration example

```cpp
// MatterEngine3/src/part_flatten.cpp or new retopo hook.
if (part.retopo_enabled) {
    auto in  = MSL::from_tri(flat_tris, &flat_triex);
    auto res = MSL::retopo(in, part.retopo_options());
    if (!res.ok) {
        log_warning("retopo: part=\"%s\" err=\"%s\" elapsed=%.2fs → "
                    "falling back to unretopo'd mesh",
                    part.name, res.err.c_str(), res.elapsed_seconds);
        // flat_tris / flat_triex unchanged; QEM ladder proceeds against them.
    } else {
        MSL::to_tri(res.mesh, flat_tris, flat_triex);
        // AO / shading normals still at unbaked defaults; vertex_ao runs next.
    }
}
```

## Schema opt-in and defaults

Retopo is opt-in per-part via a `retopo` block in the DSL part definition:

```javascript
part("Tree", {
    // ... existing part definition ...
    retopo: {
        enabled: true,
        target_ratio: 1.0,   // 1.0 = same density as flatten output
        iterations: 3,
        seed: 0,
        timeout_seconds: 60,
    }
});
```

Defaults if the block is absent:
- `enabled: false`
- `target_ratio: 1.0`
- `iterations: 3`
- `seed: 0`
- `timeout_seconds: 60`
- `threads: 1` — not exposed to the schema; implementation-pinned for determinism.

DSL wiring goes in `MatterEngine3/src/dsl_bindings.cpp` alongside existing
`FlattenTargets` plumbing, same JS-object → C++ struct pattern.

Existing worlds bake identically (default `enabled: false`). No schema migration required.

## Cache

**Artifact**: `<part_hash>.retopo.part`, sibling to `.flat.part` in the same cache dir.

**Cache key**:
```
retopo_cache_key = sha256(
    flat_part_hash
    || serialized(RetopoOptions)      // all fields
    || AUTOREMESHER_CORE_VERSION      // compiled-in string constant
    || PLATFORM_TRIPLE                // linux vs windows-cross, etc.
)
```

- Any source-geometry, retopo-settings, or library-version change invalidates automatically.
- `.retopo.part` uses the same serialization format as `.flat.part`. No new codec.
- Cache miss → run `MSL::retopo()`, write result to disk, load into pipeline.
- Cache hit → load directly; skip retopo entirely.
- Retopo failure → do NOT write a `.retopo.part`; the next bake retries.
- Platform triple in the key sidesteps cross-platform determinism ambiguity.

## Failure handling

| Failure | Detection | Behavior |
|---|---|---|
| Non-manifold input | `autoremesher::Result.ok == false` | `RetopoResult.ok = false`; caller uses input mesh; warn |
| Timeout exceeded | Library watchdog on `Options.timeout_seconds` | Same; err = `"timeout after Ns"` |
| Degenerate output (< 4 tris) | Post-run sanity check in mesh_retopo.cpp | Same; err = `"degenerate output"` |
| Non-manifold output | Not checked in v1 | Deferred. Add a post-run manifold check only if failures land there in practice; the retopo pipeline nominally guarantees manifold output. |
| Retopo core crash | Not caught | Bake fails hard. Mitigation: narrow the cherry-picked paths; add repro tests as discovered. Subprocess isolation deliberately rejected in Q4. |

Log format is a single greppable line per failure:
```
[warn] retopo: part="Tree" err="timeout after 60s" elapsed=60.03s → falling back to unretopo'd mesh
```

## Determinism

Cache correctness depends on `MSL::retopo()` being deterministic given
`(input, options)`. Requirements:

- `RetopoOptions.threads = 1` by default. TBB task scheduling reorders FP
  summations across threads; single-threaded execution eliminates this.
- `RetopoOptions.seed` drives all RNG paths in the library.
- Verified by a determinism test in `mesh_retopo_tests.cpp`: run `retopo()`
  twice with identical inputs, assert byte-identical output positions and indices.
- Cross-platform determinism (Linux native vs Windows cross) is a stretch
  goal, not a guarantee. Platform triple in the cache key sidesteps this.

## Testing

### Unit — MatterSurfaceLib

`tests/mesh_indexed_tests.cpp`:
- `from_tri`/`to_tri` round-trip identity on random Tri sets; TriEx parallel order preserved.
- Weld tolerance: two verts at ε/2 collapse; two at ε*2 stay separate.
- Empty input → empty output, no crash.

`tests/mesh_retopo_tests.cpp`:
- Determinism: same input + options → byte-identical output twice.
- Options plumbing: `target_ratio=0.5` yields ~half the triangles (±20%);
  `target_ratio=1.0` yields ~same count.
- Failure fallback: force a degenerate input → `ok=false`, err populated.
- Timeout: pathological input + `timeout_seconds=1` → `ok=false`, err mentions
  timeout, elapsed ≈ 1s.
- TriEx reprojection: identity retopo on a small mesh preserves materialId
  at expected triangles via nearest-centroid.

### Integration — MatterEngine3

`tests/retopo_integration_tests.cpp`:
- Bake a synthetic 2-part world (Tree + terrain) with `retopo.enabled=true` on Tree only.
- Assert `.retopo.part` present in cache for Tree, absent for terrain.
- Assert QEM ladder builds successfully from retopo'd LOD0.
- Second bake with no source change → cache hit, retopo not re-invoked.
- Third bake after touching `retopo.target_ratio` → cache invalidated,
  retopo re-invoked.

### End-to-end (manual smoke)

Per the viewer_shots.sh workflow:
- Bake `meadow` with `retopo.enabled=true` on Tree.
- Run viewer via `tools/viewer_shots.sh` (FIFO auto-quit).
- Compare screenshots against the non-retopo'd reference. Silhouette
  cleanliness at LOD1/LOD2 is what we're looking for.

## Migration / rollout

- Existing worlds bake identically (default `enabled: false`). No forced migration.
- Meadow's Tree.js is the natural first opt-in — visible silhouette gains on an
  organic part that the memory notes have been driving perf/LOD work.
- If Tree retopo lands cleanly: opt in Rock, then other organic parts progressively.
- Terrain and hand-authored geometric parts (crates, boxes) unlikely to benefit;
  leave them off.

## Follow-up work (out of scope here)

- **Mesher-native indexed format.** Meshers currently emit non-indexed `Tri`;
  MSL welds at its boundary. Push indexed down into `triangle_emit`,
  marching cubes, etc. to remove the remaining Tri→MeshIndexed conversion cost.
- **Per-cluster retopo with boundary preservation.** Extract boundary loops,
  pin during remesh, snap remeshed rim verts back. Real engineering effort;
  worth it only if per-cluster retopo becomes desirable independently.
- **Autoremesher CLI subprocess mode.** If crash isolation ever becomes required
  (e.g., we want to keep the viewer up when retopo dies), the API surface here
  is small enough to reimplement over a subprocess without touching the callers.
- **Native pipeline determinism cross-platform.** Would remove the platform
  triple from the cache key and let a bake on one host warm the cache for another.

## Approved decisions record

Design forks resolved during the 2026-07-07 brainstorming session:

1. **License** — MIT confirmed via GitHub API; thirdparty deps (eigen, geogram,
   isotropicremesher, tbb) are permissive. No CGAL. License concern retired.
2. **Intent** — general-purpose ("all of the above" — coarse LOD silhouettes,
   shading, UV feeder, watertightness).
3. **LOD scope** — retopo LOD0 once; QEM ladder decimates LOD1/LOD2 from
   retopo'd base. Per-level opt-out subsumed by per-part opt-out.
4. **Mesh scope** — whole-part flatten mesh only; cluster split downstream of
   retopo. Per-cluster retopo out of scope.
5. **Integration form** — vendored headless subset in
   `Libraries/autoremesher_core/`. No subprocess.
6. **Failure behavior** — fall back to unretopo'd input with a clear warning.
7. **Extraction strategy** — cherry-pick pipeline `.cpp/.h` + needed thirdparty
   subtrees (Q6 option B — clean tree, working reference pipeline as starting point).
8. **MSL siting** — retopo lives in MatterSurfaceLib alongside the QEM
   simplifier and vertex_ao bake; MSL reopened per-feature for this addition.
9. **Shared type** — `MeshIndexed` + `MeshTransform` boundary in MSL; simplifier
   refactored to expose it publicly. Pipeline-wide indexed adoption phased.
