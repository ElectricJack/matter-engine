# Meadow Density Demo Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A dense 256×256-unit meadow world (heightfield terrain tiles, SDF rocks/pebbles, mesh grass, oak trees) built entirely in JavaScript as an assembly part, hitting ~5–8M on-screen triangles through the default raster path, with only generic C++ engine additions (manifest `expand` flag, LOD floor cull, per-world radius).

**Architecture:** `Meadow.js` is a geometry-less assembly part: its `static requires` declares every variant (256 Terrain tiles, 8 Rocks, 6 Pebbles, 5 Grasses, 1 Tree) and its `build()` scatters ~45k children via the transform stack + `placeChild`. A new generic manifest flag (`expand`) makes the provider promote the root's baked child-instance table into individual world instances, so per-child LOD selection, flattening, floor cull, and instanced raster batching all apply. Spec: `docs/superpowers/specs/2026-07-02-meadow-density-demo-design.md`.

**Tech Stack:** MatterEngine3 (C++17, QuickJS-ng script host, raylib raster path), JS part schemas, headless make-based test suites in `MatterEngine3/tests/`.

## Global Constraints

- **All world building lives in JavaScript.** C++ gains ONLY generic engine features; nothing demo-specific in C++ (no Meadow/Terrain names in engine code besides the manifest file and the `MATTER_WORLD=meadow` viewer world switch).
- **MatterSurfaceLib is read-only.** No MSL changes.
- World size: 16×16 tiles of 16 units = 256×256 units. Tile grid spacing `0.25` (named constant in Terrain.js).
- Scatter constants (all named, top of Meadow.js): `ROCKS = 600`, `PEBBLES = 4000`, `GRASS_CLUMPS = 40000`, `TREES = 40`; variant counts `ROCK_VARIANTS = 8`, `PEBBLE_VARIANTS = 6`, `GRASS_VARIANTS = 5`.
- Noise: 3-octave FBM base ±6 units relief, ~40-unit wavelength; detail octaves ~1.5u @ 0.12 amp and ~0.6u @ 0.04 amp (constants in terrain_noise.js).
- Meadow viewer config: SectorLod active radius `400`, floor cull `min_projected_size = 0.0015` (~1 px at 720p), SectorLod resolver on by default.
- Existing `Demo` (single tree) and `Primitives` worlds must keep working through the same generic code path.
- All schema scatter/geometry randomness must be deterministic (shared-lib `rng(seed)`, never `Date`/entropy) — re-install must be 100% cache hits.
- Commit style: lowercase `feat:`/`fix:`/`test:` prefixes (see `git log`).
- All test commands run from `MatterEngine3/tests/`. Pre-existing failures NOT caused by this work (do not chase): `example_world` segfault, `shared_lib_tests` linker error, `part_graph_integration_tests` (references non-existent Trunk.js).

## DSL facts the implementer needs (verified against source)

- Base class verbs (src/part_base.js.h): `pushMatrix/popMatrix`, `translate(x,y,z)`, `rotateY(r)` (radians), `scale(x,y,z)`, `fill(mat)`, `tint(r,g,b,a)`, `beginVoxels(spacing)/endVoxels`, `sphere([cx,cy,cz], r)`, `box([cx,cy,cz], [hx,hy,hz])` (center + half-extents), `smoothing(k)`, `beginShape(mode)/vertex(x,y,z)/endShape`, `placeChild(module, params)`.
- `SHAPE = { triangles: 0, strip: 1, fan: 2, polygon: 3 }`; `MAT = { bark:14, leaf:15, dirt:16, grass:2, stone:8, stoneDark:9, rock:11, ... }`.
- CSG: `difference()` **retroactively retags the LAST-emitted brush** (emit the carving brush, then call `this.difference()` — see `examples/primitive_demo/schemas/VoxelPrims.js:8`).
- Mesh emission needs no session begin — `beginShape` outside `beginVoxels` emits mesh triangles (see Leaf.js). `fill`/`tint` are cursors applied to subsequent triangles.
- Parametric children: `static requires = [{ module: 'Rock', params: { seed: 3 } }, ...]` and `placeChild('Rock', { seed: 3 })` — the placement is keyed by canonical params JSON; integer-valued numbers match (proven by the `{shade:N}` pattern in part_graph_integration_tests.cpp).
- `static requires` is evaluated at class-definition time and CANNOT depend on build-time params; generate it programmatically with a module-level function.
- Shared-lib imports: `import { rng } from 'shared-lib/rng';` — `rng(seed)` returns `{ next, random(), int(n), range(a,b) }`, fully deterministic.
- Row-major transforms everywhere: translation lives in `m[3], m[7], m[11]`.

---

### Task 1: Shared terrain noise + Terrain.js heightfield rewrite

**Files:**
- Create: `MatterEngine3/shared-lib/terrain_noise.js`
- Rewrite: `MatterEngine3/examples/world_demo/schemas/Terrain.js`
- Create: `MatterEngine3/tests/meadow_bake_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile` (new `meadow_bake_tests` target)

**Interfaces:**
- Consumes: existing bake pipeline (`PartGraph`, `ScriptHost`, `load_v2`).
- Produces: `heightAt(x, z)` and `slopeAt(x, z)` exported from `shared-lib/terrain_noise` (world units, deterministic) — Tasks 4 uses both; `Terrain` schema with `static params = { tx: 0, tz: 0 }`, local footprint x/z ∈ [0,16], heights sampled at `(tx*16 + local, tz*16 + local)`.

- [ ] **Step 1: Write the failing test — new test target + terrain checks**

Create `MatterEngine3/tests/meadow_bake_tests.cpp`:

```cpp
// Headless bake tests for the Meadow schemas: shared terrain noise + Terrain
// heightfield tiles (seam agreement), Rock/Pebble SDF variants, Grass clumps.
// Fresh sandbox each run (these bakes are fast, unlike the full Meadow world).
#include "part_graph.h"
#include "part_asset_v2.h"
#include "blas_manager.hpp"
#include "tlas_manager.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>
#include <limits.h>
#include <unistd.h>

using namespace part_graph;

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); ++failures; } \
    else         { printf("ok:   %s\n", (msg)); } } while (0)

static std::string abspath(const std::string& rel) {
    char buf[PATH_MAX];
    if (realpath(rel.c_str(), buf)) return std::string(buf);
    return rel;
}

// All LOD0 triangles of a baked part, concatenated across its BLAS entries.
static std::vector<Tri> load_tris(uint64_t h) {
    std::string path = part_asset::cache_path_resolved(h);
    BLASManager blas; TLASManager tlas(256);
    std::vector<part_asset::ChildInstance> children;
    part_asset::LodLevels lods;
    std::vector<Tri> out;
    if (!part_asset::load_v2(path, h, blas, tlas, children, lods)) return out;
    for (const auto& e : blas.get_entries())
        out.insert(out.end(), e->triangles.begin(), e->triangles.end());
    return out;
}

static ParamValue num(double v) { return ParamValue::number(v); }

int main() {
    const std::string schemas    = abspath("../examples/world_demo/schemas");
    const std::string shared_lib = abspath("../shared-lib");

    const std::string sandbox = "/tmp/me3_meadow_schemas";
    system(("rm -rf " + sandbox).c_str());
    system(("mkdir -p " + sandbox + "/parts").c_str());
    if (chdir(sandbox.c_str()) != 0) { printf("FAIL: chdir sandbox\n"); return 1; }

    script_host::ScriptHost host;
    host.set_shared_lib_root(shared_lib);
    FileModuleResolver resolver(host, schemas);
    HostBaker baker(host, ".");
    PartGraph graph(resolver, baker);

    // ---- Terrain: two horizontally adjacent tiles --------------------------
    std::vector<ChildRequest> tiles = {
        ChildRequest{ "Terrain", {{"tx", num(0)}, {"tz", num(0)}} },
        ChildRequest{ "Terrain", {{"tx", num(1)}, {"tz", num(0)}} },
    };
    InstallResult ir = graph.install(tiles);
    CHECK(ir.ok, "terrain install ok");
    if (!ir.ok) { printf("  error: %s\n", ir.error.c_str()); return 1; }
    CHECK(ir.root_hashes.size() == 2 && ir.root_hashes[0] != ir.root_hashes[1],
          "different tile coords -> different variants");

    std::vector<Tri> a = load_tris(ir.root_hashes[0]);   // tile (0,0)
    std::vector<Tri> b = load_tris(ir.root_hashes[1]);   // tile (1,0)
    CHECK(a.size() == 8192, "tile is a 64x64 quad grid (8192 tris)");
    CHECK(b.size() == 8192, "second tile same density");

    // Relief sanity: FBM base is +-6 units; heights must actually vary.
    float ymin = 1e9f, ymax = -1e9f;
    for (const auto& t : a) {
        for (const float3* v : { &t.vertex0, &t.vertex1, &t.vertex2 }) {
            if (v->y < ymin) ymin = v->y;
            if (v->y > ymax) ymax = v->y;
        }
    }
    CHECK(ymax - ymin > 0.2f, "tile has visible relief (not flat)");
    CHECK(ymax < 8.0f && ymin > -8.0f, "heights within the +-6-ish design range");

    // Seam: tile(0,0) local x==16 must equal tile(1,0) local x==0 at every z.
    auto edge_heights = [](const std::vector<Tri>& tris, float edge_x) {
        std::map<int, float> h;   // lround(z / 0.25) -> y
        for (const auto& t : tris)
            for (const float3* v : { &t.vertex0, &t.vertex1, &t.vertex2 })
                if (std::fabs(v->x - edge_x) < 1e-4f)
                    h[(int)std::lround(v->z / 0.25f)] = v->y;
        return h;
    };
    std::map<int, float> ha = edge_heights(a, 16.0f);
    std::map<int, float> hb = edge_heights(b, 0.0f);
    CHECK(ha.size() == 65 && hb.size() == 65, "both tiles expose 65 seam lattice points");
    bool seam_ok = (ha.size() == hb.size());
    for (const auto& kv : ha) {
        auto it = hb.find(kv.first);
        if (it == hb.end() || std::fabs(it->second - kv.second) > 1e-5f) { seam_ok = false; break; }
    }
    CHECK(seam_ok, "adjacent tiles agree on shared-edge heights (seam check)");

    // Determinism: a fresh graph over the same warm cache re-resolves the same
    // hashes and bakes nothing.
    PartGraph graph2(resolver, baker);
    InstallResult ir2 = graph2.install(tiles);
    CHECK(ir2.ok && ir2.baked.empty(), "re-install bakes nothing (deterministic)");
    CHECK(ir2.root_hashes == ir.root_hashes, "re-install resolves identical hashes");

    printf(failures ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", failures);
    return failures ? 1 : 0;
}
```

Add to `MatterEngine3/tests/Makefile` (after the `tree_bake_check` block, mirroring its pattern):

```makefile
# Headless bake tests for the Meadow schemas (terrain noise seam, rock/pebble/
# grass variants). Fresh sandbox per run.
MEADOW_TARGET = meadow_bake_tests
MEADOW_CPP = meadow_bake_tests.cpp $(filter-out example_world.cpp, $(EXAMPLE_CPP))
$(MEADOW_TARGET): $(MEADOW_CPP) $(EXAMPLE_C) $(QJS_C)
	gcc -c $(QJS_C) -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC)
	gcc -c $(EXAMPLE_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(MEADOW_CPP) $(QJS_OBJ) $(EXAMPLE_C_OBJ) -o $(MEADOW_TARGET) \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS)
	rm -f $(QJS_OBJ) $(EXAMPLE_C_OBJ)

run-meadow: $(MEADOW_TARGET)
	./$(MEADOW_TARGET)
```

Also add `run-meadow` to the `.PHONY` line and `$(MEADOW_TARGET)` to the `clean` rule's `rm -f` list.

- [ ] **Step 2: Run test to verify it fails**

Run: `cd MatterEngine3/tests && make run-meadow`
Expected: FAIL — install error (Terrain has no `tx`/`tz` params yet; `shared-lib/terrain_noise` doesn't exist).

- [ ] **Step 3: Write `shared-lib/terrain_noise.js`**

```js
// Shared terrain height function for the Meadow world. Seeded value noise on an
// integer lattice (splitmix-style integer hash), 3-octave FBM base (rolling
// hills, ~+-6 units relief, dominant wavelength ~40 units) plus two
// high-frequency detail octaves (dirt clods / uneven turf). Deterministic: no
// entropy, integer hashing only. Terrain tiles AND the Meadow scatter both
// import this, so heights and placements always agree; editing this file
// changes the source fold of every importer (full re-bake, intended).

const SEED = 1337;

// Base relief (world units) and dominant wavelength of the rolling hills.
export const BASE_AMP        = 6.0;
export const BASE_WAVELENGTH = 40.0;
// High-frequency detail octaves: [wavelength, amplitude].
export const DETAIL_OCTAVES = [[1.5, 0.12], [0.6, 0.04]];

function hash2(ix, iz, seed) {
  let h = (Math.imul(ix, 0x27d4eb2d) ^ Math.imul(iz, 0x165667b1) ^ seed) >>> 0;
  h = Math.imul(h ^ (h >>> 15), 0x85ebca6b) >>> 0;
  h = Math.imul(h ^ (h >>> 13), 0xc2b2ae35) >>> 0;
  h = (h ^ (h >>> 16)) >>> 0;
  return h / 4294967296;                       // [0, 1)
}

function smooth(t) { return t * t * (3 - 2 * t); }

// Bilinear value noise in [-1, 1].
function valueNoise(x, z, seed) {
  const ix = Math.floor(x), iz = Math.floor(z);
  const fx = x - ix, fz = z - iz;
  const a = hash2(ix, iz, seed),     b = hash2(ix + 1, iz, seed);
  const c = hash2(ix, iz + 1, seed), d = hash2(ix + 1, iz + 1, seed);
  const u = smooth(fx), v = smooth(fz);
  const n = (a + (b - a) * u) * (1 - v) + (c + (d - c) * u) * v;
  return n * 2 - 1;
}

// n-octave FBM in [-1, 1] (amplitude halves, frequency doubles per octave).
function fbm(x, z, seed, octaves, baseFreq) {
  let sum = 0, amp = 1, freq = baseFreq, norm = 0;
  for (let i = 0; i < octaves; ++i) {
    sum  += amp * valueNoise(x * freq, z * freq, seed + i);
    norm += amp;
    amp *= 0.5; freq *= 2;
  }
  return sum / norm;
}

// Terrain height in world units at world-space (x, z).
export function heightAt(x, z) {
  let h = BASE_AMP * fbm(x, z, SEED, 3, 1 / BASE_WAVELENGTH);
  for (let i = 0; i < DETAIL_OCTAVES.length; ++i) {
    const wl = DETAIL_OCTAVES[i][0], amp = DETAIL_OCTAVES[i][1];
    h += amp * valueNoise(x / wl, z / wl, SEED + 101 * (i + 1));
  }
  return h;
}

// Slope magnitude |grad h| from central differences (material choice, grass thinning).
export function slopeAt(x, z) {
  const e = 0.5;
  const dx = (heightAt(x + e, z) - heightAt(x - e, z)) / (2 * e);
  const dz = (heightAt(x, z + e) - heightAt(x, z - e)) / (2 * e);
  return Math.sqrt(dx * dx + dz * dz);
}
```

- [ ] **Step 4: Rewrite `examples/world_demo/schemas/Terrain.js`**

Replace the whole file:

```js
import { heightAt } from 'shared-lib/terrain_noise';

// One 16x16-unit heightfield tile of the Meadow: a 64x64 quad grid (SPACING
// 0.25) sampled from the shared terrain noise. Local x/z span [0, TILE]; the
// parent places the tile at (tx*TILE, 0, tz*TILE), so heights sample the noise
// at the tile origin + local offset and adjacent tiles share identical edge
// samples (seamless). Material by slope: steep quads read as dirt, flat as
// grass. SPACING 0.125 is the terrain lever for the breaking-point run.
class Terrain extends Part {
  static params = { tx: 0, tz: 0 };

  build(p) {
    const TILE = 16.0;
    const SPACING = 0.25;
    const N = Math.round(TILE / SPACING);        // 64 quads per side
    const SLOPE_DIRT = 0.55;                     // |grad h| above this -> dirt
    const ox = p.tx * TILE, oz = p.tz * TILE;    // world-space tile origin

    // Sample the (N+1)^2 height lattice once.
    const h = [];
    for (let j = 0; j <= N; ++j) {
      const row = [];
      for (let i = 0; i <= N; ++i)
        row.push(heightAt(ox + i * SPACING, oz + j * SPACING));
      h.push(row);
    }

    for (let j = 0; j < N; ++j) {
      for (let i = 0; i < N; ++i) {
        const x0 = i * SPACING, x1 = x0 + SPACING;
        const z0 = j * SPACING, z1 = z0 + SPACING;
        const h00 = h[j][i],     h10 = h[j][i + 1];
        const h01 = h[j + 1][i], h11 = h[j + 1][i + 1];
        // Per-quad slope from the already-sampled lattice (no extra noise calls).
        const sx = (h10 - h00 + h11 - h01) * 0.5 / SPACING;
        const sz = (h01 - h00 + h11 - h10) * 0.5 / SPACING;
        this.fill(Math.sqrt(sx * sx + sz * sz) > SLOPE_DIRT ? MAT.dirt : MAT.grass);
        this.beginShape(SHAPE.triangles);
          this.vertex(x0, h00, z0); this.vertex(x0, h01, z1); this.vertex(x1, h10, z0);
          this.vertex(x1, h10, z0); this.vertex(x0, h01, z1); this.vertex(x1, h11, z1);
        this.endShape();
      }
    }
  }
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cd MatterEngine3/tests && make run-meadow`
Expected: `ALL PASS` (terrain install ok, 8192 tris, relief, seam, determinism).

- [ ] **Step 6: Regression — existing suites**

Run: `cd MatterEngine3/tests && make run-treebake && make run-viewer-logic`
Expected: both pass (Terrain rewrite doesn't affect Tree; Demo world still bakes — its manifest still lists Terrain, which now bakes with `tx:0,tz:0` defaults).

- [ ] **Step 7: Commit**

```bash
git add MatterEngine3/shared-lib/terrain_noise.js \
        MatterEngine3/examples/world_demo/schemas/Terrain.js \
        MatterEngine3/tests/meadow_bake_tests.cpp MatterEngine3/tests/Makefile
git commit -m "feat: shared terrain noise + heightfield Terrain tiles with seam-exact edges"
```

---

### Task 2: Rock.js + Pebble.js SDF variants

**Files:**
- Create: `MatterEngine3/examples/world_demo/schemas/Rock.js`
- Create: `MatterEngine3/examples/world_demo/schemas/Pebble.js`
- Modify: `MatterEngine3/tests/meadow_bake_tests.cpp` (append checks)

**Interfaces:**
- Produces: `Rock` and `Pebble` schemas, each `static params = { seed: 0 }`, geometry sitting on/above y=0 so Meadow can sink them slightly. Rock ≈ 1-unit boulder (voxel 0.15); Pebble ≈ fist-size (voxel 0.05).

- [ ] **Step 1: Write the failing test — append to `meadow_bake_tests.cpp` before the final `printf`**

```cpp
    // ---- Rock / Pebble variants --------------------------------------------
    std::vector<ChildRequest> boulders = {
        ChildRequest{ "Rock",   {{"seed", num(0)}} },
        ChildRequest{ "Rock",   {{"seed", num(1)}} },
        ChildRequest{ "Pebble", {{"seed", num(0)}} },
    };
    InstallResult rr = graph.install(boulders);
    CHECK(rr.ok, "rock/pebble install ok");
    if (rr.ok) {
        CHECK(rr.root_hashes[0] != rr.root_hashes[1], "rock seeds 0/1 are distinct variants");
        size_t rt = load_tris(rr.root_hashes[0]).size();
        size_t pt = load_tris(rr.root_hashes[2]).size();
        printf("  rock tris=%zu pebble tris=%zu\n", rt, pt);
        CHECK(rt > 200,  "rock has real geometry");
        CHECK(pt > 50,   "pebble has real geometry");
        CHECK(rt < 40000 && pt < 20000, "rock/pebble tri counts stay budget-sane");
    }
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd MatterEngine3/tests && make run-meadow`
Expected: FAIL — `rock/pebble install ok` fails (modules don't exist).

- [ ] **Step 3: Write `Rock.js`**

```js
import { rng } from 'shared-lib/rng';

// A ~1-unit SDF boulder: 3-5 overlapping smoothed spheres/boxes jittered by
// seed, then 1-2 difference cuts for facets (difference() retags the
// last-emitted brush). One baked variant per seed; Meadow instances them with
// random yaw/scale and sinks them ~15% into the ground.
class Rock extends Part {
  static params = { seed: 0 };

  build(p) {
    const r = rng(1000 + p.seed);
    this.beginVoxels(0.15);
    this.fill(MAT.rock);
    this.smoothing(0.12);

    const blobs = 3 + r.int(3);                 // 3-5 union brushes
    for (let i = 0; i < blobs; ++i) {
      const c = [r.range(-0.35, 0.35), r.range(0.2, 0.55), r.range(-0.35, 0.35)];
      if (r.random() < 0.5) {
        this.sphere(c, r.range(0.3, 0.55));
      } else {
        this.box(c, [r.range(0.25, 0.5), r.range(0.2, 0.4), r.range(0.25, 0.5)]);
      }
    }

    const cuts = 1 + r.int(2);                  // 1-2 facet cuts
    for (let i = 0; i < cuts; ++i) {
      const ang = r.range(0, Math.PI * 2);
      const d = r.range(0.75, 1.0);
      this.box([Math.cos(ang) * d, r.range(0.4, 0.9), Math.sin(ang) * d],
               [0.45, 0.45, 0.45]);
      this.difference();                        // retag the cut brush
    }

    this.endVoxels();
  }
}
```

- [ ] **Step 4: Write `Pebble.js`**

```js
import { rng } from 'shared-lib/rng';

// A fist-size SDF pebble: 2-4 overlapping smoothed spheres jittered by seed.
// Same recipe as Rock at ~1/4 the size and a finer voxel grid. One baked
// variant per seed, scattered in thousands by Meadow.
class Pebble extends Part {
  static params = { seed: 0 };

  build(p) {
    const r = rng(2000 + p.seed);
    this.beginVoxels(0.05);
    this.fill(r.random() < 0.5 ? MAT.stone : MAT.stoneDark);
    this.smoothing(0.04);
    const blobs = 2 + r.int(3);
    for (let i = 0; i < blobs; ++i) {
      this.sphere([r.range(-0.06, 0.06), r.range(0.06, 0.14), r.range(-0.06, 0.06)],
                  r.range(0.07, 0.13));
    }
    this.endVoxels();
  }
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cd MatterEngine3/tests && make run-meadow`
Expected: `ALL PASS`. If a facet cut hollows a rock to < 200 tris, shrink the cut half-extents (0.45) or push `d`'s range outward — the cut must bite the surface, not the core.

- [ ] **Step 6: Commit**

```bash
git add MatterEngine3/examples/world_demo/schemas/Rock.js \
        MatterEngine3/examples/world_demo/schemas/Pebble.js \
        MatterEngine3/tests/meadow_bake_tests.cpp
git commit -m "feat: seeded SDF Rock and Pebble variant schemas"
```

---

### Task 3: Grass.js mesh-blade rewrite

**Files:**
- Rewrite: `MatterEngine3/examples/world_demo/schemas/Grass.js`
- Modify: `MatterEngine3/tests/meadow_bake_tests.cpp` (append checks)

**Interfaces:**
- Produces: `Grass` schema, `static params = { seed: 0 }`; 25–35 tapered mesh blades, 3 tris each (~75–105 tris/clump; the spec table's "200–300" estimate is superseded by its explicit blade structure — `BLADES` is the density lever), root skirt below y=0, per-blade tint variation.

- [ ] **Step 1: Write the failing test — append to `meadow_bake_tests.cpp` before the final `printf`**

```cpp
    // ---- Grass clump --------------------------------------------------------
    InstallResult gr = graph.install({ ChildRequest{ "Grass", {{"seed", num(0)}} } });
    CHECK(gr.ok, "grass install ok");
    if (gr.ok) {
        std::vector<Tri> gt = load_tris(gr.root_hashes[0]);
        printf("  grass tris=%zu\n", gt.size());
        CHECK(gt.size() >= 60 && gt.size() <= 400, "grass clump tri count in budget");
        // Blades are mesh strips, not voxels: some vertices must dip below y=0
        // (root skirt) and reach above y=0.3 (blade tips).
        bool below = false, above = false;
        for (const auto& t : gt)
            for (const float3* v : { &t.vertex0, &t.vertex1, &t.vertex2 }) {
                if (v->y < -0.01f) below = true;
                if (v->y >  0.30f) above = true;
            }
        CHECK(below && above, "grass has a root skirt below y=0 and tips above");
    }
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd MatterEngine3/tests && make run-meadow`
Expected: FAIL — the old voxel Grass bakes far more tris than 400 and has no vertices below y=0 (or the params mismatch errors).

- [ ] **Step 3: Rewrite `Grass.js`**

Replace the whole file:

```js
import { rng } from 'shared-lib/rng';

// A grass clump: BLADES tapered mesh blades (one 5-vertex triangle strip each
// = 3 tris/blade) with a slight bend and per-blade tint variation, plus a small
// root skirt below y=0 so slope placement never leaves floating blades. A few
// seeded variants are baked once each and instanced tens of thousands of times
// by Meadow. BLADES is the per-clump density lever.
class Grass extends Part {
  static params = { seed: 0 };

  build(p) {
    const r = rng(3000 + p.seed);
    const BLADES = 25 + r.int(11);      // 25-35 blades
    const SKIRT = 0.08;                 // root depth below y=0

    this.fill(MAT.grass);
    for (let b = 0; b < BLADES; ++b) {
      const ang = r.range(0, Math.PI * 2);
      const d = r.range(0, 0.35);       // clump radius
      const hgt = r.range(0.35, 0.7);
      const w = r.range(0.02, 0.035);   // half-width at base
      const lean = r.range(0.05, 0.25); // tip lateral offset (bend)
      const g = r.range(0.75, 1.1);     // per-blade tint variation

      this.tint(0.32 * g, 0.55 * g, 0.18 * g, 0.6);
      this.pushMatrix();
      this.translate(Math.cos(ang) * d, 0, Math.sin(ang) * d);
      this.rotateY(r.range(0, Math.PI * 2));
      // 5-vertex strip: root pair (below y=0), tapered mid pair, tip = 3 tris.
      this.beginShape(SHAPE.strip);
        this.vertex(-w, -SKIRT, 0);
        this.vertex( w, -SKIRT, 0);
        this.vertex(-w * 0.6, hgt * 0.55, lean * 0.5);
        this.vertex( w * 0.6, hgt * 0.55, lean * 0.5);
        this.vertex( 0, hgt, lean);
      this.endShape();
      this.popMatrix();
    }
  }
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd MatterEngine3/tests && make run-meadow`
Expected: `ALL PASS`.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/examples/world_demo/schemas/Grass.js \
        MatterEngine3/tests/meadow_bake_tests.cpp
git commit -m "feat: mesh-blade Grass clump with bend, tint variation, root skirt"
```

---

### Task 4: Meadow.js assembly world + full-bake check

**Files:**
- Create: `MatterEngine3/examples/world_demo/schemas/Meadow.js`
- Create: `MatterEngine3/tests/meadow_bake_check.cpp`
- Modify: `MatterEngine3/tests/Makefile` (new `meadow_bake_check` target)

**Interfaces:**
- Consumes: `heightAt`/`slopeAt` (Task 1), `Terrain{tx,tz}`, `Rock{seed}`, `Pebble{seed}`, `Grass{seed}` (Tasks 1–3), existing `Tree`.
- Produces: `Meadow` assembly part — zero own geometry, child-instance table of exactly `256 + 600 + 4000 + 40000 + 40 = 44896` placements over `256 + 8 + 6 + 5 + 1 = 276` unique variants. Task 6 expands this table; Task 8 wires the world.

- [ ] **Step 1: Write the failing test — `meadow_bake_check.cpp`**

```cpp
// Headless full-world bake check for the Meadow assembly part. PERSISTENT
// sandbox (like tree_bake_check): the first run bakes 276 variants (minutes);
// later runs are warm and validate cache-hit determinism cheaply.
#include "part_graph.h"
#include "part_asset_v2.h"
#include "blas_manager.hpp"
#include "tlas_manager.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <set>
#include <string>
#include <vector>
#include <limits.h>
#include <unistd.h>

using namespace part_graph;

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); ++failures; } \
    else         { printf("ok:   %s\n", (msg)); } } while (0)

static std::string abspath(const std::string& rel) {
    char buf[PATH_MAX];
    if (realpath(rel.c_str(), buf)) return std::string(buf);
    return rel;
}

int main() {
    const std::string schemas    = abspath("../examples/world_demo/schemas");
    const std::string shared_lib = abspath("../shared-lib");

    const std::string sandbox = "/tmp/me3_meadow_bake";
    system(("mkdir -p " + sandbox + "/parts").c_str());
    if (chdir(sandbox.c_str()) != 0) { printf("FAIL: chdir sandbox\n"); return 1; }

    script_host::ScriptHost host;
    host.set_shared_lib_root(shared_lib);
    FileModuleResolver resolver(host, schemas);
    HostBaker baker(host, ".");
    PartGraph graph(resolver, baker);

    std::time_t t0 = std::time(nullptr);
    InstallResult ir = graph.install({ ChildRequest{ "Meadow", {} } });
    printf("[install] %lds, baked %zu artifact(s), %d hit(s)\n",
           (long)(std::time(nullptr) - t0), ir.baked.size(), ir.hits);
    CHECK(ir.ok, "meadow install ok");
    if (!ir.ok) { printf("  error: %s\n", ir.error.c_str()); return 1; }

    // Read back the root artifact: geometry-less assembly + full child table.
    uint64_t root = ir.root_hashes[0];
    BLASManager blas; TLASManager tlas(256);
    std::vector<part_asset::ChildInstance> children;
    part_asset::LodLevels lods;
    CHECK(part_asset::load_v2(part_asset::cache_path_resolved(root), root,
                              blas, tlas, children, lods),
          "meadow root artifact loads");

    size_t tris = 0;
    for (const auto& e : blas.get_entries()) tris += e->triangles.size();
    CHECK(tris == 0, "meadow root has zero own geometry (pure assembly)");

    printf("[root] children=%zu\n", children.size());
    CHECK(children.size() == 44896,
          "child table = 256 tiles + 600 rocks + 4000 pebbles + 40000 grass + 40 trees");

    std::set<uint64_t> uniq;
    for (const auto& c : children) uniq.insert(c.child_resolved_hash);
    printf("[root] unique variants=%zu\n", uniq.size());
    CHECK(uniq.size() == 276, "276 unique variants (256+8+6+5+1)");

    // Determinism: a fresh graph over the warm cache = same hash, zero bakes.
    PartGraph graph2(resolver, baker);
    InstallResult ir2 = graph2.install({ ChildRequest{ "Meadow", {} } });
    CHECK(ir2.ok && ir2.baked.empty(), "warm re-install bakes nothing");
    CHECK(!ir2.root_hashes.empty() && ir2.root_hashes[0] == root,
          "warm re-install resolves the identical root hash");

    printf(failures ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", failures);
    return failures ? 1 : 0;
}
```

Add to `MatterEngine3/tests/Makefile` (below the meadow_bake_tests block):

```makefile
# Full Meadow world bake check (persistent sandbox; first run bakes ~276 variants).
MEADOWCHECK_TARGET = meadow_bake_check
MEADOWCHECK_CPP = meadow_bake_check.cpp $(filter-out example_world.cpp, $(EXAMPLE_CPP))
$(MEADOWCHECK_TARGET): $(MEADOWCHECK_CPP) $(EXAMPLE_C) $(QJS_C)
	gcc -c $(QJS_C) -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC)
	gcc -c $(EXAMPLE_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(MEADOWCHECK_CPP) $(QJS_OBJ) $(EXAMPLE_C_OBJ) -o $(MEADOWCHECK_TARGET) \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS)
	rm -f $(QJS_OBJ) $(EXAMPLE_C_OBJ)

run-meadow-check: $(MEADOWCHECK_TARGET)
	./$(MEADOWCHECK_TARGET)
```

Also add `run-meadow-check` to `.PHONY` and `$(MEADOWCHECK_TARGET)` to `clean`.

- [ ] **Step 2: Run test to verify it fails**

Run: `cd MatterEngine3/tests && make run-meadow-check`
Expected: FAIL — `Meadow` module not found.

- [ ] **Step 3: Write `Meadow.js`**

```js
import { rng } from 'shared-lib/rng';
import { heightAt, slopeAt } from 'shared-lib/terrain_noise';

// The Meadow world as a geometry-less assembly part. `static requires`
// declares every variant; build() scatters ~45k children with the transform
// stack + placeChild. All placement randomness comes from one seeded rng, so
// the scatter is deterministic and content-addressed. The world manifest
// places this root with the `expand` flag, promoting each child placement to
// an individual world instance (per-child LOD, culling, instanced batching).

// ---- Scatter constants (the tuning surface for the density benchmark) -----
const TILES = 16;                 // world = TILES x TILES terrain tiles
const TILE  = 16.0;               // world units per tile (must match Terrain.js)
const ROCK_VARIANTS = 8, PEBBLE_VARIANTS = 6, GRASS_VARIANTS = 5;
const ROCKS = 600, PEBBLES = 4000, GRASS_CLUMPS = 40000, TREES = 40;
const TREE_MIN_DIST = 24.0;       // rejection-sampling spacing between oaks
const GRASS_SLOPE_MAX = 0.5;      // thin grass on slopes steeper than this
const SCATTER_SEED = 20260702;
// ---------------------------------------------------------------------------

function makeRequires() {
  const req = [];
  for (let tz = 0; tz < TILES; ++tz)
    for (let tx = 0; tx < TILES; ++tx)
      req.push({ module: 'Terrain', params: { tx: tx, tz: tz } });
  for (let s = 0; s < ROCK_VARIANTS; ++s)   req.push({ module: 'Rock',   params: { seed: s } });
  for (let s = 0; s < PEBBLE_VARIANTS; ++s) req.push({ module: 'Pebble', params: { seed: s } });
  for (let s = 0; s < GRASS_VARIANTS; ++s)  req.push({ module: 'Grass',  params: { seed: s } });
  req.push({ module: 'Tree' });
  return req;
}

class Meadow extends Part {
  static requires = makeRequires();

  build(p) {
    const r = rng(SCATTER_SEED);
    const W = TILES * TILE;                       // 256

    // Terrain tiles at their grid origins (heights baked into tile geometry).
    for (let tz = 0; tz < TILES; ++tz)
      for (let tx = 0; tx < TILES; ++tx) {
        this.pushMatrix();
        this.translate(tx * TILE, 0, tz * TILE);
        this.placeChild('Terrain', { tx: tx, tz: tz });
        this.popMatrix();
      }

    // Shared placement idiom: ground-follow + yaw + uniform scale + sink.
    const put = (module, params, x, z, s, sinkY) => {
      this.pushMatrix();
      this.translate(x, heightAt(x, z) - sinkY, z);
      this.rotateY(r.range(0, Math.PI * 2));
      this.scale(s, s, s);
      this.placeChild(module, params);
      this.popMatrix();
    };

    for (let i = 0; i < ROCKS; ++i) {
      const x = r.range(0, W), z = r.range(0, W);
      const s = r.range(0.6, 1.8);
      put('Rock', { seed: r.int(ROCK_VARIANTS) }, x, z, s, 0.15 * s);  // sink ~15%
    }
    for (let i = 0; i < PEBBLES; ++i) {
      const x = r.range(0, W), z = r.range(0, W);
      put('Pebble', { seed: r.int(PEBBLE_VARIANTS) }, x, z, r.range(0.5, 1.5), 0.02);
    }

    // Grass: density thinned on steep slopes (root skirt hides the sink).
    let placed = 0, guard = 0;
    while (placed < GRASS_CLUMPS && guard < GRASS_CLUMPS * 4) {
      ++guard;
      const x = r.range(0, W), z = r.range(0, W);
      if (slopeAt(x, z) > GRASS_SLOPE_MAX && r.random() < 0.7) continue;
      put('Grass', { seed: r.int(GRASS_VARIANTS) }, x, z, r.range(0.8, 1.3), 0.02);
      ++placed;
    }

    // Oaks: min-distance rejection sampling, planted at ground height.
    const oaks = [];
    let tguard = 0;
    while (oaks.length < TREES && tguard < TREES * 50) {
      ++tguard;
      const x = r.range(TILE, W - TILE), z = r.range(TILE, W - TILE);
      let ok = true;
      for (let i = 0; i < oaks.length; ++i) {
        const dx = x - oaks[i][0], dz = z - oaks[i][1];
        if (dx * dx + dz * dz < TREE_MIN_DIST * TREE_MIN_DIST) { ok = false; break; }
      }
      if (!ok) continue;
      oaks.push([x, z]);
      this.pushMatrix();
      this.translate(x, heightAt(x, z), z);
      this.rotateY(r.range(0, Math.PI * 2));
      this.placeChild('Tree');
      this.popMatrix();
    }
  }
}
```

**Note:** the child-count test expects EXACTLY 44896, which requires the grass and tree rejection loops to hit their target counts before the guard trips. If `meadow_bake_check` reports fewer, raise the guard multipliers (`* 4`, `* 50`) — do not lower the constants.

- [ ] **Step 4: Run test to verify it passes**

Run: `cd MatterEngine3/tests && make run-meadow-check` (first run is the expensive one — expect minutes: 256 tile bakes + voxel rocks/pebbles + the Tree)
Expected: `ALL PASS`, `children=44896`, `unique variants=276`, warm re-install bakes nothing.

- [ ] **Step 5: Run it AGAIN to confirm warm-cache behavior end to end**

Run: `cd MatterEngine3/tests && ./meadow_bake_check`
Expected: `[install] ... baked 0 artifact(s), 276 hit(s)` (or 277 counting the root) and `ALL PASS` in seconds.

- [ ] **Step 6: Commit**

```bash
git add MatterEngine3/examples/world_demo/schemas/Meadow.js \
        MatterEngine3/tests/meadow_bake_check.cpp MatterEngine3/tests/Makefile
git commit -m "feat: Meadow assembly world scattering 45k children in pure JS"
```

---

### Task 5: `expand` flag in read_manifest

**Files:**
- Modify: `MatterEngine3/include/part_graph.h:99-103` (read_manifest declaration)
- Modify: `MatterEngine3/src/part_graph.cpp:207-226` (read_manifest body)
- Test: `MatterEngine3/tests/part_graph_tests.cpp` (append a test)

**Interfaces:**
- Produces: `PartGraph::read_manifest(world_data_dir, world, roots_out, error_out, std::vector<bool>* expand_out = nullptr)` — parallel per-root expand flags; unknown flag tokens hard-error. Existing 4-arg callers compile unchanged.

- [ ] **Step 1: Write the failing test — append to `part_graph_tests.cpp`**

Add `#include <fstream>` to the includes, then add this test function and call it from `main()` alongside the existing tests:

```cpp
static void test_read_manifest_expand_flag() {
    system("mkdir -p /tmp/me3_manifest_test/W1");
    {
        std::ofstream f("/tmp/me3_manifest_test/W1/world.manifest");
        f << "# comment line\n"
          << "Tree\n"
          << "Meadow expand\n"
          << "\n";
    }
    std::vector<ChildRequest> roots;
    std::vector<bool> flags;
    std::string err;
    bool ok = PartGraph::read_manifest("/tmp/me3_manifest_test", "W1", roots, err, &flags);
    CHECK(ok, "manifest with expand flag parses");
    CHECK(roots.size() == 2 && flags.size() == 2, "two roots with parallel flags");
    CHECK(roots.size() == 2 && roots[0].module == "Tree" && !flags[0],
          "unflagged root -> expand=false");
    CHECK(roots.size() == 2 && roots[1].module == "Meadow" && flags[1],
          "expand flag parsed for flagged root");

    // 4-arg form still works (flags optional).
    std::vector<ChildRequest> r2;
    CHECK(PartGraph::read_manifest("/tmp/me3_manifest_test", "W1", r2, err) && r2.size() == 2,
          "read_manifest without expand_out still parses");

    // Unknown flag tokens are a hard error (fail-closed).
    {
        std::ofstream f("/tmp/me3_manifest_test/W1/world.manifest");
        f << "Meadow explode\n";
    }
    std::vector<ChildRequest> r3;
    CHECK(!PartGraph::read_manifest("/tmp/me3_manifest_test", "W1", r3, err),
          "unknown manifest flag rejected");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd MatterEngine3/tests && make run-graph`
Expected: compile error — read_manifest takes 4 args, test passes 5.

- [ ] **Step 3: Implement the flag**

In `part_graph.h`, replace the read_manifest declaration (keep the existing comment, extend it):

```cpp
    // Parse WorldData/<world>/world.manifest into root ChildRequests. Each line:
    // "<Module> [expand]"; '#' starts a comment. Roots take their `static params`
    // defaults (empty Params here). If expand_out is non-null it receives one flag
    // per root (parallel to roots_out): `expand` marks an assembly root whose baked
    // child-instance table the provider promotes to individual world instances.
    // Unknown flag tokens hard-error. Returns false + error on missing manifest.
    static bool read_manifest(const std::string& world_data_dir, const std::string& world,
                              std::vector<ChildRequest>& roots_out, std::string& error_out,
                              std::vector<bool>* expand_out = nullptr);
```

In `part_graph.cpp`, ensure `#include <sstream>` is present in the top (always-compiled) include block, and replace the read_manifest body:

```cpp
bool PartGraph::read_manifest(const std::string& world_data_dir, const std::string& world,
                              std::vector<ChildRequest>& roots_out, std::string& error_out,
                              std::vector<bool>* expand_out) {
    std::string path = world_data_dir + "/" + world + "/world.manifest";
    std::ifstream in(path);
    if (!in) {
        error_out = "world manifest not found: " + path;
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        // trim leading/trailing whitespace
        size_t b = line.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) continue;        // blank
        size_t e = line.find_last_not_of(" \t\r\n");
        std::string trimmed = line.substr(b, e - b + 1);
        if (trimmed.empty() || trimmed[0] == '#') continue; // comment
        std::istringstream tokens(trimmed);
        std::string name, flag;
        tokens >> name;
        bool expand = false;
        while (tokens >> flag) {
            if (flag == "expand") expand = true;
            else {
                error_out = "unknown manifest flag '" + flag + "' for root " + name;
                return false;
            }
        }
        roots_out.push_back(ChildRequest{ name, Params{} });
        if (expand_out) expand_out->push_back(expand);
    }
    return true;
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd MatterEngine3/tests && make run-graph`
Expected: PASS including the new manifest checks.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/include/part_graph.h MatterEngine3/src/part_graph.cpp \
        MatterEngine3/tests/part_graph_tests.cpp
git commit -m "feat: per-root expand flag in world manifests"
```

---

### Task 6: Generic root placement + child-table expansion in LocalProvider

**Files:**
- Modify: `MatterEngine3/viewer/local_provider.h` (declare `append_expanded_children`)
- Modify: `MatterEngine3/viewer/local_provider.cpp:111-196` (generic placement, expansion)
- Modify: `MatterEngine3/examples/world_demo/WorldData/Demo/world.manifest` (trim to Tree)
- Test: `MatterEngine3/tests/viewer_logic_tests.cpp` (append a test)

**Interfaces:**
- Consumes: Task 5's `read_manifest(..., &expand_flags)`; `part_asset::load_v2` / `cache_path_resolved`.
- Produces: `bool viewer::append_expanded_children(const std::string& cache_root, uint64_t root_hash, uint32_t& next_id, std::vector<WorldManifestEntry>& out_instances, std::string& err)` — free function in namespace `viewer`, declared in local_provider.h. `connect()` places every unflagged manifest root at the origin and expands flagged roots; the `world_name` special cases are deleted.

- [ ] **Step 1: Write the failing test — append to `viewer_logic_tests.cpp`** (add the function and call it from `main()`)

```cpp
static void test_append_expanded_children() {
    const std::string root = "/tmp/me3_expand_test";
    system(("rm -rf " + root).c_str());
    system(("mkdir -p " + root + "/parts").c_str());

    // Synthetic assembly root: no geometry, two children with distinct transforms.
    BLASManager blas; TLASManager tlas(256);
    part_asset::ChildInstance kids[2] = {};
    kids[0].child_resolved_hash = 0x1111;
    kids[1].child_resolved_hash = 0x2222;
    for (int k = 0; k < 2; ++k) {
        kids[k].transform[0] = kids[k].transform[5] =
        kids[k].transform[10] = kids[k].transform[15] = 1.0f;
        kids[k].transform[3] = 10.0f * (k + 1);   // translate-x 10 / 20
    }
    const uint64_t root_hash = 0xABCDEFull;
    part_asset::LodLevels no_lods;
    CHECK(part_asset::save_v2(root + "/" + part_asset::cache_path_resolved(root_hash),
                              blas, tlas, kids, 2, no_lods, root_hash),
          "synthetic assembly root saved");

    uint32_t next_id = 7;
    std::vector<viewer::WorldManifestEntry> out;
    std::string err;
    CHECK(viewer::append_expanded_children(root, root_hash, next_id, out, err),
          "expansion succeeds");
    CHECK(out.size() == 2, "one world instance per child");
    CHECK(out.size() == 2 && out[0].part_hash == 0x1111 && out[1].part_hash == 0x2222,
          "child hashes preserved");
    CHECK(out.size() == 2 && out[0].transform[3] == 10.0f && out[1].transform[3] == 20.0f,
          "child transforms preserved");
    CHECK(out.size() == 2 && out[0].instance_id == 7 && out[1].instance_id == 8 && next_id == 9,
          "instance ids advance");

    // Missing root artifact is a hard error.
    std::vector<viewer::WorldManifestEntry> out2;
    CHECK(!viewer::append_expanded_children(root, 0xDEADull, next_id, out2, err),
          "missing root part fails closed");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd MatterEngine3/tests && make run-viewer-logic`
Expected: compile error — `viewer::append_expanded_children` undeclared.

- [ ] **Step 3: Implement expansion + generic placement**

In `local_provider.h`, after the `LocalProvider` class (inside namespace `viewer`):

```cpp
// Expand an assembly root's baked child-instance table (from its .part in
// cache_root) into individual world-manifest instances — one per child, with
// the child's stored transform. Generic: used for any manifest root flagged
// `expand`. Fails closed if the root artifact is missing or has no children.
bool append_expanded_children(const std::string& cache_root, uint64_t root_hash,
                              uint32_t& next_id,
                              std::vector<WorldManifestEntry>& out_instances,
                              std::string& err);
```

In `local_provider.cpp` add `#include <cstring>` (for `std::memcpy`) and the implementation (bottom of the file, inside namespace `viewer`):

```cpp
bool append_expanded_children(const std::string& cache_root, uint64_t root_hash,
                              uint32_t& next_id,
                              std::vector<WorldManifestEntry>& out_instances,
                              std::string& err) {
    const std::string path = cache_root + "/" + part_asset::cache_path_resolved(root_hash);
    BLASManager blas; TLASManager tlas(256);
    std::vector<part_asset::ChildInstance> children;
    part_asset::LodLevels lods;
    if (!part_asset::load_v2(path, root_hash, blas, tlas, children, lods)) {
        err = "expand: failed to load root part " + path;
        return false;
    }
    if (children.empty()) {
        err = "expand: root has no children (nothing to expand)";
        return false;
    }
    out_instances.reserve(out_instances.size() + children.size());
    for (const auto& c : children) {
        WorldManifestEntry e;
        e.instance_id = next_id++;
        e.part_hash   = c.child_resolved_hash;
        std::memcpy(e.transform, c.transform, sizeof(e.transform));
        out_instances.push_back(e);
    }
    return true;
}
```

In `connect()`:
1. Change the manifest read to capture flags:
```cpp
    std::vector<ChildRequest> roots;
    std::vector<bool> expand_flags;
    bool manifest_ok = PartGraph::read_manifest(abs_world_data, cfg_.world_name,
                                                roots, err, &expand_flags);
```
2. Delete the `hash_of` map (local_provider.cpp:139-141) and its comment; keep the `ir.root_hashes.size() != roots.size()` guard.
3. Replace everything from the `// The Primitives world ...` comment through the final `return true;` (local_provider.cpp:182-196) with:

```cpp
    // Generic placement: every manifest root is placed at the origin, except
    // roots flagged `expand`, whose baked child-instance table is promoted to
    // individual world instances (per-child LOD, culling, and instanced
    // batching downstream). No per-world special cases.
    for (size_t i = 0; i < roots.size(); ++i) {
        if (expand_flags[i]) {
            if (!append_expanded_children(abs_cache_root, ir.root_hashes[i],
                                          next_id, out.instances, err))
                return false;
        } else {
            place(ir.root_hashes[i], 0.0f, 0.0f, 0.0f);
        }
    }
    flatten_placed();
    return true;
```

- [ ] **Step 4: Trim the Demo manifest** (generic placement would otherwise place Terrain and Grass at the origin too — the old code placed only the Tree)

Replace `MatterEngine3/examples/world_demo/WorldData/Demo/world.manifest` with:

```
# Demo world: a single Tree at the origin for close-up iteration on the tree
# geometry. (The Meadow world scatters Terrain/Grass/etc. via its assembly root.)
Tree
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cd MatterEngine3/tests && make run-viewer-logic`
Expected: PASS — new expansion checks pass AND the existing LocalProvider tests still pass (Tree hash unchanged, so warm caches stay warm; baked_count assertions are cold>0/warm==0 and survive the manifest trim).

- [ ] **Step 6: Regression — Primitives path**

Run: `cd MatterEngine3/tests && make run-gallery`
Expected: PASS (Gallery is an unflagged root → placed at origin by the generic loop, same as before).

- [ ] **Step 7: Commit**

```bash
git add MatterEngine3/viewer/local_provider.h MatterEngine3/viewer/local_provider.cpp \
        MatterEngine3/examples/world_demo/WorldData/Demo/world.manifest \
        MatterEngine3/tests/viewer_logic_tests.cpp
git commit -m "feat: generic manifest-root placement with expand-flag child promotion"
```

---

### Task 7: Projected-size floor cull in lod_select + SectorLodResolver

**Files:**
- Modify: `MatterEngine3/include/lod_select.h:31-33`
- Modify: `MatterEngine3/src/lod_select.cpp:27-50`
- Modify: `MatterEngine3/viewer/sector_resolver.h:41-53`
- Modify: `MatterEngine3/viewer/resolvers.cpp:45,60-69`
- Test: `MatterEngine3/tests/composition_tests.cpp` and `MatterEngine3/tests/viewer_logic_tests.cpp` (append tests)

**Interfaces:**
- Produces: `select_sector_lods(sectors, parts, cam_pos, float min_projected_size = 0.0f)` — parts whose projected size falls below the floor get level `-1` (culled). `SectorLodResolver::set_min_projected_size(float)` — resolver passes the floor through and skips level `-1` instances. Default `0.0f` preserves existing behavior everywhere.

- [ ] **Step 1: Write the failing lod_select test — append to `composition_tests.cpp`** (add function + call in `main()`)

```cpp
static void test_floor_cull_lod_select() {
    auto mk = [](uint64_t h, float x) {
        world_flatten::FlatInstance f;
        for (int i = 0; i < 16; ++i) f.world.cell[i] = 0.0f;
        f.world.cell[0] = f.world.cell[5] = f.world.cell[10] = f.world.cell[15] = 1.0f;
        f.world.cell[3] = x;
        f.resolved_hash = h;
        return f;
    };
    // 0xA: tiny part far away (0.05/100 = 0.0005 < floor)   -> culled (-1)
    // 0xB: large part, same far sector (10/100 = 0.1)        -> level 0
    // 0xC: tiny part near the camera (0.05/4 = 0.0125)       -> level 0
    std::vector<world_flatten::FlatInstance> flat = {
        mk(0xA, 100.0f), mk(0xB, 100.0f), mk(0xC, 4.0f)
    };
    sector_grid::SectorGrid grid(16.0f);
    sector_grid::Sectors sectors = sector_grid::bin_instances(flat, grid);
    lod_select::PartLodTable parts;
    parts[0xA] = { 0.05f, {0.0f} };
    parts[0xB] = { 10.0f, {0.0f} };
    parts[0xC] = { 0.05f, {0.0f} };
    float3 cam = make_float3(0, 0, 0);

    auto chosen = lod_select::select_sector_lods(sectors, parts, cam, 0.002f);
    int lodA = 99, lodB = 99, lodC = 99;
    for (const auto& sk : chosen)
        for (const auto& pl : sk.second) {
            if (pl.first == 0xA) lodA = pl.second;
            if (pl.first == 0xB) lodB = pl.second;
            if (pl.first == 0xC) lodC = pl.second;
        }
    CHECK(lodA == -1, "small far part floor-culled (level -1)");
    CHECK(lodB == 0,  "large far part not culled");
    CHECK(lodC == 0,  "small near part not culled");

    // Default arg (no floor) never emits -1: existing behavior preserved.
    auto chosen0 = lod_select::select_sector_lods(sectors, parts, cam);
    bool any_cull = false;
    for (const auto& sk : chosen0)
        for (const auto& pl : sk.second) if (pl.second < 0) any_cull = true;
    CHECK(!any_cull, "zero floor culls nothing (back-compat)");
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd MatterEngine3/tests && make run-comp`
Expected: compile error — select_sector_lods takes 3 args.

- [ ] **Step 3: Implement the floor in lod_select**

`lod_select.h` — replace the select_sector_lods comment + declaration:

```cpp
// For each sector, find its CLOSEST instance to cam_pos, and for every distinct
// part hash present in the sector compute its chosen LOD level using the closest
// instance's distance. Parts whose projected size falls below min_projected_size
// get level -1 ("floor-culled": too small to matter — resolvers emit nothing for
// them). The 0.0f default disables the floor. Returns
// sector -> (part hash -> chosen level index, or -1).
std::map<sector_grid::SectorCoord, std::map<uint64_t,int>>
select_sector_lods(const sector_grid::Sectors& sectors,
                   const PartLodTable& parts, const float3& cam_pos,
                   float min_projected_size = 0.0f);
```

`lod_select.cpp` — matching signature, and replace the inner per-part loop body:

```cpp
        for (const auto& f : insts) {
            auto pit = parts.find(f.resolved_hash);
            if (pit == parts.end()) continue;
            float size = pit->second.bound_radius / closest;
            out[coord][f.resolved_hash] =
                (size < min_projected_size) ? -1
                                            : select_level(size, pit->second.thresholds);
        }
```

- [ ] **Step 4: Run composition tests**

Run: `cd MatterEngine3/tests && make run-comp`
Expected: PASS (new floor checks + all existing SP-4 tests via the 0.0f default).

- [ ] **Step 5: Write the failing resolver test — append to `viewer_logic_tests.cpp`** (add function + call in `main()`)

```cpp
static void test_sector_lod_floor_cull() {
    viewer::WorldState state;
    viewer::WorldManifest m;
    m.world_root_hash = 1;
    m.instances.push_back(mk_entry(1, 0xF00D, 200.0f));   // 200 units from origin
    state.reset(m);
    lod_select::PartLodTable lods;
    lods[0xF00D] = { 0.5f, {0.0f} };    // projected size at 200u = 0.0025
    viewer::SectorLodResolver sec(16.0f, 400.0f);
    float3 cam = make_float3(0, 0, 0);

    auto r0 = sec.resolve(state, lods, cam);
    CHECK(r0.size() == 1, "no floor: instance emitted");

    sec.set_min_projected_size(0.01f);   // 0.0025 < 0.01 -> culled
    auto r1 = sec.resolve(state, lods, cam);
    CHECK(r1.empty(), "floor cull drops sub-threshold instance");

    sec.set_min_projected_size(0.001f);  // 0.0025 > 0.001 -> visible again
    auto r2 = sec.resolve(state, lods, cam);
    CHECK(r2.size() == 1, "instance returns below-floor -> above-floor");
}
```

- [ ] **Step 6: Run to verify it fails**

Run: `cd MatterEngine3/tests && make run-viewer-logic`
Expected: compile error — `set_min_projected_size` undeclared.

- [ ] **Step 7: Implement the resolver pass-through**

`sector_resolver.h` — inside `SectorLodResolver`, add next to `set_active_radius`:

```cpp
    void set_min_projected_size(float v) { min_projected_size_ = v; }
```

and a private member:

```cpp
    float min_projected_size_ = 0.0f;
```

`resolvers.cpp` — pass the floor through (line 45):

```cpp
    auto chosen = lod_select::select_sector_lods(sectors, lods, cam_pos, min_projected_size_);
```

and in the emit loop, right after `lod` is looked up (line 63):

```cpp
            if (lod < 0) continue;   // floor-culled: projected size below the min
```

- [ ] **Step 8: Run tests to verify they pass**

Run: `cd MatterEngine3/tests && make run-viewer-logic && make run-comp`
Expected: PASS.

- [ ] **Step 9: Commit**

```bash
git add MatterEngine3/include/lod_select.h MatterEngine3/src/lod_select.cpp \
        MatterEngine3/viewer/sector_resolver.h MatterEngine3/viewer/resolvers.cpp \
        MatterEngine3/tests/composition_tests.cpp MatterEngine3/tests/viewer_logic_tests.cpp
git commit -m "feat: projected-size floor cull in lod_select + SectorLodResolver"
```

---

### Task 8: Meadow world wiring, benchmark capture, docs, Windows build

**Files:**
- Create: `MatterEngine3/examples/world_demo/WorldData/Meadow/world.manifest`
- Modify: `MatterEngine3/viewer/main.cpp:70-75` (world switch) and `:132-133` (resolver config)
- Modify: `MatterEngine3/docs/rendering.md` (root expansion, floor cull, benchmark numbers)

**Interfaces:**
- Consumes: everything above.
- Produces: `MATTER_WORLD=meadow` viewer mode; the recorded raster baseline for Phase 3.

- [ ] **Step 1: Create the Meadow world manifest**

`MatterEngine3/examples/world_demo/WorldData/Meadow/world.manifest`:

```
# Meadow density world (showcase + raster benchmark). One assembly root; the
# `expand` flag promotes its baked child-instance table (~45k placements) to
# individual world instances, so per-child LOD selection, flattening, floor
# cull, and instanced raster batching all apply.
Meadow expand
```

- [ ] **Step 2: Wire the viewer**

In `main.cpp`, extend the world switch (after the `primitives` block, ~line 75):

```cpp
    // MATTER_WORLD=meadow loads the dense meadow benchmark world (same
    // world_demo schemas; the Meadow manifest root carries the expand flag).
    const bool meadow = world_env && std::string(world_env) == "meadow";
    if (meadow) cfg.world_name = "Meadow";
```

Replace the resolver construction (~line 132-133):

```cpp
    PassThroughResolver pass;
    // Per-world resolver config: the Meadow spans ~256x256 units, so activate
    // sectors across the whole world and floor-cull sub-pixel parts (grass/
    // pebbles self-cull at distance; their epsilon ladders stop well above 1 px).
    const float kActiveRadius     = meadow ? 400.0f : 64.0f;
    const float kMinProjectedSize = meadow ? 0.0015f : 0.0f;   // ~1 px at 720p (fov/height)
    SectorLodResolver sec(16.0f, kActiveRadius);
    sec.set_min_projected_size(kMinProjectedSize);
    if (meadow) stats.resolver_choice = 1;   // SectorLod by default for the benchmark
```

- [ ] **Step 3: Build the Linux viewer**

Run: `cd MatterEngine3/viewer && make`
Expected: clean build.

- [ ] **Step 4: First Meadow run + benchmark capture (headless)**

Run (from `MatterEngine3/viewer/`; the first run bakes 276 variants and flattens each — expect several minutes; warm runs are seconds):

```bash
MATTER_WORLD=meadow MATTER_CAM="128,25,40,128,2,128" \
  MATTER_SCREENSHOT=/tmp/meadow_default_cam.png ./viewer
```

Expected: screenshot written; console prints connect stats (`instances_total` ≈ 44896). View `/tmp/meadow_default_cam.png` (Read tool) — rolling green meadow, rocks, grass, oaks; HUD shows `Raster: N batches / M tris`.

- [ ] **Step 5: Record the benchmark numbers**

Read the HUD values off the screenshot (or add a temporary printf). Success criterion: **~5–8M tris** at the default camera. If outside the band, tune ONLY named constants (`GRASS_CLUMPS`/`BLADES` up-down, `kMinProjectedSize`) and re-run; note the final constants. Then wipe the viewer cache once (`rm -rf MatterEngine3/viewer/cache/parts`) and re-run to confirm a clean cold bake also works end to end.

- [ ] **Step 6: Update docs**

In `MatterEngine3/docs/rendering.md`:
1. In the raster-path section, after the per-frame composition description, add:

```markdown
### Root expansion (`expand` manifest flag)

A world-manifest root may carry the `expand` flag (`Meadow expand`). The
provider then does NOT place the root itself; after install it reads the root's
baked child-instance table and emits one world instance per child. Children
thereby become root instances: SectorLod selection, bake-time flattening (one
flat artifact per unique child hash), floor cull, and instanced raster batching
all apply per child. Unflagged roots place at the origin and flatten whole.

### Projected-size floor cull

`select_sector_lods` takes a `min_projected_size` (default 0 = off). Parts
whose projected size (`bound_radius / distance`) in a sector falls below the
floor are assigned level -1 and the resolver emits nothing for them — small
parts (grass, pebbles) self-cull at distance even though their error-bounded
LOD ladders stop above 1 px. The viewer enables this per world
(Meadow: 0.0015 ≈ 1 px at 720p, active radius 400).

### Meadow benchmark (Phase 3 raster baseline)

`MATTER_WORLD=meadow`, default camera `MATTER_CAM="128,25,40,128,2,128"`,
1280×720: <N> batches / <M> tris, <F> FPS / <ms> frame ms
(recorded YYYY-MM-DD, commit <sha>). Scatter constants in Meadow.js.
```

2. Fill in the real measured numbers from Step 5 (no placeholders left in the committed doc).

- [ ] **Step 7: Full regression sweep**

Run: `cd MatterEngine3/tests && make run-meadow && make run-meadow-check && make run-treebake && make run-viewer-logic && make run-comp && make run-graph && make run-gallery && make run-partv2 && make run-script`
Expected: all pass (pre-existing failures listed in Global Constraints excepted).

- [ ] **Step 8: Windows rebuild** (headers changed → clean the obj dir first; a stale viewer.exe silently ships the old engine)

Run: `cd MatterEngine3/viewer && rm -rf build/windows && make windows`
Expected: `viewer.exe` relinks cleanly. (If the link fails with "Permission denied", viewer.exe is open on the Windows side — ask Jack to close it, then relink.)

- [ ] **Step 9: Commit**

```bash
git add MatterEngine3/examples/world_demo/WorldData/Meadow/world.manifest \
        MatterEngine3/viewer/main.cpp MatterEngine3/docs/rendering.md
git commit -m "feat: Meadow density world wiring, floor-cull config, raster benchmark baseline"
```

---

## Verification (whole plan)

1. `cd MatterEngine3/tests && make run-meadow && make run-meadow-check` — schema + world bake suites green; re-runs are 100% cache hits.
2. `make run-graph && make run-viewer-logic && make run-comp && make run-treebake && make run-gallery` — engine changes regression-free.
3. Headless Meadow screenshot at the default camera; HUD reads ~5–8M tris; numbers + constants recorded in docs/rendering.md.
4. `MATTER_WORLD=meadow MATTER_RT=1` still functions on the same manifest (no perf target; large TLAS expected).
5. Jack launches the Windows viewer.exe for an interactive fly-through (LOD switches, floor cull pop-in acceptable at ~1 px).
