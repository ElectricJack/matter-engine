# Occupancy-Based Interior Particle Culling — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a pre-pass that drops buried lattice particles before they reach the (unchanged) meshing pipeline, cutting mesh-build work while producing visibly identical geometry.

**Architecture:** Three new, self-contained MatterSurfaceLib modules — `lattice` (slot→position + neighbor topology), `occupancy` (which slots are filled), `particle_culling` (keep shell + sub-shell margin, drop deep interior, emit jittered/tinted particles). Scene setup builds a lattice + occupancy, runs the culling pass, and feeds the result into `Cluster::add_particle` exactly as today. No meshing/SDF/BLAS code changes.

**Tech Stack:** C++17, g++, raylib (only for the `Vector3`/`Vector4` POD structs), existing MatterSurfaceLib headless test harness, WSLg headless capture.

---

## Background for the implementer (read once)

- This is a sub-project of MatterEngine2 (`/mnt/d/Shared With Desktop/AI/matter-engine-cpp`), in the `MatterSurfaceLib/` directory.
- Particles are placed into a `Cluster`, spatial-hashed into 16³ `Cell`s, and meshed via `Cell::generate_mesh_for_group` → `GenerateMesh` (marching cubes over a metaball SDF). **Do not touch any of that.** Your job is only to reduce how many particles get added.
- `Cluster::add_particle(const Vector3& pos, float radius, uint32_t material_id, const Vector4& tint)` is the integration point (see `MatterSurfaceLib/include/cluster.h:49`).
- `Vector3`/`Vector4` are plain structs from raylib.h (`{float x,y,z;}` / `{float x,y,z,w;}`). Use plain arithmetic; do not pull in raymath.
- **Why a margin?** Marching cubes only emits a surface where the SDF crosses its isolevel — near the outer shell. If we kept *only* the outermost slots, the field would go hollow just beneath the surface and the mesher would emit an unwanted *inner* surface. Keeping the shell **plus `margin` sub-shell layers** keeps the field solid deep enough that no interior surface appears. The acceptance test (Task 4) tunes `margin` to the smallest safe value.
- Build commands (run from `MatterSurfaceLib/`):
  - Headless unit tests: `make -C tests particle_culling_tests && tests/particle_culling_tests`
  - Linux app for capture (WSL default is a Windows cross-build, so this flag is required): `WSL_LINUX=1 make`
- Existing headless tests live in `MatterSurfaceLib/tests/` and use a tiny `CHECK(cond,msg)` macro with a `failures` counter and a `main()` returning `failures==0?0:1` (see `tests/blas_tint_tests.cpp` for the exact pattern to copy).

## File Structure

- `MatterSurfaceLib/include/lattice.h` / `src/lattice.cpp` — `SlotCoord`, `Lattice` interface, `GridLattice`.
- `MatterSurfaceLib/include/occupancy.h` / `src/occupancy.cpp` — `SlotData`, `pack_slot`, `Occupancy`.
- `MatterSurfaceLib/include/particle_culling.h` / `src/particle_culling.cpp` — deterministic noise (moved from main.cpp), `EmittedParticle`, `CullParams`, `slot_is_buried`, `cull_interior`, `emit_all`.
- `MatterSurfaceLib/tests/particle_culling_tests.cpp` — one headless test binary covering all three modules.
- Modify `MatterSurfaceLib/Makefile`, `MatterSurfaceLib/tests/Makefile`, `build-all.sh`, `MatterSurfaceLib/main.cpp`.

---

## Task 1: GridLattice

**Files:**
- Create: `MatterSurfaceLib/include/lattice.h`
- Create: `MatterSurfaceLib/src/lattice.cpp`
- Create: `MatterSurfaceLib/tests/particle_culling_tests.cpp`
- Modify: `MatterSurfaceLib/tests/Makefile`

- [ ] **Step 1: Create the lattice header**

Create `MatterSurfaceLib/include/lattice.h`:

```cpp
#pragma once

#include "raylib.h"
#include <vector>

// Integer coordinate of a lattice slot.
struct SlotCoord { int x, y, z; };

// A lattice maps integer slot coordinates to local-space positions and knows
// its neighbor topology (used for shell detection). Only GridLattice ships now;
// hex/diamond lattices become new implementations of this interface later.
class Lattice {
public:
    virtual ~Lattice() = default;
    // Base (un-jittered) local-space center of a slot.
    virtual Vector3 slot_position(SlotCoord c) const = 0;
    // Adjacency offsets defining a slot's immediate neighbors.
    virtual const std::vector<SlotCoord>& neighbor_offsets() const = 0;
};

// Regular cubic grid: slot c sits at c * spacing; 6-connected (face neighbors).
class GridLattice : public Lattice {
public:
    explicit GridLattice(float spacing);
    Vector3 slot_position(SlotCoord c) const override;
    const std::vector<SlotCoord>& neighbor_offsets() const override;
    float spacing() const { return spacing_; }
private:
    float spacing_;
    std::vector<SlotCoord> neighbors_;
};
```

- [ ] **Step 2: Create the lattice implementation**

Create `MatterSurfaceLib/src/lattice.cpp`:

```cpp
#include "../include/lattice.h"

GridLattice::GridLattice(float spacing)
    : spacing_(spacing),
      neighbors_{ {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} } {}

Vector3 GridLattice::slot_position(SlotCoord c) const {
    return Vector3{ c.x * spacing_, c.y * spacing_, c.z * spacing_ };
}

const std::vector<SlotCoord>& GridLattice::neighbor_offsets() const {
    return neighbors_;
}
```

- [ ] **Step 3: Create the test file with lattice tests**

Create `MatterSurfaceLib/tests/particle_culling_tests.cpp`:

```cpp
#include "../include/lattice.h"
#include <cstdio>
#include <cmath>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)

static void test_grid_lattice() {
    GridLattice lat(2.0f);
    Vector3 p = lat.slot_position(SlotCoord{3, 0, -1});
    CHECK(fabsf(p.x - 6.0f) < 1e-6f, "grid slot_position x scales by spacing");
    CHECK(fabsf(p.y - 0.0f) < 1e-6f, "grid slot_position y scales by spacing");
    CHECK(fabsf(p.z + 2.0f) < 1e-6f, "grid slot_position z scales by spacing");

    const auto& n = lat.neighbor_offsets();
    CHECK(n.size() == 6, "grid has 6 face neighbors");
    bool has_plus_x = false;
    for (const auto& o : n) if (o.x == 1 && o.y == 0 && o.z == 0) has_plus_x = true;
    CHECK(has_plus_x, "grid neighbors include +x");
    CHECK(fabsf(lat.spacing() - 2.0f) < 1e-6f, "grid reports its spacing");
}

int main() {
    test_grid_lattice();
    if (failures == 0) printf("All particle_culling tests passed\n");
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 4: Add the test target to the tests Makefile**

In `MatterSurfaceLib/tests/Makefile`, append after the `run-tint` target (end of file):

```makefile

# Occupancy-based interior culling unit tests (headless, no GL window)
CULL_TARGET = particle_culling_tests
CULL_SOURCES = particle_culling_tests.cpp ../src/lattice.cpp ../src/occupancy.cpp ../src/particle_culling.cpp

$(CULL_TARGET): $(CULL_SOURCES)
	$(CC) $(CULL_SOURCES) -o $(CULL_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)

run-cull: $(CULL_TARGET)
	./$(CULL_TARGET)
```

Also add `run-cull` to the `.PHONY` line (line 59) and `$(CULL_TARGET)` to the `clean` rule's `rm -f` list (line 62).

> Note: this target references `../src/occupancy.cpp` and `../src/particle_culling.cpp`, which are created in Tasks 2 and 3. It will not compile until then. To verify Task 1 in isolation, temporarily build with only the lattice source:
> `g++ tests/particle_culling_tests.cpp src/lattice.cpp -o /tmp/t -std=c++17 -Itests/../include -I../Libraries/raylib/src && /tmp/t`

- [ ] **Step 5: Verify the lattice test passes (isolated build)**

Run from `MatterSurfaceLib/`:
```bash
g++ tests/particle_culling_tests.cpp src/lattice.cpp -o /tmp/cull_t1 -std=c++17 -Iinclude -I../Libraries/raylib/src && /tmp/cull_t1
```
Expected: `All particle_culling tests passed`

- [ ] **Step 6: Commit**

```bash
git add MatterSurfaceLib/include/lattice.h MatterSurfaceLib/src/lattice.cpp \
        MatterSurfaceLib/tests/particle_culling_tests.cpp MatterSurfaceLib/tests/Makefile
git commit -m "feat: add GridLattice (slot positions + neighbor topology)"
```

---

## Task 2: Occupancy grid

**Files:**
- Create: `MatterSurfaceLib/include/occupancy.h`
- Create: `MatterSurfaceLib/src/occupancy.cpp`
- Modify: `MatterSurfaceLib/tests/particle_culling_tests.cpp`

- [ ] **Step 1: Create the occupancy header**

Create `MatterSurfaceLib/include/occupancy.h`:

```cpp
#pragma once

#include "lattice.h"
#include <cstdint>
#include <unordered_map>
#include <functional>

// Per-slot authoring data. Kept minimal; per-particle visual variation is
// derived deterministically from SlotCoord at emit time (see particle_culling).
struct SlotData { uint32_t materialId; };

// Pack a SlotCoord into a 64-bit key. Each axis is biased by +2^20 and stored
// in 21 bits, so coordinates in [-1048576, 1048575] per axis are representable
// -- far beyond any realistic part size.
uint64_t pack_slot(SlotCoord c);

// Sparse set of occupied slots. Sparse (hash map) so parts need not be dense
// axis-aligned blocks.
class Occupancy {
public:
    void set(SlotCoord c, const SlotData& d);   // mark slot occupied
    bool occupied(SlotCoord c) const;
    size_t count() const;
    void for_each(const std::function<void(SlotCoord, const SlotData&)>& fn) const;
private:
    std::unordered_map<uint64_t, SlotData> slots_;
};
```

- [ ] **Step 2: Create the occupancy implementation**

Create `MatterSurfaceLib/src/occupancy.cpp`:

```cpp
#include "../include/occupancy.h"

static constexpr int64_t SLOT_BIAS = 1 << 20;   // 1048576
static constexpr uint64_t SLOT_MASK = 0x1FFFFF; // 21 bits

uint64_t pack_slot(SlotCoord c) {
    uint64_t x = (uint64_t)(c.x + SLOT_BIAS) & SLOT_MASK;
    uint64_t y = (uint64_t)(c.y + SLOT_BIAS) & SLOT_MASK;
    uint64_t z = (uint64_t)(c.z + SLOT_BIAS) & SLOT_MASK;
    return (x << 42) | (y << 21) | z;
}

void Occupancy::set(SlotCoord c, const SlotData& d) { slots_[pack_slot(c)] = d; }

bool Occupancy::occupied(SlotCoord c) const {
    return slots_.find(pack_slot(c)) != slots_.end();
}

size_t Occupancy::count() const { return slots_.size(); }

void Occupancy::for_each(const std::function<void(SlotCoord, const SlotData&)>& fn) const {
    for (const auto& kv : slots_) {
        uint64_t k = kv.first;
        SlotCoord c;
        c.x = (int)((int64_t)((k >> 42) & SLOT_MASK) - SLOT_BIAS);
        c.y = (int)((int64_t)((k >> 21) & SLOT_MASK) - SLOT_BIAS);
        c.z = (int)((int64_t)( k        & SLOT_MASK) - SLOT_BIAS);
        fn(c, kv.second);
    }
}
```

- [ ] **Step 3: Add occupancy tests**

In `MatterSurfaceLib/tests/particle_culling_tests.cpp`, add `#include "../include/occupancy.h"` below the lattice include, add this function above `main()`:

```cpp
static void test_occupancy() {
    Occupancy occ;
    CHECK(occ.count() == 0, "empty occupancy has count 0");
    CHECK(!occ.occupied(SlotCoord{0,0,0}), "unset slot not occupied");

    occ.set(SlotCoord{2, -3, 5}, SlotData{7});
    occ.set(SlotCoord{2, -3, 5}, SlotData{7});  // idempotent
    CHECK(occ.count() == 1, "re-setting same slot does not grow count");
    CHECK(occ.occupied(SlotCoord{2, -3, 5}), "set slot is occupied");
    CHECK(!occ.occupied(SlotCoord{2, -3, 6}), "neighbor slot not occupied");

    // pack/unpack round-trips negatives via for_each.
    bool seen = false;
    occ.for_each([&](SlotCoord c, const SlotData& d) {
        if (c.x == 2 && c.y == -3 && c.z == 5 && d.materialId == 7) seen = true;
    });
    CHECK(seen, "for_each round-trips coords (incl. negatives) and data");
}
```

and call `test_occupancy();` as the second line of `main()` (after `test_grid_lattice();`).

- [ ] **Step 4: Verify lattice + occupancy tests pass (isolated build)**

Run from `MatterSurfaceLib/`:
```bash
g++ tests/particle_culling_tests.cpp src/lattice.cpp src/occupancy.cpp -o /tmp/cull_t2 -std=c++17 -Iinclude -I../Libraries/raylib/src && /tmp/cull_t2
```
Expected: `All particle_culling tests passed`

- [ ] **Step 5: Commit**

```bash
git add MatterSurfaceLib/include/occupancy.h MatterSurfaceLib/src/occupancy.cpp \
        MatterSurfaceLib/tests/particle_culling_tests.cpp
git commit -m "feat: add sparse Occupancy grid with packed slot keys"
```

---

## Task 3: Interior-culling pass

**Files:**
- Create: `MatterSurfaceLib/include/particle_culling.h`
- Create: `MatterSurfaceLib/src/particle_culling.cpp`
- Modify: `MatterSurfaceLib/tests/particle_culling_tests.cpp`

- [ ] **Step 1: Create the culling header**

Create `MatterSurfaceLib/include/particle_culling.h`:

```cpp
#pragma once

#include "lattice.h"
#include "occupancy.h"
#include <vector>
#include <cstdint>

// A particle ready to hand to Cluster::add_particle (jitter already applied).
struct EmittedParticle {
    Vector3 position;   // local-space
    float radius;
    uint32_t materialId;
    Vector4 tint;       // RGBA; w = blend strength
};

struct CullParams {
    int margin;          // sub-shell layers to keep; clamped to >= 1
    float base_radius;   // nominal particle radius
    float jitter_amount; // per-axis position jitter magnitude (0 = none)
    float tint_alpha;    // tint blend strength written to EmittedParticle.tint.w
    uint32_t seed;       // determinism seed for jitter/tint
};

// Deterministic value-noise primitives (moved from main.cpp). [0,1] output.
float lattice_vhash(int x, int y, int z);
float lattice_vnoise(float x, float y, float z);

// A slot is buried iff every slot in the Chebyshev box of half-width `margin`
// around it is occupied. Grid (6/26-connectivity) assumption; future lattices
// would expand neighbor_offsets via BFS instead.
bool slot_is_buried(const Occupancy& occ, SlotCoord c, int margin);

// Keep occupied slots that are NOT buried; emit a particle for each kept slot.
std::vector<EmittedParticle> cull_interior(const Lattice& lattice,
                                           const Occupancy& occ,
                                           const CullParams& p);

// Baseline: emit a particle for every occupied slot (no culling). Used by the
// A/B acceptance comparison.
std::vector<EmittedParticle> emit_all(const Lattice& lattice,
                                      const Occupancy& occ,
                                      const CullParams& p);
```

- [ ] **Step 2: Create the culling implementation**

Create `MatterSurfaceLib/src/particle_culling.cpp`:

```cpp
#include "../include/particle_culling.h"
#include <cmath>

float lattice_vhash(int x, int y, int z) {
    uint32_t h = ((uint32_t)x * 374761393u) ^ ((uint32_t)y * 668265263u) ^ ((uint32_t)z * 2147483647u);
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (float)(h & 0xFFFFFFu) / (float)0xFFFFFFu; // [0,1]
}

float lattice_vnoise(float x, float y, float z) {
    int xi = (int)floorf(x), yi = (int)floorf(y), zi = (int)floorf(z);
    float xf = x - xi, yf = y - yi, zf = z - zi;
    auto lerpf  = [](float a, float b, float t) { return a + (b - a) * t; };
    auto smooth = [](float t) { return t * t * (3.0f - 2.0f * t); };
    float u = smooth(xf), v = smooth(yf), w = smooth(zf);
    float c000 = lattice_vhash(xi, yi, zi),     c100 = lattice_vhash(xi+1, yi, zi);
    float c010 = lattice_vhash(xi, yi+1, zi),   c110 = lattice_vhash(xi+1, yi+1, zi);
    float c001 = lattice_vhash(xi, yi, zi+1),   c101 = lattice_vhash(xi+1, yi, zi+1);
    float c011 = lattice_vhash(xi, yi+1, zi+1), c111 = lattice_vhash(xi+1, yi+1, zi+1);
    float x00 = lerpf(c000, c100, u), x10 = lerpf(c010, c110, u);
    float x01 = lerpf(c001, c101, u), x11 = lerpf(c011, c111, u);
    return lerpf(lerpf(x00, x10, v), lerpf(x01, x11, v), w); // [0,1]
}

bool slot_is_buried(const Occupancy& occ, SlotCoord c, int margin) {
    for (int dz = -margin; dz <= margin; ++dz)
    for (int dy = -margin; dy <= margin; ++dy)
    for (int dx = -margin; dx <= margin; ++dx) {
        if (!occ.occupied(SlotCoord{c.x + dx, c.y + dy, c.z + dz})) return false;
    }
    return true;
}

// Build one emitted particle for a slot. Jitter and tint are pure functions of
// (SlotCoord, seed) so the same design always bakes identically.
static EmittedParticle make_particle(const Lattice& lat, SlotCoord c,
                                     const SlotData& d, const CullParams& p) {
    Vector3 base = lat.slot_position(c);
    int s = (int)p.seed;
    float jx = (lattice_vhash(c.x * 2 + 1 + s, c.y, c.z) - 0.5f) * p.jitter_amount;
    float jy = (lattice_vhash(c.x, c.y * 2 + 1 + s, c.z) - 0.5f) * p.jitter_amount;
    float jz = (lattice_vhash(c.x, c.y, c.z * 2 + 1 + s) - 0.5f) * p.jitter_amount;

    EmittedParticle ep;
    ep.position   = Vector3{ base.x + jx, base.y + jy, base.z + jz };
    ep.radius     = p.base_radius;
    ep.materialId = d.materialId;
    float tr = lattice_vhash(c.x + 101 + s, c.y, c.z);
    float tg = lattice_vhash(c.x, c.y + 101 + s, c.z);
    float tb = lattice_vhash(c.x, c.y, c.z + 101 + s);
    ep.tint = Vector4{ tr, tg, tb, p.tint_alpha };
    return ep;
}

std::vector<EmittedParticle> cull_interior(const Lattice& lattice,
                                           const Occupancy& occ,
                                           const CullParams& p) {
    int margin = p.margin < 1 ? 1 : p.margin;
    std::vector<EmittedParticle> out;
    occ.for_each([&](SlotCoord c, const SlotData& d) {
        if (!slot_is_buried(occ, c, margin)) out.push_back(make_particle(lattice, c, d, p));
    });
    return out;
}

std::vector<EmittedParticle> emit_all(const Lattice& lattice,
                                      const Occupancy& occ,
                                      const CullParams& p) {
    std::vector<EmittedParticle> out;
    occ.for_each([&](SlotCoord c, const SlotData& d) {
        out.push_back(make_particle(lattice, c, d, p));
    });
    return out;
}
```

- [ ] **Step 3: Add culling tests**

In `MatterSurfaceLib/tests/particle_culling_tests.cpp`, add `#include "../include/particle_culling.h"` below the occupancy include, add these helpers and tests above `main()`:

```cpp
// Fill an N x N x N solid block of one material at the origin.
static Occupancy solid_block(int N) {
    Occupancy occ;
    for (int x = 0; x < N; ++x)
    for (int y = 0; y < N; ++y)
    for (int z = 0; z < N; ++z)
        occ.set(SlotCoord{x, y, z}, SlotData{8});
    return occ;
}

static CullParams default_params(int margin) {
    CullParams p;
    p.margin = margin; p.base_radius = 0.4f;
    p.jitter_amount = 0.1f; p.tint_alpha = 0.2f; p.seed = 1337;
    return p;
}

static void test_burial() {
    Occupancy occ = solid_block(5);
    // Center slot is buried at margin 1 (all 26 neighbors occupied).
    CHECK(slot_is_buried(occ, SlotCoord{2,2,2}, 1), "center of 5^3 is buried at margin 1");
    // A face slot has an empty neighbor outside the block -> not buried.
    CHECK(!slot_is_buried(occ, SlotCoord{0,2,2}, 1), "face slot not buried");
    // At margin 2 even the center sees outside the block -> not buried.
    CHECK(!slot_is_buried(occ, SlotCoord{2,2,2}, 2), "center of 5^3 not buried at margin 2");
}

static void test_cull_counts() {
    GridLattice lat(0.8f);
    Occupancy occ = solid_block(5);

    // margin 1: buried = inner 3^3 = 27; kept = 125 - 27 = 98.
    auto m1 = cull_interior(lat, occ, default_params(1));
    CHECK(m1.size() == 98, "margin 1 keeps 98 of 125 (drops inner 3^3)");

    // margin 2: buried = inner 1^3 = 1; kept = 124.
    auto m2 = cull_interior(lat, occ, default_params(2));
    CHECK(m2.size() == 124, "margin 2 keeps 124 of 125 (drops inner 1^3)");

    // emit_all keeps everything.
    auto all = emit_all(lat, occ, default_params(1));
    CHECK(all.size() == 125, "emit_all keeps all 125");

    // margin < 1 is clamped to 1.
    auto m0 = cull_interior(lat, occ, default_params(0));
    CHECK(m0.size() == 98, "margin 0 clamped to 1");
}

static void test_thin_shape_keeps_all() {
    // A 5x5x1 wall: every slot has an empty neighbor in z, so none are buried.
    Occupancy occ;
    for (int x = 0; x < 5; ++x)
    for (int y = 0; y < 5; ++y)
        occ.set(SlotCoord{x, y, 0}, SlotData{8});
    GridLattice lat(0.8f);
    auto kept = cull_interior(lat, occ, default_params(2));
    CHECK(kept.size() == 25, "one-slot-thick wall keeps all slots at any margin");
}

static void test_determinism() {
    GridLattice lat(0.8f);
    Occupancy occ = solid_block(5);

    auto a = cull_interior(lat, occ, default_params(1));
    auto b = cull_interior(lat, occ, default_params(1));
    CHECK(a.size() == b.size(), "two identical culls produce equal counts");
    bool identical = (a.size() == b.size());
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
        if (a[i].position.x != b[i].position.x ||
            a[i].position.y != b[i].position.y ||
            a[i].position.z != b[i].position.z ||
            a[i].tint.x != b[i].tint.x) identical = false;
    }
    CHECK(identical, "two identical culls produce bit-identical particles");

    // Different seed: same kept set, but positions move.
    CullParams p2 = default_params(1); p2.seed = 9999;
    auto c = cull_interior(lat, occ, p2);
    CHECK(c.size() == a.size(), "different seed keeps the same slots");
    bool any_moved = false;
    for (size_t i = 0; i < a.size() && i < c.size(); ++i)
        if (a[i].position.x != c[i].position.x) any_moved = true;
    CHECK(any_moved, "different seed changes jittered positions");
}
```

and call all four (`test_burial(); test_cull_counts(); test_thin_shape_keeps_all(); test_determinism();`) in `main()` before the pass/fail print.

- [ ] **Step 4: Build and run the full test binary via the tests Makefile**

Run from `MatterSurfaceLib/`:
```bash
make -C tests particle_culling_tests && tests/particle_culling_tests
```
Expected: `All particle_culling tests passed`

- [ ] **Step 5: Commit**

```bash
git add MatterSurfaceLib/include/particle_culling.h MatterSurfaceLib/src/particle_culling.cpp \
        MatterSurfaceLib/tests/particle_culling_tests.cpp
git commit -m "feat: add occupancy-based interior particle culling pass"
```

---

## Task 4: Integrate into the app build + scene, gate tests, A/B acceptance

**Files:**
- Modify: `MatterSurfaceLib/Makefile`
- Modify: `MatterSurfaceLib/main.cpp:294-314` (remove static noise), `main.cpp:540-605` (rewrite scene)
- Modify: `build-all.sh:114`

- [ ] **Step 1: Add the three modules to the app Makefile**

In `MatterSurfaceLib/Makefile`:

Add the three sources to the `SRC =` list (line 129), appending before the imgui sources:
`src/lattice.cpp src/occupancy.cpp src/particle_culling.cpp`

Add their objects to the `OBJ =` list (line 130), appending before the imgui objects:
`$(OBJ_DIR)/lattice.o $(OBJ_DIR)/occupancy.o $(OBJ_DIR)/particle_culling.o`

Add three object build rules after the `mesh_simplifier.o` rule (after line 262):

```makefile
$(OBJ_DIR)/lattice.o: src/lattice.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

$(OBJ_DIR)/occupancy.o: src/occupancy.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

$(OBJ_DIR)/particle_culling.o: src/particle_culling.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@
```

- [ ] **Step 2: Remove the now-duplicated noise functions from main.cpp**

Delete the two `static` functions in `MatterSurfaceLib/main.cpp` lines 294-314 (the `lattice_vhash` and `lattice_vnoise` definitions, including the `// --- Deterministic 3D value noise...` comment above them). They now live in `particle_culling.cpp`.

Add `#include "include/particle_culling.h"` near the other project includes at the top of `main.cpp` (place it next to the existing `#include "include/cluster.h"`-style includes; if includes use the `include/` prefix this matches — verify against the existing include block and match its style).

- [ ] **Step 3: Rewrite the scene to use the culling pass**

Replace the entire body of `setup_lattice_scene()` in `MatterSurfaceLib/main.cpp` (lines 540-605) with:

```cpp
    void setup_lattice_scene() {
        printf("Setting up lattice brick scene...\n");

        // --- Tunables ---
        const int   DIM_X = 20, DIM_Y = 20, DIM_Z = 20;  // solid block of slots
        const float BASE_RADIUS = 0.4f;
        const float SPACING     = 2.0f * BASE_RADIUS;     // neighbors just touch
        const float POS_JITTER  = 0.15f * BASE_RADIUS;
        const float TINT_ALPHA  = 0.2f;
        const uint32_t MAT_OPAQUE_A = 8;  // stone_light (GROUP_STONE)
        const uint32_t MAT_OPAQUE_B = 9;  // stone_dark  (GROUP_STONE)

        // Default margin = 2 (conservatively safe). Set MSL_CULL_MARGIN to tune;
        // MSL_CULL_MARGIN=-1 bypasses culling (emit every slot) for A/B compare.
        int margin = 2;
        const char* mEnv = getenv("MSL_CULL_MARGIN");
        bool bypass = false;
        if (mEnv) { margin = atoi(mEnv); if (margin < 0) bypass = true; }

        // Build a solid block of occupancy, centered on the origin, with a
        // checkerboard of the two opaque stones so the surface shows variation.
        GridLattice lattice(SPACING);
        Occupancy occ;
        for (int ix = 0; ix < DIM_X; ++ix)
        for (int iy = 0; iy < DIM_Y; ++iy)
        for (int iz = 0; iz < DIM_Z; ++iz) {
            uint32_t mat = ((ix + iy + iz) & 1) ? MAT_OPAQUE_A : MAT_OPAQUE_B;
            occ.set(SlotCoord{ix, iy, iz}, SlotData{mat});
        }

        CullParams p;
        p.margin = margin; p.base_radius = BASE_RADIUS;
        p.jitter_amount = POS_JITTER; p.tint_alpha = TINT_ALPHA; p.seed = 1337;

        std::vector<EmittedParticle> emitted =
            bypass ? emit_all(lattice, occ, p) : cull_interior(lattice, occ, p);

        // Re-center: GridLattice puts slot 0 at the origin, so shift by half the
        // block extent to center the brick.
        float halfx = (DIM_X - 1) * SPACING * 0.5f;
        float halfy = (DIM_Y - 1) * SPACING * 0.5f;
        float halfz = (DIM_Z - 1) * SPACING * 0.5f;
        for (auto& ep : emitted) {
            Vector3 pos = { ep.position.x - halfx, ep.position.y - halfy, ep.position.z - halfz };
            test_cluster_->add_particle(pos, ep.radius, ep.materialId, ep.tint);
        }

        printf("[cull] occupied=%zu emitted=%zu (margin=%d%s)\n",
               occ.count(), emitted.size(), margin, bypass ? ", BYPASS" : "");

        test_cluster_->set_position({0.0f, 2.0f, 0.0f});
        test_cluster_->set_lod_level(0);
        test_cluster_->rebuild_dirty_cells();

        printf("Brick has %u cells, %u dirty\n",
               test_cluster_->get_cell_count(), test_cluster_->get_dirty_cell_count());
    }
```

- [ ] **Step 4: Build the Linux app and confirm it links**

Run from `MatterSurfaceLib/`:
```bash
WSL_LINUX=1 make 2>&1 | tail -5
```
Expected: ends with `✓ Copied to ./matter_surface_lib` (no compile/link errors).

- [ ] **Step 5: Gate the unit tests in build-all.sh**

In `build-all.sh`, add `particle_culling_tests` to the headless suite loop. Change line 114-115 from:
```bash
    for suite in mesh_simplifier_tests material_registry_tests cell_bounds_tests \
                 blas_refcount_tests mesh_continuity_tests blas_tint_tests; do
```
to:
```bash
    for suite in mesh_simplifier_tests material_registry_tests cell_bounds_tests \
                 blas_refcount_tests mesh_continuity_tests blas_tint_tests \
                 particle_culling_tests; do
```

- [ ] **Step 6: A/B acceptance capture (proves identical geometry + work reduction)**

Run from `MatterSurfaceLib/` (WSLg provides headless GL). Capture the culled build and the bypass build with an identical camera, and compare emitted-particle counts and the resulting images:

```bash
# Culled (default margin 2)
MSL_CAPTURE=/tmp/brick_culled.png MSL_RENDER_MODE=0 MSL_FRAMES=8 \
  MSL_CAM="14,14,14,0,2,0" ./matter_surface_lib 2>&1 | grep -E '\[cull\]|triangles' | tail -20

# Bypass (every slot emitted)
MSL_CULL_MARGIN=-1 MSL_CAPTURE=/tmp/brick_full.png MSL_RENDER_MODE=0 MSL_FRAMES=8 \
  MSL_CAM="14,14,14,0,2,0" ./matter_surface_lib 2>&1 | grep -E '\[cull\]|triangles' | tail -20
```

Expected and acceptance criteria:
- The `[cull]` lines show `emitted` far smaller for the culled run than the bypass run (e.g. for 20³ = 8000 occupied, margin 2 emits roughly the outer two shells ≈ 8000 − 16³ = 8000 − 4096 = 3904; bypass emits 8000).
- The summed per-cell triangle counts printed during meshing (`... N triangles`) should match between the two runs (identical outer surface). If they differ noticeably, the margin is too small — increase it and re-capture.
- `/tmp/brick_culled.png` and `/tmp/brick_full.png` should look identical. Report both images to the user for visual confirmation; the user makes the final call on the smallest safe margin.

> If margin 2 already matches, try `MSL_CULL_MARGIN=1` and re-run to see whether margin 1 still matches (smaller margin = more savings). Record the smallest margin that keeps triangle counts equal and update the `margin = 2` default in `setup_lattice_scene` accordingly.

- [ ] **Step 7: Commit**

```bash
git add MatterSurfaceLib/Makefile MatterSurfaceLib/main.cpp build-all.sh
git commit -m "feat: drive lattice brick scene through interior-culling pass"
```

---

## Self-Review

**Spec coverage:**
- Lattice interface + grid impl → Task 1. ✓
- Occupancy (sparse, packed keys) → Task 2. ✓
- Interior-culling pass with margin safeguard, determinism, emit_all baseline → Task 3. ✓
- Moved `lattice_vhash`/`lattice_vnoise` into the module → Task 3 (create), Task 4 Step 2 (remove from main). ✓
- Integration glue feeding `Cluster::add_particle` unchanged → Task 4 Step 3. ✓
- Margin clamped to ≥1; empty occupancy → empty result → Task 3 impl + `test_cull_counts`/burial tests. ✓
- Unit tests (burial counts, margin layers, thin shape, determinism, lattice, occupancy) → Tasks 1-3. ✓
- A/B visual capture acceptance + margin tuning → Task 4 Step 6. ✓
- `build-all.sh` gating → Task 4 Step 5. ✓
- Meshing pipeline untouched → no task modifies cell/surface/blas code. ✓

**Type consistency:** `SlotCoord`, `SlotData`, `EmittedParticle{position,radius,materialId,tint}`, `CullParams{margin,base_radius,jitter_amount,tint_alpha,seed}`, `cull_interior(const Lattice&, const Occupancy&, const CullParams&)`, `emit_all(...)`, `slot_is_buried(const Occupancy&, SlotCoord, int)` — names/signatures identical across header, impl, tests, and scene. `EmittedParticle` fields map 1:1 onto `Cluster::add_particle(Vector3, float, uint32_t, Vector4)`. ✓

**Placeholder scan:** No TBD/TODO; every code step shows complete code; commands have expected output. ✓
