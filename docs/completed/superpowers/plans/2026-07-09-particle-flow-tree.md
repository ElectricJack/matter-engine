# Particle-Flow Tree Generation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build ParticleFlowLib (generic particle kernel + path recorder), bind it to the MatterEngine3 DSL, and rewrite Tree.js to grow trunks/branches via strand particles blending into space colonization.

**Architecture:** A new sibling lib `ParticleFlowLib` (C++17, zero engine deps, instance-contained state) provides `pf::Sim` (velocity-canonical particle kernel with force/steer fields) and `pf::PathRecorder` (append-only PathSet observer). MatterEngine3 gains `src/pf_bindings.cpp` (global `__pf_*` functions dispatching through a DslState-owned registry; zero-copy Float32Array onTick views; `this.paths()` voxel-session sink stamping tapered cone+sphere brushes). JS layer: `shared-lib/particleflow.js` (wrapper classes) + `shared-lib/strands.js` (tree helpers) + rewritten `Tree.js`.

**Tech Stack:** C++17, QuickJS-ng (external ArrayBuffers + JS_NewTypedArray), existing voxel-session SDF pipeline (cone/sphere brushes, modifier stack), GNU make.

**Spec:** `docs/superpowers/specs/2026-07-09-particle-flow-tree-design.md` — read it before starting any task.

## Global Constraints

- C++17, `-Wall -Wextra`; ParticleFlowLib tests build with `-fsanitize=address,undefined` (MemoryLib convention).
- **No globals/statics in ParticleFlowLib** — all state lives in instances (sims run concurrently on Phase B bake worker threads).
- **Determinism:** same seed + config + tick count ⇒ bit-identical PathSet. Kernel iterates ascending slot order; RNG is instance-owned xoshiro256++; never wall-clock.
- **Append-only:** PathSet and the deposited point set never remove or mutate existing entries (monotonic accretion).
- **Incremental equivalence:** `run(N+M)` ≡ `run(N); run(M)` — dedicated test, never break it.
- Handles (sim/recorder) are bake-scoped: registry lives on DslState, freed when DslState dies; stale JS access throws — never reads freed memory.
- Test discipline (per Jack): each task runs ONLY its own new/changed test binary. Full suites ONLY at Checkpoint A (Task 4), Checkpoint B (Task 7), and the Final Gate (Task 10).
- All viewer/GPU runs need `GALLIUM_DRIVER=d3d12` (WSLg).
- Commit after every task (`git add <specific files>`).

## File Structure

```
ParticleFlowLib/                    (NEW project)
├── Makefile                        lib + ASan test build
├── include/
│   ├── particle_flow.h             V3 math, Rng, PathSet, SimConfig/FieldConfig/EmitterConfig,
│   │                               ITickObserver, Sim, PathRecorder declarations
│   └── pf_spatial_hash.h           header-only uniform-grid point hash
├── src/
│   ├── pf_math.cpp                 V3 helpers + Rng (xoshiro256++)
│   ├── pf_sim.cpp                  kernel: emit/fields/integrate/deposit/kill/observers
│   ├── pf_fields.cpp               field evaluation (bias/curl/adhere/attract/separate/drag)
│   └── pf_path_recorder.cpp        PathRecorder observer
└── tests/
    └── pf_tests.cpp                single ASan test binary (assert + printf style)

MatterEngine3/
├── Makefile                        MODIFY: compile ParticleFlowLib srcs + pf_bindings.cpp into libmatter_engine3.a
├── src/dsl_state.h                 MODIFY: pf registry (shared_ptr<void>) + budget deadline mirror
├── src/script_host.cpp             MODIFY: stash InterruptCtx deadline into DslState
├── src/pf_bindings.h               NEW: install_pf_bindings(JSContext*)
├── src/pf_bindings.cpp             NEW: __pf_* bindings, registry, views, paths sink
├── src/part_base.js.h              MODIFY: add paths() Part method
├── shared-lib/particleflow.js      NEW: particleSim()/pathRecorder() JS wrappers
├── shared-lib/strands.js           NEW: crown clouds, twig anchor sampling
├── examples/world_demo/schemas/Tree.js   REWRITE
└── tests/Makefile                  MODIFY: extend SCRIPT_CPP (pf sources) for run-script

MatterViewer/Makefile               MODIFY (Windows target only): PFL sources, include path, vpath
build-all.sh                        MODIFY: register ParticleFlowLib
```

---

### Task 1: ParticleFlowLib scaffold — contract types, Rng, spatial hash

**Files:**
- Create: `ParticleFlowLib/Makefile`
- Create: `ParticleFlowLib/include/particle_flow.h` (partial — types this task needs; Sim/config decls added in Task 2)
- Create: `ParticleFlowLib/include/pf_spatial_hash.h`
- Create: `ParticleFlowLib/src/pf_math.cpp`
- Test: `ParticleFlowLib/tests/pf_tests.cpp`

**Interfaces:**
- Consumes: nothing (greenfield).
- Produces: `pf::V3` (+`dot/cross/length/normalize` and operators), `pf::Rng{Rng(uint64_t), next_u64(), next_unit(), range(a,b), unit_sphere()}`, `pf::PathSet{channel_names, paths[]: {particle_id, xyz, channels[][], closed}}`, `pf::SpatialHash{SpatialHash(float cell), clear(), size(), insert(V3,uint32_t), query(V3,float,fn)}`. Task 2+ builds on these exact names.

- [ ] **Step 1: Write the failing test**

Create `ParticleFlowLib/tests/pf_tests.cpp`:

```cpp
// ParticleFlowLib test binary. Plain assert() + printf, built with ASan+UBSan.
#include "particle_flow.h"
#include "pf_spatial_hash.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>

using namespace pf;

static void test_rng_determinism() {
    Rng a(42), b(42), c(43);
    bool diverged = false;
    for (int i = 0; i < 1000; ++i) {
        uint64_t va = a.next_u64();
        assert(va == b.next_u64() && "same seed must produce identical stream");
        if (va != c.next_u64()) diverged = true;
    }
    assert(diverged && "different seeds must diverge");
    Rng d(7);
    for (int i = 0; i < 10000; ++i) {
        float u = d.next_unit();
        assert(u >= 0.0f && u < 1.0f);
        V3 s = d.unit_sphere();
        assert(std::fabs(length(s) - 1.0f) < 1e-4f);
    }
    printf("  rng determinism OK\n");
}

static void test_v3_math() {
    V3 v = normalize(V3{3, 0, 4});
    assert(std::fabs(v.x - 0.6f) < 1e-6f && std::fabs(v.z - 0.8f) < 1e-6f);
    V3 z = normalize(V3{0, 0, 0});           // zero-safe
    assert(z.x == 0 && z.y == 0 && z.z == 0);
    V3 c = cross(V3{1,0,0}, V3{0,1,0});
    assert(std::fabs(c.z - 1.0f) < 1e-6f);
    printf("  v3 math OK\n");
}

static void test_spatial_hash_vs_brute_force() {
    Rng rng(99);
    std::vector<V3> pts;
    for (int i = 0; i < 500; ++i)
        pts.push_back({rng.range(-5,5), rng.range(-5,5), rng.range(-5,5)});
    SpatialHash h(0.7f);
    for (uint32_t i = 0; i < pts.size(); ++i) h.insert(pts[i], i);
    assert(h.size() == pts.size());

    const V3 centers[] = {{0,0,0}, {2.5f,-1,3}, {-4.9f,4.9f,0}};
    const float radii[] = {0.3f, 1.1f, 4.0f};
    for (V3 q : centers) for (float r : radii) {
        std::vector<uint32_t> got;
        h.query(q, r, [&](uint32_t idx, V3, float d2) {
            assert(d2 <= r * r + 1e-5f);
            got.push_back(idx);
        });
        size_t expect = 0;
        for (uint32_t i = 0; i < pts.size(); ++i) {
            V3 d = pts[i] - q;
            if (dot(d, d) <= r * r) ++expect;
        }
        assert(got.size() == expect && "hash query must match brute force");
    }
    // Determinism: same query twice -> identical visit order.
    std::vector<uint32_t> o1, o2;
    h.query({0,0,0}, 3.0f, [&](uint32_t i, V3, float){ o1.push_back(i); });
    h.query({0,0,0}, 3.0f, [&](uint32_t i, V3, float){ o2.push_back(i); });
    assert(o1 == o2);
    printf("  spatial hash OK (%zu pts)\n", pts.size());
}

int main() {
    printf("pf_tests:\n");
    test_rng_determinism();
    test_v3_math();
    test_spatial_hash_vs_brute_force();
    printf("pf_tests: ALL OK\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make -C ParticleFlowLib test`
Expected: FAIL — `particle_flow.h: No such file or directory` (Makefile doesn't exist yet either; create Makefile in Step 3 first, then the compile fails on the missing header — either failure mode is the expected red).

- [ ] **Step 3: Write the implementation**

Create `ParticleFlowLib/Makefile`:

```makefile
# ParticleFlowLib: generic agent-particle kernel + path recording.
# Standalone build for dev/tests; MatterEngine3 compiles src/ directly
# into libmatter_engine3.a (MatterSurfaceLib direct-source pattern).
CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -g -O2 -I./include
OBJ_DIR = obj
SRCS = $(wildcard src/*.cpp)
OBJS = $(SRCS:src/%.cpp=$(OBJ_DIR)/%.o)
HEADERS = $(wildcard include/*.h)

all: libparticleflow.a

libparticleflow.a: $(OBJS)
	ar rcs $@ $^

$(OBJ_DIR)/%.o: src/%.cpp $(HEADERS)
	@mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

TEST_FLAGS = -std=c++17 -Wall -Wextra -g -I./include -fsanitize=address,undefined

test: tests/pf_tests.cpp $(SRCS) $(HEADERS)
	$(CXX) $(TEST_FLAGS) -o pf_tests tests/pf_tests.cpp $(SRCS)
	./pf_tests

clean:
	rm -rf $(OBJ_DIR) libparticleflow.a pf_tests

.PHONY: all test clean
```

Create `ParticleFlowLib/include/particle_flow.h`:

```cpp
#pragma once
// ParticleFlowLib: generic agent-particle kernel + path recording.
// No engine dependencies. All state is instance-contained (no globals/statics):
// N sims run concurrently on bake worker threads with zero coordination.
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace pf {

struct V3 { float x = 0, y = 0, z = 0; };

inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float length(V3 a);
V3 normalize(V3 a);   // zero-safe: returns {0,0,0} for near-zero input

// xoshiro256++ seeded via splitmix64. Deterministic, instance-owned.
struct Rng {
    uint64_t s[4];
    explicit Rng(uint64_t seed);
    uint64_t next_u64();
    float next_unit();                  // [0, 1)
    float range(float a, float b);
    V3 unit_sphere();                   // uniform direction on the unit sphere
};

// Append-only polylines with per-vertex attribute channels.
// Monotonic accretion is enforced structurally: vertices are only appended,
// paths are only appended, existing data is never mutated.
struct PathSet {
    struct Path {
        uint32_t particle_id = 0;
        std::vector<float> xyz;                     // 3 floats per vertex
        std::vector<std::vector<float>> channels;   // [channel][vertex]
        bool closed = false;                        // particle died / finalized
        size_t vertex_count() const { return xyz.size() / 3; }
    };
    std::vector<std::string> channel_names;         // fixed at construction
    std::vector<Path> paths;
};

} // namespace pf
```

Create `ParticleFlowLib/include/pf_spatial_hash.h`:

```cpp
#pragma once
#include "particle_flow.h"
#include <cmath>
#include <unordered_map>
#include <utility>

namespace pf {

// Uniform-grid point hash. Correctness does not depend on the key function
// (all candidates are exact-distance filtered); key collisions only cost time.
// Deterministic: cells are visited in fixed (z,y,x) loop order and points in
// insertion order, so query callbacks always fire in the same sequence.
class SpatialHash {
public:
    explicit SpatialHash(float cell) : cell_(cell > 1e-6f ? cell : 1e-6f) {}
    void clear() { cells_.clear(); count_ = 0; }
    size_t size() const { return count_; }
    float cell_size() const { return cell_; }

    void insert(V3 p, uint32_t idx) {
        cells_[key(cx(p.x), cx(p.y), cx(p.z))].push_back({p, idx});
        ++count_;
    }

    // Visit stored points within r of p: fn(idx, point, distance_squared).
    template <class F>
    void query(V3 p, float r, F&& fn) const {
        const float r2 = r * r;
        const int x0 = cx(p.x - r), x1 = cx(p.x + r);
        const int y0 = cx(p.y - r), y1 = cx(p.y + r);
        const int z0 = cx(p.z - r), z1 = cx(p.z + r);
        for (int z = z0; z <= z1; ++z)
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    auto it = cells_.find(key(x, y, z));
                    if (it == cells_.end()) continue;
                    for (const auto& e : it->second) {
                        V3 d = e.first - p;
                        float d2 = dot(d, d);
                        if (d2 <= r2) fn(e.second, e.first, d2);
                    }
                }
    }

private:
    int cx(float v) const { return (int)std::floor(v / cell_); }
    static uint64_t key(int x, int y, int z) {
        return ((uint64_t)(uint32_t)x * 73856093ull) ^
               ((uint64_t)(uint32_t)y * 19349663ull) ^
               ((uint64_t)(uint32_t)z * 83492791ull);
    }
    float cell_;
    size_t count_ = 0;
    std::unordered_map<uint64_t, std::vector<std::pair<V3, uint32_t>>> cells_;
};

} // namespace pf
```

Create `ParticleFlowLib/src/pf_math.cpp`:

```cpp
#include "particle_flow.h"
#include <cmath>

namespace pf {

float length(V3 a) { return std::sqrt(dot(a, a)); }

V3 normalize(V3 a) {
    float l = length(a);
    if (l < 1e-8f) return {0, 0, 0};
    return a * (1.0f / l);
}

static inline uint64_t rotl64(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

Rng::Rng(uint64_t seed) {
    // splitmix64 expansion of the seed into 4 non-zero lanes.
    uint64_t z = seed;
    for (int i = 0; i < 4; ++i) {
        z += 0x9E3779B97F4A7C15ull;
        uint64_t t = z;
        t = (t ^ (t >> 30)) * 0xBF58476D1CE4E5B9ull;
        t = (t ^ (t >> 27)) * 0x94D049BB133111EBull;
        s[i] = t ^ (t >> 31);
    }
    if (!(s[0] | s[1] | s[2] | s[3])) s[0] = 1;
}

uint64_t Rng::next_u64() {
    const uint64_t r = rotl64(s[0] + s[3], 23) + s[0];
    const uint64_t t = s[1] << 17;
    s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl64(s[3], 45);
    return r;
}

float Rng::next_unit() { return (float)((next_u64() >> 40) * (1.0 / 16777216.0)); }

float Rng::range(float a, float b) { return a + (b - a) * next_unit(); }

V3 Rng::unit_sphere() {
    float z = range(-1.0f, 1.0f);
    float a = range(0.0f, 6.28318530718f);
    float r = std::sqrt(std::fmax(0.0f, 1.0f - z * z));
    return {r * std::cos(a), r * std::sin(a), z};
}

} // namespace pf
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make -C ParticleFlowLib test`
Expected: PASS — output ends with `pf_tests: ALL OK`, exit 0, no ASan reports.

- [ ] **Step 5: Commit**

```bash
git add ParticleFlowLib/Makefile ParticleFlowLib/include ParticleFlowLib/src ParticleFlowLib/tests
git commit -m "feat(particleflow): scaffold ParticleFlowLib — V3 math, xoshiro256++ Rng, spatial hash, PathSet contract"
```

---

### Task 2: `pf::Sim` core — config, storage, emission, integrator, deposit, kill, observers

**Files:**
- Modify: `ParticleFlowLib/include/particle_flow.h` (append config structs + Sim + ITickObserver)
- Create: `ParticleFlowLib/src/pf_sim.cpp`
- Create: `ParticleFlowLib/src/pf_fields.cpp` (Bias + Drag only; Task 3 extends the switch)
- Test: `ParticleFlowLib/tests/pf_tests.cpp` (append tests)

**Interfaces:**
- Consumes: Task 1 types (`V3`, `Rng`, `SpatialHash`).
- Produces (exact, later tasks depend on these): `pf::FieldType{Bias,Curl,Adhere,Attract,Separate,Drag}`, `pf::FieldMode{Steer,Force}`, `pf::Fade`, `pf::FieldConfig`, `pf::EmitterConfig`, `pf::SimConfig`, `pf::ITickObserver::on_tick(const Sim&, uint32_t)`, and `pf::Sim` with: `Sim(SimConfig)`, `attach(ITickObserver*)`, `set_attractors(const float*, size_t)`, `run(uint32_t)`, `slot_count()`, `alive_count()`, `tick()`, `pos_data()`, `vel_data()`, `alive_data()`, `attr_data(uint32_t)`, `channel_count()`, `channel_index(const std::string&)`, `id_of(uint32_t)`, `born_this_tick()`, `died_this_tick()`, `emit_particle(V3,V3,const float*)`, `kill(uint32_t)`, `set_field_weight(uint32_t,float)`, `field_count()`, `attractors_remaining()`, `deposited_count()`, `deposited_points()`, `deposited_hash()`, `live_hash()`, `surface_normal(V3,float,bool*)` (declared now, implemented Task 3), `config()`, `rng()`. Internal free functions in pf_fields.cpp: `V3 field_steer_dir(const Sim&, const FieldConfig&, uint32_t slot)` and `V3 field_force(const Sim&, const FieldConfig&, uint32_t slot)` (declared in pf_sim.cpp via `namespace pf { ... }` prototypes).

- [ ] **Step 1: Write the failing tests**

Append to `ParticleFlowLib/tests/pf_tests.cpp` (before `main`, and add the calls to `main`):

```cpp
static V3 slot_pos(Sim& s, uint32_t i) {
    float* p = s.pos_data();
    return {p[3*i], p[3*i+1], p[3*i+2]};
}
static V3 slot_vel(Sim& s, uint32_t i) {
    float* v = s.vel_data();
    return {v[3*i], v[3*i+1], v[3*i+2]};
}

static SimConfig base_cfg() {
    SimConfig c;
    c.seed = 1234;
    c.dt = 1.0f;
    c.max_turn_rate = 0.1f;
    c.deposit_every = 0.05f;
    c.attributes = {"radius"};
    EmitterConfig e;
    e.shape = 1; e.center = {0,0,0}; e.axis = {0,1,0}; e.radius = 0.3f;
    e.rate = 0.5f; e.vel0 = 0.05f; e.jitter = 0.01f;
    e.attr_init = {0.1f};
    c.emitters = {e};
    FieldConfig up; up.type = FieldType::Bias; up.mode = FieldMode::Steer;
    up.dir = {0,1,0}; up.weight = 1.0f;
    c.fields = {up};
    c.speed_target = 0.05f; c.speed_relax = 0.2f;
    return c;
}

static bool sims_equal(Sim& a, Sim& b) {
    if (a.slot_count() != b.slot_count()) return false;
    if (a.alive_count() != b.alive_count()) return false;
    if (a.deposited_count() != b.deposited_count()) return false;
    size_t n = a.slot_count();
    if (memcmp(a.pos_data(), b.pos_data(), n*3*sizeof(float))) return false;
    if (memcmp(a.vel_data(), b.vel_data(), n*3*sizeof(float))) return false;
    for (size_t i = 0; i < n; ++i)
        if (a.id_of((uint32_t)i) != b.id_of((uint32_t)i)) return false;
    return true;
}

static void test_sim_determinism_and_incremental() {
    Sim a(base_cfg()), b(base_cfg()), c(base_cfg());
    a.run(300);
    b.run(300);
    assert(sims_equal(a, b) && "same seed+config+ticks must be bit-identical");
    c.run(120); c.run(180);                      // N+M == N then M
    assert(sims_equal(a, c) && "incremental run must equal one-shot run");
    assert(a.alive_count() > 0 && a.deposited_count() > 100);
    printf("  sim determinism + incremental OK (%u alive, %zu deposited)\n",
           a.alive_count(), a.deposited_count());
}

static void test_gravity_parabola() {
    SimConfig c;
    c.seed = 5; c.dt = 0.05f; c.max_turn_rate = 10.0f; c.deposit_every = 1e9f;
    // no emitters/steer: single hand-emitted ballistic particle + force gravity
    FieldConfig g; g.type = FieldType::Bias; g.mode = FieldMode::Force;
    g.dir = {0,-1,0}; g.weight = 9.8f;
    c.fields = {g};
    Sim s(c);
    uint32_t slot = s.emit_particle({0,0,0}, {2,0,0}, nullptr);
    assert(slot != UINT32_MAX);
    const int K = 40;
    s.run(K);
    // Semi-implicit Euler: y = -g*dt^2 * K*(K+1)/2 ; x = vx*dt*K
    V3 p = slot_pos(s, slot);
    float ey = -9.8f * c.dt * c.dt * (K * (K + 1) / 2.0f);
    assert(std::fabs(p.y - ey) < 1e-3f && "gravity must integrate a parabola");
    assert(std::fabs(p.x - 2.0f * c.dt * K) < 1e-3f);
    printf("  gravity parabola OK (y=%.4f expect %.4f)\n", p.y, ey);
}

static void test_turn_clamp() {
    SimConfig c;
    c.seed = 6; c.dt = 1.0f; c.max_turn_rate = 0.1f; c.deposit_every = 1e9f;
    FieldConfig fx; fx.type = FieldType::Bias; fx.mode = FieldMode::Steer;
    fx.dir = {1,0,0}; fx.weight = 1.0f;
    c.fields = {fx};
    Sim s(c);
    uint32_t slot = s.emit_particle({0,0,0}, {0,1,0}, nullptr);  // moving +y, steered to +x
    V3 prev = slot_vel(s, slot);
    for (int k = 0; k < 30; ++k) {
        s.run(1);
        V3 cur = slot_vel(s, slot);
        float cang = dot(normalize(prev), normalize(cur));
        float ang = std::acos(std::fmin(1.0f, std::fmax(-1.0f, cang)));
        assert(ang <= c.max_turn_rate + 1e-4f && "per-tick turn must be clamped");
        assert(std::fabs(length(cur) - length(prev)) < 1e-5f && "steer preserves speed");
        prev = cur;
    }
    // pi/2 turn at 0.1 rad/tick -> aligned with +x after ~16 ticks (30 run)
    assert(normalize(prev).x > 0.999f && "must converge to steer target");
    printf("  turn clamp OK\n");
}

static void test_emission_cap_age_reuse() {
    SimConfig c = base_cfg();
    c.max_particles = 8; c.max_age = 5;
    c.emitters[0].rate = 2.5f;
    Sim s(c);
    s.run(3);
    assert(s.alive_count() == 7 && "rate 2.5/tick for 3 ticks = 7 particles");
    s.run(1);
    assert(s.alive_count() == 8 && "hard cap at max_particles");
    s.run(3);  // ticks 5..7: age-5 kills begin; slots recycle, ids stay unique
    assert(s.alive_count() <= 8);
    assert(s.slot_count() <= 8 && "slots must be reused after death");
    // Unique ids: collect and pairwise-compare
    std::vector<uint32_t> ids;
    for (uint32_t i = 0; i < s.slot_count(); ++i) ids.push_back(s.id_of(i));
    for (size_t i = 0; i < ids.size(); ++i)
        for (size_t j = i + 1; j < ids.size(); ++j)
            assert(ids[i] != ids[j] && "live ids unique");
    printf("  emission/cap/age/reuse OK\n");
}
```

Add to `main()` before the final printf:

```cpp
    test_sim_determinism_and_incremental();
    test_gravity_parabola();
    test_turn_clamp();
    test_emission_cap_age_reuse();
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make -C ParticleFlowLib test`
Expected: FAIL — compile errors (`SimConfig` not declared).

- [ ] **Step 3: Write the implementation**

Append to `ParticleFlowLib/include/particle_flow.h` (inside `namespace pf`, after `PathSet`; also add `#include "pf_spatial_hash.h"` — or forward-declare and hold hashes by value via include; use the include):

At top of the header add after existing includes:
```cpp
#include <memory>
```
…and after the PathSet definition append:

```cpp
// ---------------------------------------------------------------------------
// Sim configuration
// ---------------------------------------------------------------------------
enum class FieldType { Bias, Curl, Adhere, Attract, Separate, Drag };
enum class FieldMode { Steer, Force };

// Optional axial weight fade: multiplier 1 where dot(p,axis) <= from,
// 0 where >= to, linear between. Disabled by default.
struct Fade { V3 axis{0,1,0}; float from = 0, to = 0; bool enabled = false; };

struct FieldConfig {
    FieldType type = FieldType::Bias;
    FieldMode mode = FieldMode::Steer;
    float weight = 1.0f;
    Fade fade;
    V3 dir{0,1,0};                // Bias direction
    float radius = 1.0f;          // Adhere/Separate neighborhood radius
    float surface_offset = 0.0f;  // Adhere: ride this far outside the surface
    float influence = 1.0f;       // Attract: capture radius for steering
    float kill_radius = 0.1f;     // Attract: consume distance
    bool kill_on_consume = true;  // Attract: strand terminates at its attractor
    float scale = 1.0f;           // Curl: spatial noise scale
    uint32_t seed = 0;            // Curl: noise seed
    float k = 0.0f;               // Drag coefficient
};

struct EmitterConfig {
    int shape = 1;                // 0=point 1=disc 2=ring
    V3 center{0,0,0}, axis{0,1,0};
    float radius = 0.5f;
    float rate = 1.0f;            // particles per tick (fractions accumulate)
    float vel0 = 1.0f;            // initial speed along axis
    float jitter = 0.0f;          // random velocity magnitude added
    std::vector<float> attr_init; // one per declared channel (missing -> 0)
};

struct SimConfig {
    uint64_t seed = 1;
    float dt = 1.0f;
    float max_turn_rate = 0.15f;  // radians/tick steer clamp
    float speed_target = -1.0f;   // <0 = no speed regulation
    float speed_relax = 0.1f;     // fraction/tick toward speed_target
    float deposit_every = 0.1f;   // distance between deposited points
    uint32_t max_age = 0;         // ticks; 0 = unlimited
    uint32_t max_particles = 16384;
    float hash_cell = 0.0f;       // 0 = auto (max field neighborhood radius)
    std::vector<std::string> attributes;
    std::vector<EmitterConfig> emitters;
    std::vector<FieldConfig> fields;
};

class Sim;
struct ITickObserver {
    virtual ~ITickObserver() = default;
    virtual void on_tick(const Sim& s, uint32_t tick) = 0;
};

// ---------------------------------------------------------------------------
// Sim: velocity-canonical agent-particle kernel. Deterministic: ascending
// slot iteration, instance-owned RNG, fixed dt. Single-threaded per instance;
// instances are fully independent (safe on concurrent bake workers).
// ---------------------------------------------------------------------------
class Sim {
public:
    explicit Sim(SimConfig cfg);
    void attach(ITickObserver* o);                     // not owned
    void set_attractors(const float* xyz, size_t n);   // appends n points
    void run(uint32_t n_ticks);                        // callable repeatedly

    uint32_t slot_count() const { return (uint32_t)id_.size(); }
    uint32_t alive_count() const { return alive_n_; }
    uint32_t tick() const { return tick_; }
    float* pos_data() { return pos_.data(); }
    float* vel_data() { return vel_.data(); }
    uint8_t* alive_data() { return alive_.data(); }
    const float* pos_data() const { return pos_.data(); }
    const uint8_t* alive_data() const { return alive_.data(); }
    float* attr_data(uint32_t ch) { return attrs_[ch].data(); }
    const float* attr_data(uint32_t ch) const { return attrs_[ch].data(); }
    uint32_t channel_count() const { return (uint32_t)attrs_.size(); }
    int channel_index(const std::string& name) const;
    uint32_t id_of(uint32_t slot) const { return id_[slot]; }
    const std::vector<uint32_t>& born_this_tick() const { return born_; }
    const std::vector<uint32_t>& died_this_tick() const { return died_; }

    uint32_t emit_particle(V3 pos, V3 vel, const float* attr_or_null);
    void kill(uint32_t slot);
    void set_field_weight(uint32_t field_index, float w);
    uint32_t field_count() const { return (uint32_t)cfg_.fields.size(); }
    uint32_t attractors_remaining() const { return attr_remaining_; }

    size_t deposited_count() const { return deposited_pts_.size(); }
    const std::vector<V3>& deposited_points() const { return deposited_pts_; }
    const SpatialHash& deposited_hash() const { return dep_hash_; }
    const SpatialHash& live_hash() const { return live_hash_; }
    // Outward surface normal estimate from the deposited neighborhood.
    // *ok=false when no deposited points lie within radius. (Impl: Task 3.)
    V3 surface_normal(V3 p, float radius, bool* ok) const;

    const SimConfig& config() const { return cfg_; }
    Rng& rng() { return rng_; }

private:
    void step();
    void run_emitters();
    void integrate_slot(uint32_t i);
    void kill_slot(uint32_t i);
    void deposit(V3 p);
    float fade_mult(const FieldConfig& f, V3 p) const;
    V3 attract_dir(uint32_t slot, V3 p);   // consumes attractors (Task 3)

    SimConfig cfg_;
    Rng rng_;
    uint32_t tick_ = 0, next_id_ = 0, alive_n_ = 0;
    std::vector<float> pos_, vel_;
    std::vector<std::vector<float>> attrs_;
    std::vector<uint8_t> alive_;
    std::vector<uint32_t> id_, age_;
    std::vector<float> dep_dist_;
    std::vector<uint32_t> free_slots_;
    std::vector<uint32_t> born_, died_;
    std::vector<float> emit_acc_;
    std::vector<V3> deposited_pts_;         // append-only
    SpatialHash dep_hash_, live_hash_;
    std::vector<V3> attractors_;            // append-only
    std::vector<uint8_t> attr_consumed_;
    uint32_t attr_remaining_ = 0;
    std::vector<ITickObserver*> observers_;
};
```

Also add `#include "pf_spatial_hash.h"` — NOTE: pf_spatial_hash.h includes particle_flow.h, which would be circular. Resolve by moving `V3` + inline math + `Rng` declarations ABOVE a new include of pf_spatial_hash.h in particle_flow.h is still circular. Instead: pf_spatial_hash.h must NOT include particle_flow.h; give it its own guard-friendly design — change `pf_spatial_hash.h` (from Task 1) to be included FROM particle_flow.h after V3 is defined, and delete its `#include "particle_flow.h"` line. Concretely: in `pf_spatial_hash.h`, replace `#include "particle_flow.h"` with a comment `// Included by particle_flow.h after V3 is defined; do not include directly.` and in `particle_flow.h` add `#include "pf_spatial_hash.h"` immediately after the `normalize` declaration (before Rng). Tests keep working because they include particle_flow.h first (update pf_tests.cpp includes to only `#include "particle_flow.h"`).

Create `ParticleFlowLib/src/pf_sim.cpp`:

```cpp
#include "particle_flow.h"
#include <algorithm>
#include <cmath>

namespace pf {

// Implemented in pf_fields.cpp. Bias/Drag land in Task 2; Curl/Adhere/Separate
// in Task 3 (until then they contribute zero, which is valid field behavior).
V3 field_steer_dir(const Sim& s, const FieldConfig& f, uint32_t slot);
V3 field_force(const Sim& s, const FieldConfig& f, uint32_t slot);

static inline V3 read3(const std::vector<float>& a, uint32_t i) {
    return {a[3*i], a[3*i+1], a[3*i+2]};
}
static inline void write3(std::vector<float>& a, uint32_t i, V3 v) {
    a[3*i] = v.x; a[3*i+1] = v.y; a[3*i+2] = v.z;
}

static float auto_cell(const SimConfig& c) {
    float r = 0.0f;
    for (const auto& f : c.fields) {
        if (f.type == FieldType::Adhere || f.type == FieldType::Separate)
            r = std::max(r, f.radius);
        if (f.type == FieldType::Attract)
            r = std::max(r, f.influence);
    }
    return r > 1e-6f ? r : 1.0f;
}

Sim::Sim(SimConfig cfg)
    : cfg_(std::move(cfg)), rng_(cfg_.seed),
      dep_hash_(cfg_.hash_cell > 0 ? cfg_.hash_cell : auto_cell(cfg_)),
      live_hash_(cfg_.hash_cell > 0 ? cfg_.hash_cell : auto_cell(cfg_)) {
    attrs_.resize(cfg_.attributes.size());
    emit_acc_.assign(cfg_.emitters.size(), 0.0f);
}

void Sim::attach(ITickObserver* o) { observers_.push_back(o); }

void Sim::set_attractors(const float* xyz, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        attractors_.push_back({xyz[3*i], xyz[3*i+1], xyz[3*i+2]});
        attr_consumed_.push_back(0);
    }
    attr_remaining_ += (uint32_t)n;
}

int Sim::channel_index(const std::string& name) const {
    for (size_t i = 0; i < cfg_.attributes.size(); ++i)
        if (cfg_.attributes[i] == name) return (int)i;
    return -1;
}

void Sim::deposit(V3 p) {
    dep_hash_.insert(p, (uint32_t)deposited_pts_.size());
    deposited_pts_.push_back(p);
}

uint32_t Sim::emit_particle(V3 p, V3 v, const float* attr_or_null) {
    if (alive_n_ >= cfg_.max_particles) return UINT32_MAX;
    uint32_t slot;
    if (!free_slots_.empty()) { slot = free_slots_.back(); free_slots_.pop_back(); }
    else {
        slot = slot_count();
        pos_.resize(pos_.size() + 3); vel_.resize(vel_.size() + 3);
        for (auto& ch : attrs_) ch.push_back(0.0f);
        alive_.push_back(0); id_.push_back(0); age_.push_back(0);
        dep_dist_.push_back(0.0f);
    }
    write3(pos_, slot, p); write3(vel_, slot, v);
    for (size_t c = 0; c < attrs_.size(); ++c)
        attrs_[c][slot] = attr_or_null ? attr_or_null[c] : 0.0f;
    alive_[slot] = 1; id_[slot] = next_id_++; age_[slot] = 0;
    dep_dist_[slot] = 0.0f;
    ++alive_n_;
    born_.push_back(slot);
    deposit(p);                     // spawn point is wood from tick zero
    return slot;
}

void Sim::kill(uint32_t slot) {
    if (slot < slot_count() && alive_[slot]) kill_slot(slot);
}

void Sim::kill_slot(uint32_t i) {
    alive_[i] = 0; --alive_n_;
    died_.push_back(i);
    free_slots_.push_back(i);
}

void Sim::set_field_weight(uint32_t idx, float w) {
    if (idx < cfg_.fields.size()) cfg_.fields[idx].weight = w;
}

float Sim::fade_mult(const FieldConfig& f, V3 p) const {
    if (!f.fade.enabled) return 1.0f;
    float t = dot(p, f.fade.axis);
    if (t <= f.fade.from) return 1.0f;
    if (t >= f.fade.to) return 0.0f;
    float d = f.fade.to - f.fade.from;
    return d > 1e-8f ? 1.0f - (t - f.fade.from) / d : 0.0f;
}

void Sim::run_emitters() {
    for (size_t e = 0; e < cfg_.emitters.size(); ++e) {
        const EmitterConfig& em = cfg_.emitters[e];
        emit_acc_[e] += em.rate;
        while (emit_acc_[e] >= 1.0f) {
            emit_acc_[e] -= 1.0f;
            V3 ax = normalize(em.axis);
            V3 ref = std::fabs(ax.y) < 0.9f ? V3{0,1,0} : V3{1,0,0};
            V3 n1 = normalize(cross(ax, ref));
            V3 n2 = cross(ax, n1);
            V3 p = em.center;
            if (em.shape != 0) {
                float a = rng_.range(0.0f, 6.28318530718f);
                float r = (em.shape == 2) ? em.radius
                          : em.radius * std::sqrt(rng_.next_unit());
                p = p + n1 * (std::cos(a) * r) + n2 * (std::sin(a) * r);
            }
            V3 v = ax * em.vel0;
            if (em.jitter > 0.0f) v = v + rng_.unit_sphere() * (em.jitter * rng_.next_unit());
            float attrs[16] = {0};
            size_t nc = std::min(attrs_.size(), (size_t)16);
            for (size_t c = 0; c < nc; ++c)
                attrs[c] = c < em.attr_init.size() ? em.attr_init[c] : 0.0f;
            if (emit_particle(p, v, attrs) == UINT32_MAX) { emit_acc_[e] = 0; break; }
        }
    }
}

static V3 rotate_toward(V3 v, V3 desired, float max_angle) {
    float sp = length(v);
    if (sp < 1e-8f || length(desired) < 1e-8f) return v;
    V3 vn = v * (1.0f / sp);
    float c = std::fmax(-1.0f, std::fmin(1.0f, dot(vn, desired)));
    float ang = std::acos(c);
    if (ang <= max_angle) return desired * sp;
    V3 axis = cross(vn, desired);
    if (length(axis) < 1e-6f)  // near-parallel/antiparallel: any perpendicular
        axis = cross(vn, std::fabs(vn.y) < 0.9f ? V3{0,1,0} : V3{1,0,0});
    axis = normalize(axis);
    float ca = std::cos(max_angle), sa = std::sin(max_angle);
    V3 r = vn * ca + cross(axis, vn) * sa + axis * (dot(axis, vn) * (1.0f - ca));
    return r * sp;
}

void Sim::integrate_slot(uint32_t i) {
    V3 p = read3(pos_, i), v = read3(vel_, i);
    V3 force{0,0,0}, steer{0,0,0};
    for (size_t fi = 0; fi < cfg_.fields.size(); ++fi) {
        const FieldConfig& f = cfg_.fields[fi];
        float w = f.weight * fade_mult(f, p);
        if (w == 0.0f) continue;
        if (f.mode == FieldMode::Force) {
            force = force + field_force(*this, f, i) * w;
        } else {
            V3 d = (f.type == FieldType::Attract) ? attract_dir(i, p)
                                                  : field_steer_dir(*this, f, i);
            steer = steer + d * w;
        }
    }
    if (!alive_[i]) return;   // attract capture may have killed this slot
    v = v + force * cfg_.dt;
    V3 desired = normalize(steer);
    if (desired.x != 0 || desired.y != 0 || desired.z != 0)
        v = rotate_toward(v, desired, cfg_.max_turn_rate);
    if (cfg_.speed_target >= 0.0f) {
        float sp = length(v);
        float ns = sp + (cfg_.speed_target - sp) * cfg_.speed_relax;
        V3 dirv = sp > 1e-8f ? v * (1.0f / sp) : desired;
        v = dirv * ns;
    }
    p = p + v * cfg_.dt;
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
        kill_slot(i);           // NaN guard: kill, never propagate
        return;
    }
    write3(pos_, i, p); write3(vel_, i, v);
    dep_dist_[i] += length(v) * cfg_.dt;
    if (dep_dist_[i] >= cfg_.deposit_every) { deposit(p); dep_dist_[i] = 0.0f; }
    if (cfg_.max_age > 0 && ++age_[i] >= cfg_.max_age) kill_slot(i);
    else if (cfg_.max_age == 0) ++age_[i];
}

void Sim::step() {
    born_.clear(); died_.clear();
    ++tick_;
    run_emitters();
    // Live hash holds start-of-tick positions; already-integrated neighbors
    // are seen at their old position this tick. Deterministic and cheap.
    live_hash_.clear();
    for (uint32_t i = 0; i < slot_count(); ++i)
        if (alive_[i]) live_hash_.insert(read3(pos_, i), i);
    for (uint32_t i = 0; i < slot_count(); ++i)
        if (alive_[i]) integrate_slot(i);
    for (auto* o : observers_) o->on_tick(*this, tick_);
}

void Sim::run(uint32_t n) { for (uint32_t k = 0; k < n; ++k) step(); }

} // namespace pf
```

NOTE the age bug trap: the `if/else` above increments age exactly once per tick in both branches — keep it exactly as written (increment inside the kill check when max_age>0, plain increment otherwise).

Create `ParticleFlowLib/src/pf_fields.cpp`:

```cpp
#include "particle_flow.h"
#include <cmath>

namespace pf {

// Task 2 implements Bias (steer+force) and Drag. Task 3 extends this file
// with Curl/Adhere/Separate and implements Sim::attract_dir/surface_normal.

V3 field_steer_dir(const Sim& s, const FieldConfig& f, uint32_t slot) {
    (void)s; (void)slot;
    switch (f.type) {
        case FieldType::Bias: return normalize(f.dir);
        default:              return {0, 0, 0};
    }
}

V3 field_force(const Sim& s, const FieldConfig& f, uint32_t slot) {
    switch (f.type) {
        case FieldType::Bias: return normalize(f.dir);
        case FieldType::Drag: {
            const float* v = const_cast<Sim&>(s).vel_data();
            return V3{v[3*slot], v[3*slot+1], v[3*slot+2]} * (-f.k);
        }
        default: return {0, 0, 0};
    }
}

// Stubs completed in Task 3.
V3 Sim::attract_dir(uint32_t, V3) { return {0, 0, 0}; }
V3 Sim::surface_normal(V3, float, bool* ok) const { if (ok) *ok = false; return {0,0,0}; }

} // namespace pf
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make -C ParticleFlowLib test`
Expected: PASS — all Task 1 + Task 2 tests, `pf_tests: ALL OK`, no ASan reports.

- [ ] **Step 5: Commit**

```bash
git add ParticleFlowLib/include/particle_flow.h ParticleFlowLib/include/pf_spatial_hash.h ParticleFlowLib/src/pf_sim.cpp ParticleFlowLib/src/pf_fields.cpp ParticleFlowLib/tests/pf_tests.cpp
git commit -m "feat(particleflow): Sim kernel — emission, force/steer integrator with turn clamp, deposit, kill, observers"
```

---

### Task 3: Field primitives — curl, adhere, separate, attract, surface_normal

**Files:**
- Modify: `ParticleFlowLib/src/pf_fields.cpp` (replace the Task 2 stubs/switch)
- Test: `ParticleFlowLib/tests/pf_tests.cpp` (append tests)

**Interfaces:**
- Consumes: Task 2 `Sim` accessors (`live_hash()`, `deposited_hash()`, `deposited_points()`, `pos_data()`, `alive_data()`, `config()`).
- Produces: complete `field_steer_dir`/`field_force` switches; `Sim::attract_dir(uint32_t, V3)` (consumes attractors, honors `kill_on_consume`); `Sim::surface_normal(V3, float, bool*)`. No signature changes.

- [ ] **Step 1: Write the failing tests**

Append to `ParticleFlowLib/tests/pf_tests.cpp` (and call from `main`):

```cpp
static void test_adhere_pulls_toward_deposited() {
    SimConfig c;
    c.seed = 11; c.dt = 1.0f; c.max_turn_rate = 0.5f; c.deposit_every = 1e9f;
    FieldConfig ad; ad.type = FieldType::Adhere; ad.mode = FieldMode::Steer;
    ad.weight = 1.0f; ad.radius = 2.0f; ad.surface_offset = 0.0f;
    c.fields = {ad};
    Sim s(c);
    // Wall of deposited points on the x=0 plane (via a throwaway particle's
    // spawn deposits): emit dead-still particles along the plane then kill.
    for (int i = -3; i <= 3; ++i) {
        uint32_t sl = s.emit_particle({0, (float)i * 0.3f, 0}, {0,0,0}, nullptr);
        s.kill(sl);
    }
    assert(s.deposited_count() == 7);
    // A mover at x=1 heading +y must curve toward the wall (x decreasing).
    uint32_t m = s.emit_particle({1, 0, 0}, {0, 0.1f, 0}, nullptr);
    s.run(20);
    assert(slot_pos(s, m).x < 0.9f && "adhere must pull toward deposited set");
    printf("  adhere OK (x=%.3f)\n", slot_pos(s, m).x);
}

static void test_separate_pushes_apart() {
    SimConfig c;
    c.seed = 12; c.dt = 1.0f; c.max_turn_rate = 0.5f; c.deposit_every = 1e9f;
    FieldConfig sep; sep.type = FieldType::Separate; sep.mode = FieldMode::Steer;
    sep.weight = 1.0f; sep.radius = 1.0f;
    c.fields = {sep};
    Sim s(c);
    uint32_t a = s.emit_particle({-0.1f, 0, 0}, {0, 0.1f, 0}, nullptr);
    uint32_t b = s.emit_particle({ 0.1f, 0, 0}, {0, 0.1f, 0}, nullptr);
    float d0 = length(slot_pos(s, a) - slot_pos(s, b));
    s.run(15);
    float d1 = length(slot_pos(s, a) - slot_pos(s, b));
    assert(d1 > d0 + 0.05f && "separate must push live particles apart");
    printf("  separate OK (%.3f -> %.3f)\n", d0, d1);
}

static void test_attract_consume_and_kill() {
    SimConfig c;
    c.seed = 13; c.dt = 1.0f; c.max_turn_rate = 0.6f; c.deposit_every = 1e9f;
    c.speed_target = 0.2f; c.speed_relax = 1.0f;
    FieldConfig at; at.type = FieldType::Attract; at.mode = FieldMode::Steer;
    at.weight = 1.0f; at.influence = 10.0f; at.kill_radius = 0.25f;
    at.kill_on_consume = true;
    c.fields = {at};
    Sim s(c);
    float cloud[6] = { 3, 0, 0,   0, 3, 0 };
    s.set_attractors(cloud, 2);
    assert(s.attractors_remaining() == 2);
    uint32_t a = s.emit_particle({0.5f, 0, 0}, {0.2f, 0, 0}, nullptr);
    (void)a;
    s.run(40);
    assert(s.attractors_remaining() == 1 && "nearest attractor must be consumed");
    assert(s.alive_count() == 0 && "kill_on_consume terminates the strand");
    printf("  attract consume/kill OK\n");
}

static void test_surface_normal() {
    SimConfig c; c.seed = 14; c.deposit_every = 1e9f;
    Sim s(c);
    // Deposit a small patch around the origin in the y=0 plane.
    for (int i = -2; i <= 2; ++i) for (int j = -2; j <= 2; ++j) {
        uint32_t sl = s.emit_particle({i * 0.2f, 0, j * 0.2f}, {0,0,0}, nullptr);
        s.kill(sl);
    }
    bool ok = false;
    V3 n = s.surface_normal({0, 0.5f, 0}, 1.5f, &ok);
    assert(ok && "patch within radius");
    assert(n.y > 0.95f && "normal points outward (away from the deposit)");
    ok = true;
    s.surface_normal({100, 100, 100}, 1.0f, &ok);
    assert(!ok && "no neighbors -> ok=false");
    printf("  surface normal OK\n");
}

static void test_curl_is_deterministic_and_bounded() {
    SimConfig c; c.seed = 15; c.deposit_every = 1e9f; c.max_turn_rate = 0.5f;
    FieldConfig cu; cu.type = FieldType::Curl; cu.mode = FieldMode::Steer;
    cu.weight = 1.0f; cu.scale = 2.0f; cu.seed = 777;
    c.fields = {cu};
    Sim s1(c), s2(c);
    uint32_t p1 = s1.emit_particle({0,0,0}, {0.1f, 0, 0}, nullptr);
    uint32_t p2 = s2.emit_particle({0,0,0}, {0.1f, 0, 0}, nullptr);
    s1.run(50); s2.run(50);
    V3 a = slot_pos(s1, p1), b = slot_pos(s2, p2);
    assert(std::memcmp(&a, &b, sizeof(V3)) == 0 && "curl must be deterministic");
    assert(length(a) > 1e-3f && "particle must move");
    printf("  curl OK\n");
}
```

Add to `main()`:

```cpp
    test_adhere_pulls_toward_deposited();
    test_separate_pushes_apart();
    test_attract_consume_and_kill();
    test_surface_normal();
    test_curl_is_deterministic_and_bounded();
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make -C ParticleFlowLib test`
Expected: FAIL — assertions fire (stub fields return zero: adhere mover keeps x=1.0, attract consumes nothing).

- [ ] **Step 3: Write the implementation**

Replace the entire contents of `ParticleFlowLib/src/pf_fields.cpp` with:

```cpp
#include "particle_flow.h"
#include <cmath>

namespace pf {

// ---------------------------------------------------------------------------
// Seeded value noise + curl. Divergence-free steering from the curl of a
// 3-component value-noise vector potential (finite differences).
// ---------------------------------------------------------------------------
static float hash01(uint32_t seed, int x, int y, int z) {
    uint32_t h = seed;
    h ^= (uint32_t)x * 0x8DA6B343u;
    h ^= (uint32_t)y * 0xD8163841u;
    h ^= (uint32_t)z * 0xCB1AB31Fu;
    h ^= h >> 13; h *= 0x5BD1E995u; h ^= h >> 15;
    return (float)(h & 0xFFFFFFu) * (1.0f / 16777216.0f);
}

static float vnoise(uint32_t seed, V3 p) {
    int x0 = (int)std::floor(p.x), y0 = (int)std::floor(p.y), z0 = (int)std::floor(p.z);
    float fx = p.x - x0, fy = p.y - y0, fz = p.z - z0;
    // smoothstep fade
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy); fz = fz * fz * (3 - 2 * fz);
    float c[2][2][2];
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j)
            for (int k = 0; k < 2; ++k)
                c[i][j][k] = hash01(seed, x0 + i, y0 + j, z0 + k);
    float x00 = c[0][0][0] + (c[1][0][0] - c[0][0][0]) * fx;
    float x10 = c[0][1][0] + (c[1][1][0] - c[0][1][0]) * fx;
    float x01 = c[0][0][1] + (c[1][0][1] - c[0][0][1]) * fx;
    float x11 = c[0][1][1] + (c[1][1][1] - c[0][1][1]) * fx;
    float y0v = x00 + (x10 - x00) * fy;
    float y1v = x01 + (x11 - x01) * fy;
    return y0v + (y1v - y0v) * fz;
}

static V3 potential(uint32_t seed, V3 p) {
    return { vnoise(seed ^ 0x9E3779B9u, p),
             vnoise(seed ^ 0x85EBCA6Bu, p),
             vnoise(seed ^ 0xC2B2AE35u, p) };
}

static V3 curl_noise(uint32_t seed, V3 p, float scale) {
    const float inv = scale > 1e-6f ? 1.0f / scale : 1.0f;
    p = p * inv;
    const float e = 0.05f;
    V3 dx1 = potential(seed, p + V3{e,0,0}), dx0 = potential(seed, p - V3{e,0,0});
    V3 dy1 = potential(seed, p + V3{0,e,0}), dy0 = potential(seed, p - V3{0,e,0});
    V3 dz1 = potential(seed, p + V3{0,0,e}), dz0 = potential(seed, p - V3{0,0,e});
    const float s = 1.0f / (2.0f * e);
    // curl F = (dFz/dy - dFy/dz, dFx/dz - dFz/dx, dFy/dx - dFx/dy)
    return normalize(V3{
        (dy1.z - dy0.z) * s - (dz1.y - dz0.y) * s,
        (dz1.x - dz0.x) * s - (dx1.z - dx0.z) * s,
        (dx1.y - dx0.y) * s - (dy1.x - dy0.x) * s });
}

// ---------------------------------------------------------------------------
// Neighborhood fields
// ---------------------------------------------------------------------------
static V3 slot_p(const Sim& s, uint32_t i) {
    const float* p = s.pos_data();
    return {p[3*i], p[3*i+1], p[3*i+2]};
}

static V3 adhere_dir(const Sim& s, const FieldConfig& f, V3 p) {
    V3 sum{0,0,0}; uint32_t n = 0;
    s.deposited_hash().query(p, f.radius, [&](uint32_t, V3 q, float) {
        sum = sum + q; ++n;
    });
    if (n == 0) return {0,0,0};
    V3 avg = sum * (1.0f / (float)n);
    V3 out = normalize(p - avg);                       // outward surface normal
    V3 target = avg + out * f.surface_offset;          // ride outside the wood
    return normalize(target - p);
}

static V3 separate_dir(const Sim& s, const FieldConfig& f, uint32_t slot, V3 p) {
    V3 push{0,0,0}; uint32_t n = 0;
    s.live_hash().query(p, f.radius, [&](uint32_t idx, V3 q, float d2) {
        if (idx == slot) return;
        V3 d = p - q;
        float dist = std::sqrt(d2);
        if (dist > 1e-6f) { push = push + d * (1.0f / (dist * dist)); ++n; }
    });
    if (n == 0) return {0,0,0};
    return normalize(push);
}

V3 field_steer_dir(const Sim& s, const FieldConfig& f, uint32_t slot) {
    switch (f.type) {
        case FieldType::Bias:     return normalize(f.dir);
        case FieldType::Curl:     return curl_noise(f.seed, slot_p(s, slot), f.scale);
        case FieldType::Adhere:   return adhere_dir(s, f, slot_p(s, slot));
        case FieldType::Separate: return separate_dir(s, f, slot, slot_p(s, slot));
        default:                  return {0, 0, 0};   // Attract handled by Sim; Drag is force-only
    }
}

V3 field_force(const Sim& s, const FieldConfig& f, uint32_t slot) {
    switch (f.type) {
        case FieldType::Bias: return normalize(f.dir);
        case FieldType::Curl: return curl_noise(f.seed, slot_p(s, slot), f.scale);
        case FieldType::Drag: {
            const float* v = const_cast<Sim&>(s).vel_data();
            return V3{v[3*slot], v[3*slot+1], v[3*slot+2]} * (-f.k);
        }
        default: return {0, 0, 0};
    }
}

// ---------------------------------------------------------------------------
// Sim members that need attractor mutation / deposited queries
// ---------------------------------------------------------------------------
V3 Sim::attract_dir(uint32_t slot, V3 p) {
    // Nearest unconsumed attractor within influence. Linear scan is fine at
    // the ~500-attractor scale; ascending index = deterministic tie-break.
    const FieldConfig* fc = nullptr;
    for (const auto& f : cfg_.fields)
        if (f.type == FieldType::Attract) { fc = &f; break; }
    if (!fc || attr_remaining_ == 0) return {0,0,0};
    int best = -1; float best_d2 = fc->influence * fc->influence;
    for (size_t i = 0; i < attractors_.size(); ++i) {
        if (attr_consumed_[i]) continue;
        V3 d = attractors_[i] - p;
        float d2 = dot(d, d);
        if (d2 < best_d2) { best_d2 = d2; best = (int)i; }
    }
    if (best < 0) return {0,0,0};
    if (best_d2 <= fc->kill_radius * fc->kill_radius) {
        attr_consumed_[best] = 1;
        --attr_remaining_;
        if (fc->kill_on_consume) kill_slot(slot);
        return {0,0,0};
    }
    return normalize(attractors_[best] - p);
}

V3 Sim::surface_normal(V3 p, float radius, bool* ok) const {
    V3 sum{0,0,0}; uint32_t n = 0;
    dep_hash_.query(p, radius, [&](uint32_t, V3 q, float) { sum = sum + q; ++n; });
    if (n == 0) { if (ok) *ok = false; return {0,0,0}; }
    if (ok) *ok = true;
    return normalize(p - sum * (1.0f / (float)n));
}

} // namespace pf
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make -C ParticleFlowLib test`
Expected: PASS — `pf_tests: ALL OK`, no ASan reports. If `test_attract_consume_and_kill` is flaky on turn geometry, the particle spawn/speed values above are chosen so the strand reaches the (3,0,0) attractor within 40 ticks — debug with prints rather than loosening the assert.

- [ ] **Step 5: Commit**

```bash
git add ParticleFlowLib/src/pf_fields.cpp ParticleFlowLib/tests/pf_tests.cpp
git commit -m "feat(particleflow): field primitives — curl noise, adhere, separate, attract consume/kill, surface_normal"
```

---

### Task 4: PathRecorder + CHECKPOINT A

**Files:**
- Modify: `ParticleFlowLib/include/particle_flow.h` (append PathRecorder decl)
- Create: `ParticleFlowLib/src/pf_path_recorder.cpp`
- Test: `ParticleFlowLib/tests/pf_tests.cpp` (append tests)

**Interfaces:**
- Consumes: `ITickObserver`, `Sim` accessors, `PathSet`.
- Produces (bindings in Task 5 depend on these exact names): `pf::PathRecorder{PathRecorder(float min_segment, const std::vector<std::string>& channel_names), const PathSet& paths() const, on_tick(...)}` — plus helper `V3 path_end_dir(const PathSet::Path&)` (free function in namespace pf, declared in particle_flow.h).

- [ ] **Step 1: Write the failing tests**

Append to `ParticleFlowLib/tests/pf_tests.cpp` (and call from `main`):

```cpp
static bool pathsets_equal(const PathSet& a, const PathSet& b) {
    if (a.paths.size() != b.paths.size()) return false;
    for (size_t i = 0; i < a.paths.size(); ++i) {
        const auto& pa = a.paths[i]; const auto& pb = b.paths[i];
        if (pa.particle_id != pb.particle_id || pa.closed != pb.closed) return false;
        if (pa.xyz.size() != pb.xyz.size()) return false;
        if (memcmp(pa.xyz.data(), pb.xyz.data(), pa.xyz.size()*sizeof(float))) return false;
        for (size_t c = 0; c < pa.channels.size(); ++c)
            if (memcmp(pa.channels[c].data(), pb.channels[c].data(),
                       pa.channels[c].size()*sizeof(float))) return false;
    }
    return true;
}

static void test_path_recorder() {
    SimConfig c = base_cfg();
    c.max_age = 120;
    // one-shot vs incremental, both recorded
    Sim a(c), b(c);
    PathRecorder ra(0.02f, c.attributes), rb(0.02f, c.attributes);
    a.attach(&ra); b.attach(&rb);
    a.run(200);
    b.run(80); b.run(120);
    assert(pathsets_equal(ra.paths(), rb.paths()) &&
           "recorded PathSet must be identical for N+M vs N,M");
    const PathSet& ps = ra.paths();
    assert(!ps.paths.empty());
    assert(ps.channel_names.size() == 1 && ps.channel_names[0] == "radius");
    size_t closed = 0;
    for (const auto& p : ps.paths) {
        assert(p.vertex_count() >= 1);
        assert(p.channels.size() == 1);
        assert(p.channels[0].size() == p.vertex_count());
        // min-segment decimation: consecutive vertices >= min_segment apart
        for (size_t v = 1; v < p.vertex_count(); ++v) {
            V3 q0{p.xyz[3*(v-1)], p.xyz[3*(v-1)+1], p.xyz[3*(v-1)+2]};
            V3 q1{p.xyz[3*v],     p.xyz[3*v+1],     p.xyz[3*v+2]};
            assert(length(q1 - q0) >= 0.02f - 1e-5f);
        }
        if (p.closed) ++closed;
    }
    assert(closed > 0 && "max_age deaths must close paths");
    // end direction helper
    for (const auto& p : ps.paths) {
        if (p.vertex_count() < 2) continue;
        V3 d = path_end_dir(p);
        assert(std::fabs(length(d) - 1.0f) < 1e-4f);
        break;
    }
    printf("  path recorder OK (%zu paths, %zu closed)\n", ps.paths.size(), closed);
}

static void test_pathset_append_only() {
    SimConfig c = base_cfg();
    Sim s(c);
    PathRecorder r(0.02f, c.attributes);
    s.attach(&r);
    s.run(100);
    // Snapshot, run more, verify the old prefix is untouched (accretion).
    PathSet snap = r.paths();   // copy
    s.run(100);
    const PathSet& now = r.paths();
    assert(now.paths.size() >= snap.paths.size());
    for (size_t i = 0; i < snap.paths.size(); ++i) {
        const auto& po = snap.paths[i]; const auto& pn = now.paths[i];
        assert(pn.particle_id == po.particle_id);
        assert(pn.xyz.size() >= po.xyz.size());
        assert(!memcmp(pn.xyz.data(), po.xyz.data(), po.xyz.size()*sizeof(float))
               && "existing vertices must never move (monotonic accretion)");
    }
    printf("  pathset append-only OK\n");
}
```

Add to `main()`:

```cpp
    test_path_recorder();
    test_pathset_append_only();
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make -C ParticleFlowLib test`
Expected: FAIL — compile error (`PathRecorder` not declared).

- [ ] **Step 3: Write the implementation**

Append to `ParticleFlowLib/include/particle_flow.h` (after Sim, inside namespace pf):

```cpp
// Records each particle's trajectory into an append-only PathSet.
// Vertices are appended when a particle has moved >= min_segment from its
// last recorded vertex (plus the spawn vertex and the final position at death).
class PathRecorder : public ITickObserver {
public:
    PathRecorder(float min_segment, const std::vector<std::string>& channel_names);
    void on_tick(const Sim& s, uint32_t tick) override;
    const PathSet& paths() const { return set_; }

private:
    struct Track { uint32_t path_index; V3 last; };
    float min_seg_;
    PathSet set_;
    std::vector<Track> by_id_;      // particle id -> track (ids are dense)
    std::vector<uint8_t> known_;    // particle id known?
    void append_vertex(const Sim& s, uint32_t slot, uint32_t path_index);
};

// Unit direction of the last segment ({0,0,0} for single-vertex paths).
V3 path_end_dir(const PathSet::Path& p);
```

Create `ParticleFlowLib/src/pf_path_recorder.cpp`:

```cpp
#include "particle_flow.h"

namespace pf {

PathRecorder::PathRecorder(float min_segment, const std::vector<std::string>& names)
    : min_seg_(min_segment > 0 ? min_segment : 0.0f) {
    set_.channel_names = names;
}

void PathRecorder::append_vertex(const Sim& s, uint32_t slot, uint32_t path_index) {
    const float* pd = s.pos_data();
    PathSet::Path& path = set_.paths[path_index];
    path.xyz.push_back(pd[3*slot]);
    path.xyz.push_back(pd[3*slot+1]);
    path.xyz.push_back(pd[3*slot+2]);
    for (uint32_t c = 0; c < s.channel_count(); ++c)
        path.channels[c].push_back(s.attr_data(c)[slot]);
}

void PathRecorder::on_tick(const Sim& s, uint32_t) {
    const float* pd = s.pos_data();
    const uint8_t* alive = s.alive_data();
    // Ascending slot order = deterministic. New ids start paths; movement
    // appends decimated vertices.
    for (uint32_t slot = 0; slot < s.slot_count(); ++slot) {
        if (!alive[slot]) continue;
        uint32_t id = s.id_of(slot);
        if (id >= known_.size()) { known_.resize(id + 1, 0); by_id_.resize(id + 1); }
        V3 p{pd[3*slot], pd[3*slot+1], pd[3*slot+2]};
        if (!known_[id]) {
            known_[id] = 1;
            PathSet::Path path;
            path.particle_id = id;
            path.channels.resize(s.channel_count());
            set_.paths.push_back(std::move(path));
            by_id_[id] = {(uint32_t)set_.paths.size() - 1, p};
            append_vertex(s, slot, by_id_[id].path_index);
            continue;
        }
        Track& t = by_id_[id];
        if (length(p - t.last) >= min_seg_ && min_seg_ > 0) {
            append_vertex(s, slot, t.path_index);
            t.last = p;
        }
    }
    // Deaths this tick: record the final position and close the path.
    for (uint32_t slot : s.died_this_tick()) {
        uint32_t id = s.id_of(slot);
        if (id >= known_.size() || !known_[id]) continue;
        Track& t = by_id_[id];
        PathSet::Path& path = set_.paths[t.path_index];
        if (path.closed) continue;
        V3 p{pd[3*slot], pd[3*slot+1], pd[3*slot+2]};
        if (length(p - t.last) > 1e-6f) append_vertex(s, slot, t.path_index);
        path.closed = true;
    }
}

V3 path_end_dir(const PathSet::Path& p) {
    size_t n = p.vertex_count();
    if (n < 2) return {0, 0, 0};
    V3 a{p.xyz[3*(n-2)], p.xyz[3*(n-2)+1], p.xyz[3*(n-2)+2]};
    V3 b{p.xyz[3*(n-1)], p.xyz[3*(n-1)+1], p.xyz[3*(n-1)+2]};
    return normalize(b - a);
}

} // namespace pf
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make -C ParticleFlowLib test`
Expected: PASS — `pf_tests: ALL OK`, no ASan reports.

- [ ] **Step 5: CHECKPOINT A — full ParticleFlowLib suite + standalone lib build**

Run: `make -C ParticleFlowLib clean && make -C ParticleFlowLib && make -C ParticleFlowLib test`
Expected: `libparticleflow.a` builds clean (no warnings beyond repo norms), all tests pass under ASan/UBSan. This is the ONLY full-suite point before Task 7.

- [ ] **Step 6: Commit**

```bash
git add ParticleFlowLib/include/particle_flow.h ParticleFlowLib/src/pf_path_recorder.cpp ParticleFlowLib/tests/pf_tests.cpp
git commit -m "feat(particleflow): PathRecorder — append-only PathSet with decimation, death-closing, end directions"
```

---

### Task 5: Engine integration — build wiring, DslState hooks, pf_bindings core

**Files:**
- Modify: `MatterEngine3/Makefile` (INCLUDE_PATHS ~:64-67, ME3_OBJ ~:129, vpath ~:185)
- Modify: `MatterEngine3/src/dsl_state.h` (registry + budget members)
- Modify: `MatterEngine3/src/script_host.cpp` (~:977, after `JS_SetContextOpaque`)
- Create: `MatterEngine3/src/pf_bindings.h`
- Create: `MatterEngine3/src/pf_bindings.cpp`
- Modify: `MatterEngine3/src/dsl_bindings.cpp` (install call at end of `install_bindings`, ~:768-802)
- Modify: `MatterEngine3/tests/Makefile` (INCLUDE_PATHS :57, SCRIPT_CPP ~:337-343)
- Modify: `MatterEngine3/tests/script_host_tests.cpp` (append smoke test)

**Interfaces:**
- Consumes: `pf::Sim`, `pf::SimConfig`, `pf::FieldConfig`, `pf::EmitterConfig`, `pf::PathRecorder`, `pf::PathSet`, `pf::V3` from Tasks 1-4 (`ParticleFlowLib/include/particle_flow.h`).
- Produces: JS globals `__pf_simCreate(cfgObj) -> int`, `__pf_recorderCreate(minSegment, namesArray) -> int`, `__pf_attach(simId, recId)`, `__pf_setAttractors(simId, Float32Array)`, `__pf_run(simId, ticks) -> ticksRun` (chunking/callback added Task 6), `__pf_attractorsRemaining(simId) -> int`, `__pf_depositedCount(simId) -> int`, `__pf_surfaceNormal(simId, x,y,z, radius) -> [nx,ny,nz]|null`, `__pf_pathCount(recId) -> int`, `__pf_path(recId, i) -> {particleId, closed, xyz: Float32Array, channels: {name: Float32Array}}`. C++: `dsl::install_pf_bindings(JSContext*)`; `DslState::set_pf_registry/pf_registry`, `DslState::set_budget/budget_exceeded`. Internal: `dsl::PfRegistry` + `dsl::pf_registry_of(DslState*)` (Task 6/7 bindings reuse these from pf_bindings.cpp — Task 6/7 code is appended to this same file).

- [ ] **Step 1: Wire ParticleFlowLib sources into the MatterEngine3 archive**

In `MatterEngine3/Makefile`:

(a) Near `MSL_DIR = ../MatterSurfaceLib` (~:59) add:

```make
PFL_DIR     = ../ParticleFlowLib
```

(b) In `INCLUDE_PATHS` (~:64-67) append ` -I$(PFL_DIR)/include` to the last line (before `$(EXTRA_INCLUDE)`).

(c) In `ME3_OBJ` (~:129) append a new line to the list:

```make
          pf_math.o pf_sim.o pf_fields.o pf_path_recorder.o pf_bindings.o \
```

(the trailing entry `async_bake.o` stays last; insert this line before `matter_engine.o` or anywhere inside the list — position is cosmetic).

(d) Extend the vpath (~:185):

```make
vpath %.cpp src src/render src/provider $(MSL_DIR)/src $(PFL_DIR)/src
```

No `ar` line change needed — the pf objects ride inside `$(ME3_OBJ)`.

- [ ] **Step 2: Add registry + budget hooks to DslState**

In `MatterEngine3/src/dsl_state.h`: add `#include <chrono>` to the include block (~:5), and inside the `DslState` public section (near `set_error`, ~:284) add:

```cpp
    // --- ParticleFlowLib handle registry (bake-scoped) -----------------------
    // pf_bindings.cpp owns the concrete type; DslState just keeps it alive for
    // the duration of the bake so sim/recorder ids die with the context.
    void set_pf_registry(std::shared_ptr<void> r) { pf_registry_ = std::move(r); }
    void* pf_registry() const { return pf_registry_.get(); }

    // --- Time-budget mirror ---------------------------------------------------
    // script_host stashes the interrupt-handler deadline here so native run
    // loops (which the VM interrupt cannot preempt) can check it between chunks.
    void set_budget(std::chrono::steady_clock::time_point d, bool bounded) {
        budget_deadline_ = d; budget_bounded_ = bounded;
    }
    bool budget_exceeded() const {
        return budget_bounded_ &&
               std::chrono::steady_clock::now() >= budget_deadline_;
    }
```

and in the private member section:

```cpp
    std::shared_ptr<void> pf_registry_;
    std::chrono::steady_clock::time_point budget_deadline_{};
    bool budget_bounded_ = false;
```

- [ ] **Step 3: Stash the budget in script_host.cpp**

In `MatterEngine3/src/script_host.cpp`, immediately after `JS_SetContextOpaque(ctx, &state);` (~:977), add:

```cpp
    state.set_budget(ic.deadline, ic.bounded);
```

(`ic` is the `InterruptCtx` configured ~8 lines above; both bake paths in this file that create a `DslState` + `InterruptCtx` get this line — search for every `JS_SetContextOpaque(ctx, &state)` occurrence and mirror it.)

- [ ] **Step 4: Create pf_bindings.h**

```cpp
#pragma once
#include "quickjs.h"

namespace dsl {
void install_pf_bindings(JSContext* ctx);
}
```

- [ ] **Step 5: Create pf_bindings.cpp (core handles + config parsing + run loop)**

```cpp
#include "pf_bindings.h"
#include "dsl_state.h"
#include "particle_flow.h"

#include <memory>
#include <string>
#include <vector>

namespace dsl {
namespace {

DslState* state_of(JSContext* c) {
    return static_cast<DslState*>(JS_GetContextOpaque(c));
}

double argd(JSContext* c, JSValueConst v) {
    double d = 0; JS_ToFloat64(c, &d, v); return d;
}

} // namespace

// Bake-scoped registry of live sims/recorders. Owned (via shared_ptr<void>)
// by the DslState so handles die with the bake context. Ids are indices;
// nullptr slots would mean "freed" but we never free mid-bake.
struct PfRegistry {
    std::vector<std::unique_ptr<pf::Sim>> sims;
    std::vector<std::unique_ptr<pf::PathRecorder>> recorders;
};

PfRegistry* pf_registry_of(DslState* st) {
    if (!st->pf_registry()) {
        st->set_pf_registry(std::shared_ptr<void>(
            static_cast<void*>(new PfRegistry),
            [](void* p) { delete static_cast<PfRegistry*>(p); }));
    }
    return static_cast<PfRegistry*>(st->pf_registry());
}

namespace {

pf::Sim* sim_of(JSContext* c, DslState* st, JSValueConst idv) {
    int32_t id = -1; JS_ToInt32(c, &id, idv);
    PfRegistry* reg = pf_registry_of(st);
    if (id < 0 || static_cast<size_t>(id) >= reg->sims.size()) {
        st->set_error("particleSim: stale or invalid sim handle");
        return nullptr;
    }
    return reg->sims[static_cast<size_t>(id)].get();
}

pf::PathRecorder* rec_of(JSContext* c, DslState* st, JSValueConst idv) {
    int32_t id = -1; JS_ToInt32(c, &id, idv);
    PfRegistry* reg = pf_registry_of(st);
    if (id < 0 || static_cast<size_t>(id) >= reg->recorders.size()) {
        st->set_error("pathRecorder: stale or invalid recorder handle");
        return nullptr;
    }
    return reg->recorders[static_cast<size_t>(id)].get();
}

// ---- config-object readers (missing key => default) -------------------------
double get_num(JSContext* c, JSValueConst obj, const char* key, double def) {
    JSValue v = JS_GetPropertyStr(c, obj, key);
    double out = def;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) JS_ToFloat64(c, &out, v);
    JS_FreeValue(c, v);
    return out;
}

bool get_bool(JSContext* c, JSValueConst obj, const char* key, bool def) {
    JSValue v = JS_GetPropertyStr(c, obj, key);
    bool out = def;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) out = JS_ToBool(c, v) > 0;
    JS_FreeValue(c, v);
    return out;
}

pf::V3 get_v3(JSContext* c, JSValueConst obj, const char* key, pf::V3 def) {
    JSValue v = JS_GetPropertyStr(c, obj, key);
    pf::V3 out = def;
    if (JS_IsObject(v)) {
        for (uint32_t i = 0; i < 3; ++i) {
            JSValue e = JS_GetPropertyUint32(c, v, i);
            double d = 0; JS_ToFloat64(c, &d, e); JS_FreeValue(c, e);
            (&out.x)[i] = static_cast<float>(d);
        }
    }
    JS_FreeValue(c, v);
    return out;
}

std::string get_str(JSContext* c, JSValueConst obj, const char* key,
                    const char* def) {
    JSValue v = JS_GetPropertyStr(c, obj, key);
    std::string out = def;
    if (JS_IsString(v)) {
        const char* s = JS_ToCString(c, v);
        if (s) { out = s; JS_FreeCString(c, s); }
    }
    JS_FreeValue(c, v);
    return out;
}

bool parse_field(JSContext* c, DslState* st, JSValueConst f,
                 pf::FieldConfig* out) {
    std::string type = get_str(c, f, "type", "");
    if      (type == "bias")     out->type = pf::FieldType::Bias;
    else if (type == "curl")     out->type = pf::FieldType::Curl;
    else if (type == "adhere")   out->type = pf::FieldType::Adhere;
    else if (type == "attract")  out->type = pf::FieldType::Attract;
    else if (type == "separate") out->type = pf::FieldType::Separate;
    else if (type == "drag")     out->type = pf::FieldType::Drag;
    else {
        st->set_error("particleSim: unknown field type '" + type + "'");
        return false;
    }
    std::string mode = get_str(c, f, "mode", "steer");
    out->mode = (mode == "force") ? pf::FieldMode::Force : pf::FieldMode::Steer;
    out->weight         = static_cast<float>(get_num(c, f, "weight", 1.0));
    out->dir            = get_v3(c, f, "dir", {0, 1, 0});
    out->radius         = static_cast<float>(get_num(c, f, "radius", 0.5));
    out->surface_offset = static_cast<float>(get_num(c, f, "surfaceOffset", 0.0));
    out->influence      = static_cast<float>(get_num(c, f, "influence", 1.0));
    out->kill_radius    = static_cast<float>(get_num(c, f, "killRadius", 0.1));
    out->kill_on_consume= get_bool(c, f, "killOnConsume", false);
    out->scale          = static_cast<float>(get_num(c, f, "scale", 1.0));
    out->seed           = static_cast<uint64_t>(get_num(c, f, "seed", 0.0));
    out->k              = static_cast<float>(get_num(c, f, "k", 1.0));
    JSValue fade = JS_GetPropertyStr(c, f, "fade");
    if (JS_IsObject(fade)) {
        out->fade.enabled = true;
        std::string ax = get_str(c, fade, "axis", "y");
        out->fade.axis = (ax == "x") ? 0 : (ax == "z") ? 2 : 1;
        out->fade.from = static_cast<float>(get_num(c, fade, "from", 0.0));
        out->fade.to   = static_cast<float>(get_num(c, fade, "to", 1.0));
    }
    JS_FreeValue(c, fade);
    return true;
}

void parse_emitter(JSContext* c, JSValueConst e, pf::EmitterConfig* out) {
    std::string shape = get_str(c, e, "shape", "point");
    out->shape  = (shape == "disc") ? 1 : (shape == "ring") ? 2 : 0;
    out->center = get_v3(c, e, "center", {0, 0, 0});
    out->axis   = get_v3(c, e, "axis", {0, 1, 0});
    out->radius = static_cast<float>(get_num(c, e, "radius", 0.0));
    out->rate   = static_cast<float>(get_num(c, e, "rate", 1.0));
    out->vel0   = get_v3(c, e, "vel0", {0, 1, 0});
    out->jitter = static_cast<float>(get_num(c, e, "jitter", 0.0));
    JSValue ai = JS_GetPropertyStr(c, e, "attrInit");
    if (JS_IsObject(ai)) {
        JSValue len = JS_GetPropertyStr(c, ai, "length");
        uint32_t n = 0; JS_ToUint32(c, &n, len); JS_FreeValue(c, len);
        for (uint32_t i = 0; i < n; ++i) {
            JSValue x = JS_GetPropertyUint32(c, ai, i);
            double d = 0; JS_ToFloat64(c, &d, x); JS_FreeValue(c, x);
            out->attr_init.push_back(static_cast<float>(d));
        }
    }
    JS_FreeValue(c, ai);
}

// __pf_simCreate(cfgObj) -> id
JSValue j_pf_simCreate(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    DslState* st = state_of(c);
    if (n < 1 || !JS_IsObject(a[0])) {
        st->set_error("particleSim: config object required");
        return JS_NewInt32(c, -1);
    }
    pf::SimConfig cfg;
    cfg.seed          = static_cast<uint64_t>(get_num(c, a[0], "seed", 1.0));
    cfg.dt            = static_cast<float>(get_num(c, a[0], "dt", 1.0));
    cfg.max_turn_rate = static_cast<float>(get_num(c, a[0], "maxTurnRate", 0.2));
    cfg.speed_target  = static_cast<float>(get_num(c, a[0], "speedTarget", -1.0)); // <0 = off
    cfg.speed_relax   = static_cast<float>(get_num(c, a[0], "speedRelax", 0.1));
    cfg.deposit_every = static_cast<float>(get_num(c, a[0], "depositEvery", 0.05));
    cfg.max_age       = static_cast<uint32_t>(get_num(c, a[0], "maxAge", 0.0));
    cfg.max_particles = static_cast<uint32_t>(get_num(c, a[0], "maxParticles", 4096.0));
    cfg.hash_cell     = static_cast<float>(get_num(c, a[0], "hashCell", 0.25));
    JSValue attrs = JS_GetPropertyStr(c, a[0], "attributes");
    if (JS_IsObject(attrs)) {
        JSValue len = JS_GetPropertyStr(c, attrs, "length");
        uint32_t na = 0; JS_ToUint32(c, &na, len); JS_FreeValue(c, len);
        for (uint32_t i = 0; i < na; ++i) {
            JSValue s = JS_GetPropertyUint32(c, attrs, i);
            const char* cs = JS_ToCString(c, s);
            if (cs) { cfg.attributes.push_back(cs); JS_FreeCString(c, cs); }
            JS_FreeValue(c, s);
        }
    }
    JS_FreeValue(c, attrs);
    JSValue ems = JS_GetPropertyStr(c, a[0], "emitters");
    if (JS_IsObject(ems)) {
        JSValue len = JS_GetPropertyStr(c, ems, "length");
        uint32_t ne = 0; JS_ToUint32(c, &ne, len); JS_FreeValue(c, len);
        for (uint32_t i = 0; i < ne; ++i) {
            JSValue e = JS_GetPropertyUint32(c, ems, i);
            pf::EmitterConfig ec; parse_emitter(c, e, &ec);
            cfg.emitters.push_back(std::move(ec));
            JS_FreeValue(c, e);
        }
    }
    JS_FreeValue(c, ems);
    JSValue flds = JS_GetPropertyStr(c, a[0], "fields");
    if (JS_IsObject(flds)) {
        JSValue len = JS_GetPropertyStr(c, flds, "length");
        uint32_t nf = 0; JS_ToUint32(c, &nf, len); JS_FreeValue(c, len);
        for (uint32_t i = 0; i < nf; ++i) {
            JSValue f = JS_GetPropertyUint32(c, flds, i);
            pf::FieldConfig fc;
            bool ok = parse_field(c, st, f, &fc);
            JS_FreeValue(c, f);
            if (!ok) { JS_FreeValue(c, flds); return JS_NewInt32(c, -1); }
            cfg.fields.push_back(std::move(fc));
        }
    }
    JS_FreeValue(c, flds);
    PfRegistry* reg = pf_registry_of(st);
    reg->sims.push_back(std::make_unique<pf::Sim>(cfg));
    return JS_NewInt32(c, static_cast<int32_t>(reg->sims.size() - 1));
}

// __pf_recorderCreate(minSegment, channelNamesArray) -> id
JSValue j_pf_recorderCreate(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    DslState* st = state_of(c);
    float min_seg = (n >= 1) ? static_cast<float>(argd(c, a[0])) : 0.0f;
    std::vector<std::string> names;
    if (n >= 2 && JS_IsObject(a[1])) {
        JSValue len = JS_GetPropertyStr(c, a[1], "length");
        uint32_t nn = 0; JS_ToUint32(c, &nn, len); JS_FreeValue(c, len);
        for (uint32_t i = 0; i < nn; ++i) {
            JSValue s = JS_GetPropertyUint32(c, a[1], i);
            const char* cs = JS_ToCString(c, s);
            if (cs) { names.push_back(cs); JS_FreeCString(c, cs); }
            JS_FreeValue(c, s);
        }
    }
    PfRegistry* reg = pf_registry_of(st);
    reg->recorders.push_back(std::make_unique<pf::PathRecorder>(min_seg, names));
    return JS_NewInt32(c, static_cast<int32_t>(reg->recorders.size() - 1));
}

// __pf_attach(simId, recId)
JSValue j_pf_attach(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    DslState* st = state_of(c);
    if (n < 2) { st->set_error("pf.attach: (simId, recId) required"); return JS_UNDEFINED; }
    pf::Sim* sim = sim_of(c, st, a[0]);
    pf::PathRecorder* rec = rec_of(c, st, a[1]);
    if (sim && rec) sim->attach(rec);
    return JS_UNDEFINED;
}

// __pf_setAttractors(simId, Float32Array of xyz triplets)
JSValue j_pf_setAttractors(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    DslState* st = state_of(c);
    if (n < 2) { st->set_error("pf.setAttractors: (simId, Float32Array) required"); return JS_UNDEFINED; }
    pf::Sim* sim = sim_of(c, st, a[0]);
    if (!sim) return JS_UNDEFINED;
    size_t byte_off = 0, byte_len = 0, bpe = 0;
    JSValue buf = JS_GetTypedArrayBuffer(c, a[1], &byte_off, &byte_len, &bpe);
    if (JS_IsException(buf) || bpe != 4 ||
        JS_GetTypedArrayType(a[1]) != JS_TYPED_ARRAY_FLOAT32) {
        JS_FreeValue(c, buf);
        st->set_error("pf.setAttractors: expected a Float32Array");
        return JS_UNDEFINED;
    }
    size_t abuf_len = 0;
    uint8_t* raw = JS_GetArrayBuffer(c, &abuf_len, buf);
    JS_FreeValue(c, buf);
    if (!raw) { st->set_error("pf.setAttractors: detached buffer"); return JS_UNDEFINED; }
    const float* f = reinterpret_cast<const float*>(raw + byte_off);
    size_t count = (byte_len / 4) / 3;
    sim->set_attractors(f, count);   // kernel copies; appends count xyz points
    return JS_UNDEFINED;
}

// __pf_run(simId, ticks) -> ticks actually run. Checks the bake time budget
// every RUN_CHUNK ticks (the VM interrupt handler cannot preempt native code).
// Task 6 extends this signature with (every, onTick) for zero-copy views.
constexpr uint32_t RUN_CHUNK = 32;
JSValue j_pf_run(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    DslState* st = state_of(c);
    if (n < 2) { st->set_error("pf.run: (simId, ticks) required"); return JS_NewInt32(c, 0); }
    pf::Sim* sim = sim_of(c, st, a[0]);
    if (!sim) return JS_NewInt32(c, 0);
    uint32_t ticks = static_cast<uint32_t>(argd(c, a[1]));
    uint32_t done = 0;
    while (done < ticks) {
        if (st->budget_exceeded()) {
            st->set_error("pf.run: bake time budget exceeded mid-simulation");
            break;
        }
        uint32_t chunk = std::min(RUN_CHUNK, ticks - done);
        for (uint32_t i = 0; i < chunk; ++i) sim->step();
        done += chunk;
    }
    return JS_NewInt32(c, static_cast<int32_t>(done));
}

JSValue j_pf_attractorsRemaining(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    DslState* st = state_of(c);
    pf::Sim* sim = (n >= 1) ? sim_of(c, st, a[0]) : nullptr;
    return JS_NewInt32(c, sim ? static_cast<int32_t>(sim->attractors_remaining()) : 0);
}

JSValue j_pf_depositedCount(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    DslState* st = state_of(c);
    pf::Sim* sim = (n >= 1) ? sim_of(c, st, a[0]) : nullptr;
    return JS_NewInt32(c, sim ? static_cast<int32_t>(sim->deposited_count()) : 0);
}

// __pf_surfaceNormal(simId, x, y, z, radius) -> [nx,ny,nz] | null
JSValue j_pf_surfaceNormal(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    DslState* st = state_of(c);
    if (n < 5) return JS_NULL;
    pf::Sim* sim = sim_of(c, st, a[0]);
    if (!sim) return JS_NULL;
    pf::V3 p{static_cast<float>(argd(c, a[1])), static_cast<float>(argd(c, a[2])),
             static_cast<float>(argd(c, a[3]))};
    bool ok = false;
    pf::V3 nrm = sim->surface_normal(p, static_cast<float>(argd(c, a[4])), &ok);
    if (!ok) return JS_NULL;
    JSValue arr = JS_NewArray(c);
    JS_SetPropertyUint32(c, arr, 0, JS_NewFloat64(c, nrm.x));
    JS_SetPropertyUint32(c, arr, 1, JS_NewFloat64(c, nrm.y));
    JS_SetPropertyUint32(c, arr, 2, JS_NewFloat64(c, nrm.z));
    return arr;
}

JSValue j_pf_pathCount(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    DslState* st = state_of(c);
    pf::PathRecorder* rec = (n >= 1) ? rec_of(c, st, a[0]) : nullptr;
    return JS_NewInt32(c, rec ? static_cast<int32_t>(rec->paths().paths.size()) : 0);
}

// Copy a float vector out as a fresh Float32Array (JS owns the copy).
JSValue f32_copy(JSContext* c, const std::vector<float>& v) {
    JSValue buf = JS_NewArrayBufferCopy(
        c, reinterpret_cast<const uint8_t*>(v.data()), v.size() * sizeof(float));
    JSValue argv[1] = {buf};
    JSValue ta = JS_NewTypedArray(c, 1, argv, JS_TYPED_ARRAY_FLOAT32);
    JS_FreeValue(c, buf);
    return ta;
}

// __pf_path(recId, i) -> {particleId, closed, xyz, channels:{name: Float32Array}}
JSValue j_pf_path(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    DslState* st = state_of(c);
    if (n < 2) return JS_NULL;
    pf::PathRecorder* rec = rec_of(c, st, a[0]);
    if (!rec) return JS_NULL;
    int32_t i = -1; JS_ToInt32(c, &i, a[1]);
    const pf::PathSet& ps = rec->paths();
    if (i < 0 || static_cast<size_t>(i) >= ps.paths.size()) return JS_NULL;
    const pf::PathSet::Path& p = ps.paths[static_cast<size_t>(i)];
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "particleId", JS_NewInt64(c, p.particle_id));
    JS_SetPropertyStr(c, o, "closed", JS_NewBool(c, p.closed));
    JS_SetPropertyStr(c, o, "xyz", f32_copy(c, p.xyz));
    JSValue ch = JS_NewObject(c);
    for (size_t k = 0; k < ps.channel_names.size() && k < p.channels.size(); ++k)
        JS_SetPropertyStr(c, ch, ps.channel_names[k].c_str(), f32_copy(c, p.channels[k]));
    JS_SetPropertyStr(c, o, "channels", ch);
    return o;
}

} // namespace

void install_pf_bindings(JSContext* ctx) {
    JSValue g = JS_GetGlobalObject(ctx);
    auto bind = [&](const char* n, JSCFunction* f, int argc) {
        JS_SetPropertyStr(ctx, g, n, JS_NewCFunction(ctx, f, n, argc));
    };
    bind("__pf_simCreate", j_pf_simCreate, 1);
    bind("__pf_recorderCreate", j_pf_recorderCreate, 2);
    bind("__pf_attach", j_pf_attach, 2);
    bind("__pf_setAttractors", j_pf_setAttractors, 2);
    bind("__pf_run", j_pf_run, 4);
    bind("__pf_attractorsRemaining", j_pf_attractorsRemaining, 1);
    bind("__pf_depositedCount", j_pf_depositedCount, 1);
    bind("__pf_surfaceNormal", j_pf_surfaceNormal, 5);
    bind("__pf_pathCount", j_pf_pathCount, 1);
    bind("__pf_path", j_pf_path, 2);
    JS_FreeValue(ctx, g);
}

} // namespace dsl
```

NOTE: this file needs `#include <algorithm>` for `std::min` — add it to the include block. The kernel names used here are `Sim::attach(ITickObserver*)` and `Sim::set_attractors(const float* xyz, size_t n)`; if the built kernel header differs, adjust THIS file to match it — the kernel is the source of truth.

- [ ] **Step 6: Install from dsl_bindings.cpp**

In `MatterEngine3/src/dsl_bindings.cpp`: add `#include "pf_bindings.h"` near the top includes, and inside `install_bindings` (just before `JS_FreeValue(ctx,g);` at the end, ~:800) add:

```cpp
    install_pf_bindings(ctx);
```

- [ ] **Step 7: Build the archive to verify compile + link**

Run: `make -C MatterEngine3`
Expected: `libmatter_engine3.a` builds with pf_*.o members, no new warnings.

- [ ] **Step 8: Wire the test target and add a smoke test**

In `MatterEngine3/tests/Makefile`:

(a) Append ` -I../../ParticleFlowLib/include` to `INCLUDE_PATHS` (:57).

(b) Extend `SCRIPT_CPP` (~:337-343) — add to the list:

```make
             ../src/pf_bindings.cpp \
             ../../ParticleFlowLib/src/pf_math.cpp ../../ParticleFlowLib/src/pf_sim.cpp \
             ../../ParticleFlowLib/src/pf_fields.cpp ../../ParticleFlowLib/src/pf_path_recorder.cpp \
```

(the mangle/obj_of machinery handles `../../` paths automatically).

Append to `MatterEngine3/tests/script_host_tests.cpp` (follow the file's existing test-function + registration convention — read a neighboring test first and mirror it):

```cpp
static void test_pf_bindings_smoke() {
    const char* src = R"JS(
class P extends Part {
  build(p) {
    const sim = __pf_simCreate({
      seed: 7, dt: 1.0, maxTurnRate: 0.5, depositEvery: 0.05,
      maxParticles: 64, hashCell: 0.25, maxAge: 40,
      emitters: [{ shape: 'point', center: [0,0,0], rate: 2, vel0: [0,0.05,0], jitter: 0.2 }],
      fields: [{ type: 'bias', dir: [0,1,0], weight: 0.5 }],
    });
    const rec = __pf_recorderCreate(0.02, []);
    __pf_attach(sim, rec);
    const ran = __pf_run(sim, 60);
    if (ran !== 60) throw new Error('run returned ' + ran);
    if (__pf_depositedCount(sim) < 10) throw new Error('too few deposits');
    if (__pf_pathCount(rec) < 1) throw new Error('no paths recorded');
    const path = __pf_path(rec, 0);
    if (!(path.xyz instanceof Float32Array)) throw new Error('xyz not Float32Array');
    if (path.xyz.length < 6) throw new Error('path too short');
  }
}
)JS";
    // Bake it twice; identical deposited counts double as a cheap determinism probe.
    // Use the file's standard bake-a-source helper; assert no script error.
}
```

(The comment block is the contract; implement the body with whatever helper `script_host_tests.cpp` already uses to bake an inline class source — e.g. the same call pattern as the nearest `test_*` that evaluates a `class ... extends Part` string. Assert: bake succeeds, no `set_error`, and a second bake with the same source succeeds.)

- [ ] **Step 9: Run ONLY the script-host suite**

Run: `make -C MatterEngine3/tests run-script`
Expected: PASS including the new pf smoke test. Do NOT run other suites (checkpoint discipline).

- [ ] **Step 10: Commit**

```bash
git add MatterEngine3/Makefile MatterEngine3/src/dsl_state.h MatterEngine3/src/script_host.cpp \
        MatterEngine3/src/pf_bindings.h MatterEngine3/src/pf_bindings.cpp \
        MatterEngine3/src/dsl_bindings.cpp MatterEngine3/tests/Makefile MatterEngine3/tests/script_host_tests.cpp
git commit -m "feat(pf): ParticleFlowLib wired into engine — __pf_* bindings, DslState registry + budget mirror"
```

---

### Task 6: onTick zero-copy views + live sim control verbs

**Files:**
- Modify: `MatterEngine3/src/pf_bindings.cpp` (extend `j_pf_run`; add view builder + 3 verbs)
- Modify: `MatterEngine3/tests/script_host_tests.cpp` (append onTick test)

**Interfaces:**
- Consumes: `pf::Sim` accessors from Task 2 — `slot_count()`, `tick()`, `pos_data()` (float*, 3 per slot), `vel_data()` (float*, 3 per slot), `alive_data()` (uint8_t*, 1 per slot), `attr_data(ch)` (float*, 1 per slot), `channel_count()`, `config().attributes`, `emit_particle(V3 pos, V3 vel, const float* attr_or_null)`, `kill(slot)`, `set_field_weight(i, w)`, `field_count()`.
- Produces: extended `__pf_run(simId, ticks, every, onTick) -> ticksRun`; `onTick(view)` receives `{count, tick, pos: Float32Array, vel: Float32Array, alive: Uint8Array, attrs: {name: Float32Array}}` — views are ZERO-COPY into sim SoA buffers and are DETACHED after the callback returns (touching a saved view later throws). `onTick` returning `false` stops the run early. New globals: `__pf_emit(simId, emitterCfgObj)`, `__pf_kill(simId, slot)`, `__pf_setFieldWeight(simId, fieldIndex, weight)`.

- [ ] **Step 1: Add the view builder and rewrite j_pf_run**

In `pf_bindings.cpp`, replace the Task 5 `j_pf_run` with the version below, and add the helper above it (inside the anonymous namespace):

```cpp
// Wrap a raw sim buffer as a typed array WITHOUT copying. free_func = nullptr:
// QuickJS does not own the memory; we detach the buffer after the callback so
// JS can never touch freed/moved sim storage. Returns {ta, buf} — caller must
// detach buf and free both values.
struct RawView { JSValue ta; JSValue buf; };

RawView raw_view(JSContext* c, void* data, size_t bytes, JSTypedArrayEnum kind) {
    JSValue buf = JS_NewArrayBuffer(c, static_cast<uint8_t*>(data), bytes,
                                    /*free_func*/ nullptr, /*opaque*/ nullptr,
                                    /*is_shared*/ false);
    JSValue argv[1] = {buf};
    JSValue ta = JS_NewTypedArray(c, 1, argv, kind);
    return {ta, buf};
}

// Build the per-tick view object; collect its buffers for post-callback detach.
JSValue build_tick_view(JSContext* c, pf::Sim* sim, std::vector<JSValue>* bufs) {
    JSValue o = JS_NewObject(c);
    size_t slots = sim->slot_count();
    JS_SetPropertyStr(c, o, "count", JS_NewInt64(c, static_cast<int64_t>(slots)));
    JS_SetPropertyStr(c, o, "tick", JS_NewInt64(c, sim->tick()));
    RawView pos = raw_view(c, sim->pos_data(), slots * 3 * sizeof(float),
                           JS_TYPED_ARRAY_FLOAT32);
    JS_SetPropertyStr(c, o, "pos", pos.ta); bufs->push_back(pos.buf);
    RawView vel = raw_view(c, sim->vel_data(), slots * 3 * sizeof(float),
                           JS_TYPED_ARRAY_FLOAT32);
    JS_SetPropertyStr(c, o, "vel", vel.ta); bufs->push_back(vel.buf);
    RawView alv = raw_view(c, sim->alive_data(), slots * sizeof(uint8_t),
                           JS_TYPED_ARRAY_UINT8);
    JS_SetPropertyStr(c, o, "alive", alv.ta); bufs->push_back(alv.buf);
    JSValue attrs = JS_NewObject(c);
    for (size_t ch = 0; ch < sim->channel_count(); ++ch) {
        RawView av = raw_view(c, sim->attr_data(ch), slots * sizeof(float),
                              JS_TYPED_ARRAY_FLOAT32);
        JS_SetPropertyStr(c, attrs, sim->config().attributes[ch].c_str(), av.ta);
        bufs->push_back(av.buf);
    }
    JS_SetPropertyStr(c, o, "attrs", attrs);
    return o;
}

// __pf_run(simId, ticks, every?, onTick?) -> ticks actually run.
// Chunk size = min(every, RUN_CHUNK-capped remainder); budget checked per chunk.
JSValue j_pf_run(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    DslState* st = state_of(c);
    if (n < 2) { st->set_error("pf.run: (simId, ticks) required"); return JS_NewInt32(c, 0); }
    pf::Sim* sim = sim_of(c, st, a[0]);
    if (!sim) return JS_NewInt32(c, 0);
    uint32_t ticks = static_cast<uint32_t>(argd(c, a[1]));
    uint32_t every = (n >= 3 && !JS_IsUndefined(a[2]))
                         ? static_cast<uint32_t>(argd(c, a[2])) : 0;
    JSValueConst on_tick = (n >= 4 && JS_IsFunction(c, a[3])) ? a[3] : JS_UNDEFINED;
    bool has_cb = every > 0 && !JS_IsUndefined(on_tick);

    uint32_t done = 0;
    while (done < ticks) {
        if (st->budget_exceeded()) {
            st->set_error("pf.run: bake time budget exceeded mid-simulation");
            break;
        }
        uint32_t chunk = std::min(has_cb ? every : RUN_CHUNK, ticks - done);
        for (uint32_t i = 0; i < chunk; ++i) sim->step();
        done += chunk;
        if (has_cb) {
            std::vector<JSValue> bufs;
            JSValue view = build_tick_view(c, sim, &bufs);
            JSValue arg[1] = {view};
            JSValue r = JS_Call(c, on_tick, JS_UNDEFINED, 1, arg);
            for (JSValue b : bufs) { JS_DetachArrayBuffer(c, b); JS_FreeValue(c, b); }
            JS_FreeValue(c, view);
            if (JS_IsException(r)) { JS_FreeValue(c, r); return JS_EXCEPTION; }
            bool stop = JS_IsBool(r) && JS_ToBool(c, r) == 0;
            JS_FreeValue(c, r);
            if (stop) break;
        }
    }
    return JS_NewInt32(c, static_cast<int32_t>(done));
}
```

NOTE: verify `JS_DetachArrayBuffer`'s exact signature in `../Libraries/quickjs-ng/quickjs.h` (~:947-983) — some versions return int; adjust the call if so. `JS_TYPED_ARRAY_UINT8` likewise. If `Sim::tick()` returns uint32_t, the `JS_NewInt64` cast is fine.

- [ ] **Step 2: Add the control verbs**

Still in the anonymous namespace:

```cpp
// __pf_emit(simId, cfgObj) — one-shot manual emission. cfg reuses emitter keys:
// { center: [x,y,z], vel0: [x,y,z], attrInit: [floats] } (shape/rate/etc ignored).
JSValue j_pf_emit(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    DslState* st = state_of(c);
    if (n < 2 || !JS_IsObject(a[1])) { st->set_error("pf.emit: (simId, cfg) required"); return JS_UNDEFINED; }
    pf::Sim* sim = sim_of(c, st, a[0]);
    if (!sim) return JS_UNDEFINED;
    pf::EmitterConfig ec; parse_emitter(c, a[1], &ec);
    sim->emit_particle(ec.center, ec.vel0,
                       ec.attr_init.empty() ? nullptr : ec.attr_init.data());
    return JS_UNDEFINED;
}

JSValue j_pf_kill(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    DslState* st = state_of(c);
    if (n < 2) return JS_UNDEFINED;
    pf::Sim* sim = sim_of(c, st, a[0]);
    if (sim) sim->kill(static_cast<uint32_t>(argd(c, a[1])));
    return JS_UNDEFINED;
}

JSValue j_pf_setFieldWeight(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    DslState* st = state_of(c);
    if (n < 3) return JS_UNDEFINED;
    pf::Sim* sim = sim_of(c, st, a[0]);
    if (!sim) return JS_UNDEFINED;
    uint32_t i = static_cast<uint32_t>(argd(c, a[1]));
    if (i >= sim->field_count()) { st->set_error("pf.setFieldWeight: field index out of range"); return JS_UNDEFINED; }
    sim->set_field_weight(i, static_cast<float>(argd(c, a[2])));
    return JS_UNDEFINED;
}
```

And in `install_pf_bindings` add:

```cpp
    bind("__pf_emit", j_pf_emit, 2);
    bind("__pf_kill", j_pf_kill, 2);
    bind("__pf_setFieldWeight", j_pf_setFieldWeight, 3);
```

- [ ] **Step 3: Append the onTick test to script_host_tests.cpp**

Same bake-helper pattern as Task 5's smoke test; JS body:

```js
class P extends Part {
  build(p) {
    const sim = __pf_simCreate({
      seed: 3, dt: 1.0, maxTurnRate: 0.5, depositEvery: 0.05,
      maxParticles: 32, hashCell: 0.25, maxAge: 0,
      attributes: ['thickness'],
      emitters: [{ shape: 'point', center: [0,0,0], rate: 1, vel0: [0,0.05,0],
                   jitter: 0, attrInit: [0.5] }],
      fields: [{ type: 'bias', dir: [0,1,0], weight: 0.3 }],
    });
    let calls = 0, sawAlive = false, savedView = null;
    const ran = __pf_run(sim, 50, 10, (v) => {
      ++calls;
      if (!(v.pos instanceof Float32Array)) throw new Error('pos not F32');
      if (!(v.attrs.thickness instanceof Float32Array)) throw new Error('no attr view');
      for (let i = 0; i < v.count; ++i) if (v.alive[i]) { sawAlive = true; break; }
      savedView = v;                    // detached after return — probed below
      if (calls === 3) __pf_setFieldWeight(sim, 0, 0.0);
      return calls < 4;                 // early stop after 4th callback
    });
    if (calls !== 4) throw new Error('expected 4 callbacks, got ' + calls);
    if (ran !== 40) throw new Error('early stop should yield 40 ticks, got ' + ran);
    if (!sawAlive) throw new Error('no alive particles seen');
    let detached = false;
    try { const x = savedView.pos[0]; if (savedView.pos.length === 0) detached = true; }
    catch (e) { detached = true; }
    if (!detached && savedView.pos.length !== 0) throw new Error('view not detached');
  }
}
```

(Detached typed arrays in QuickJS-ng report `length === 0`; the try/catch keeps the assertion robust either way.)

- [ ] **Step 4: Rebuild + run ONLY run-script**

Run: `make -C MatterEngine3 && make -C MatterEngine3/tests run-script`
Expected: PASS (both pf tests). No other suites.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/pf_bindings.cpp MatterEngine3/tests/script_host_tests.cpp
git commit -m "feat(pf): onTick zero-copy SoA views (detach-after-call) + emit/kill/setFieldWeight verbs"
```

---

### Task 7: `this.paths()` voxel sink + shared-lib/particleflow.js + CHECKPOINT B

**Files:**
- Modify: `MatterEngine3/src/pf_bindings.cpp` (add `j_pf_stampPaths`)
- Modify: `MatterEngine3/src/part_base.js.h` (add `paths()` to the `Part` class)
- Create: `MatterEngine3/shared-lib/particleflow.js`
- Modify: `MatterEngine3/tests/script_host_tests.cpp` (append stamp test)

**Interfaces:**
- Consumes: `DslState::session()` (`Session::Voxels`), `DslState::cone(a,b,r0,r1,CsgOp)` (tapered brush, world transform captured from stack top), `DslState::sphere(c,r,CsgOp)`, `DslState::set_error`; `pf::PathRecorder::paths()`; `pf::PathSet`.
- Produces: global `__pf_stampPaths(recId, optsObj)`; JS `Part.paths(recId, opts)` where `opts = { radiusChannel?: string (default 'thickness'), minRadius?: number (default 0.01), radiusScale?: number (default 1.0), filter?: int[] (path indices; default all) }`; `shared-lib/particleflow.js` exporting `ParticleSim` and `PathRecorder` wrapper classes (exact API in Step 4 — Tasks 8-9 import these).

- [ ] **Step 1: Add j_pf_stampPaths to pf_bindings.cpp**

Requires `#include "csg_lowering.h"` or wherever `CsgOp` lives — check `dsl_state.h`'s own includes and mirror how `j_capsule` in `dsl_bindings.cpp` (~:212-215) names the op enum; use the same `CsgOp::Union`-equivalent spelling found there.

```cpp
// __pf_stampPaths(recId, opts) — feed recorded paths into the OPEN voxel
// session: one tapered cone brush per segment + a sphere per vertex so joints
// are rounded. Radii come from a recorded channel (default 'thickness'),
// floored at minRadius, scaled by radiusScale.
JSValue j_pf_stampPaths(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    DslState* st = state_of(c);
    if (st->session() != DslState::Session::Voxels) {
        st->set_error("paths() outside an open voxel session");
        return JS_UNDEFINED;
    }
    if (n < 1) { st->set_error("paths(): recorder handle required"); return JS_UNDEFINED; }
    pf::PathRecorder* rec = rec_of(c, st, a[0]);
    if (!rec) return JS_UNDEFINED;
    JSValueConst opts = (n >= 2 && JS_IsObject(a[1])) ? a[1] : JS_UNDEFINED;

    std::string rch = "thickness";
    double min_r = 0.01, r_scale = 1.0;
    std::vector<int32_t> filter;
    if (!JS_IsUndefined(opts)) {
        rch     = get_str(c, opts, "radiusChannel", rch.c_str());
        min_r   = get_num(c, opts, "minRadius", min_r);
        r_scale = get_num(c, opts, "radiusScale", r_scale);
        JSValue f = JS_GetPropertyStr(c, opts, "filter");
        if (JS_IsObject(f)) {
            JSValue len = JS_GetPropertyStr(c, f, "length");
            uint32_t nf = 0; JS_ToUint32(c, &nf, len); JS_FreeValue(c, len);
            for (uint32_t i = 0; i < nf; ++i) {
                JSValue e = JS_GetPropertyUint32(c, f, i);
                int32_t idx = -1; JS_ToInt32(c, &idx, e); JS_FreeValue(c, e);
                filter.push_back(idx);
            }
        }
        JS_FreeValue(c, f);
    }

    const pf::PathSet& ps = rec->paths();
    int ch = -1;
    for (size_t k = 0; k < ps.channel_names.size(); ++k)
        if (ps.channel_names[k] == rch) { ch = static_cast<int>(k); break; }

    auto stamp_path = [&](const pf::PathSet::Path& p) {
        size_t nv = p.vertex_count();
        if (nv == 0) return;
        auto radius_at = [&](size_t i) -> float {
            float r = (ch >= 0 && i < p.channels[ch].size())
                          ? p.channels[ch][i] : static_cast<float>(min_r);
            r *= static_cast<float>(r_scale);
            return std::max(r, static_cast<float>(min_r));
        };
        auto vert = [&](size_t i) {
            return Vector3{p.xyz[3*i], p.xyz[3*i+1], p.xyz[3*i+2]};
        };
        st->sphere(vert(0), radius_at(0), CsgOp::Union);
        for (size_t i = 1; i < nv; ++i) {
            st->cone(vert(i - 1), vert(i), radius_at(i - 1), radius_at(i), CsgOp::Union);
            st->sphere(vert(i), radius_at(i), CsgOp::Union);
        }
    };

    if (filter.empty()) {
        for (const auto& p : ps.paths) stamp_path(p);
    } else {
        for (int32_t idx : filter)
            if (idx >= 0 && static_cast<size_t>(idx) < ps.paths.size())
                stamp_path(ps.paths[static_cast<size_t>(idx)]);
    }
    return JS_UNDEFINED;
}
```

Register in `install_pf_bindings`:

```cpp
    bind("__pf_stampPaths", j_pf_stampPaths, 2);
```

ADAPT: the exact parameter types of `DslState::sphere/cone` (Vector3 vs float triples) and the `CsgOp` spelling MUST be read from `dsl_state.h` (~:100-230) before writing this — match the real signatures; the code above assumes `sphere(Vector3, float, CsgOp)` / `cone(Vector3, Vector3, float, float, CsgOp)`. Also `#include <algorithm>` is already present from Task 5 (`std::max` needs it).

- [ ] **Step 2: Add Part.paths() to part_base.js.h**

In `MatterEngine3/src/part_base.js.h`, inside the `Part` class (after `endVoxels()`):

```js
  paths(rec,opts)        { __pf_stampPaths((rec&&rec.__id!==undefined)?rec.__id:rec, opts); }
```

(accepts either a raw recorder id or a `PathRecorder` wrapper from particleflow.js).

- [ ] **Step 3: Verify shared-lib module loading convention**

Read `MatterEngine3/shared-lib/rng.js` (short) and check how `lsystem.js` exports classes — mirror the same `export` style in the next step. Check how shared-lib modules are registered/folded (grep `shared-lib` in `module_resolver.cpp`) — if there is a manifest/list of module names, add `particleflow` (and note: Task 8 adds `strands`).

- [ ] **Step 4: Create shared-lib/particleflow.js**

```js
// Thin wrappers over the __pf_* native bindings so schemas get an ergonomic,
// documented API. All heavy state lives in C++; these classes only hold ids.

export class PathRecorder {
  // channels: array of attribute names to record per-vertex (e.g. ['thickness'])
  constructor(minSegment, channels) {
    this.__id = __pf_recorderCreate(minSegment, channels || []);
  }
  get count() { return __pf_pathCount(this.__id); }
  path(i) { return __pf_path(this.__id, i); }         // copied {xyz, channels,...}
  forEach(fn) { const n = this.count; for (let i = 0; i < n; ++i) fn(this.path(i), i); }
}

export class ParticleSim {
  // cfg: see __pf_simCreate — {seed, dt, maxTurnRate, speedTarget, speedRelax,
  //   depositEvery, maxAge, maxParticles, hashCell, attributes: [names],
  //   emitters: [{shape:'point'|'disc'|'ring', center, axis, radius, rate,
  //               vel0, jitter, attrInit}],
  //   fields: [{type:'bias'|'curl'|'adhere'|'attract'|'separate'|'drag',
  //             mode:'steer'|'force', weight, dir, radius, surfaceOffset,
  //             influence, killRadius, killOnConsume, scale, seed, k,
  //             fade:{axis:'x'|'y'|'z', from, to}}]}
  constructor(cfg) { this.__id = __pf_simCreate(cfg); }
  attach(recorder) { __pf_attach(this.__id, recorder.__id); return this; }
  setAttractors(f32) { __pf_setAttractors(this.__id, f32); return this; }
  // run(ticks) or run(ticks, every, onTick) — onTick(view) may return false to stop.
  run(ticks, every, onTick) { return __pf_run(this.__id, ticks, every, onTick); }
  emit(cfg) { __pf_emit(this.__id, cfg); }
  kill(slot) { __pf_kill(this.__id, slot); }
  setFieldWeight(i, w) { __pf_setFieldWeight(this.__id, i, w); }
  get attractorsRemaining() { return __pf_attractorsRemaining(this.__id); }
  get depositedCount() { return __pf_depositedCount(this.__id); }
  surfaceNormal(p, radius) { return __pf_surfaceNormal(this.__id, p[0], p[1], p[2], radius); }
}
```

- [ ] **Step 5: Append the stamp test to script_host_tests.cpp**

Same bake-helper pattern; this one imports the shared-lib module and opens a voxel session:

```js
import { ParticleSim, PathRecorder } from 'shared-lib/particleflow';
class P extends Part {
  build(p) {
    const rec = new PathRecorder(0.03, ['thickness']);
    const sim = new ParticleSim({
      seed: 11, dt: 1.0, maxTurnRate: 0.4, depositEvery: 0.05,
      maxParticles: 16, hashCell: 0.25, maxAge: 60,
      attributes: ['thickness'],
      emitters: [{ shape: 'point', center: [0,0,0], rate: 1, vel0: [0,0.06,0],
                   jitter: 0.1, attrInit: [0.08] }],
      fields: [{ type: 'bias', dir: [0,1,0], weight: 0.4 }],
    }).attach(rec);
    sim.run(80);
    if (rec.count < 1) throw new Error('no paths');
    this.fill(MAT.bark);
    this.beginVoxels(0.05);
    this.paths(rec, { radiusChannel: 'thickness', minRadius: 0.02 });
    this.endVoxels();
  }
}
```

C++ assertions: bake succeeds with no error AND the bake produced non-empty geometry (use the same buffer/vertex-count check the existing voxel-session tests in this file use). Also add a negative case: same script but `this.paths(rec)` called BEFORE `beginVoxels` → expect the bake to fail with error containing `"paths() outside an open voxel session"`.

- [ ] **Step 6: Rebuild + run-script**

Run: `make -C MatterEngine3 && make -C MatterEngine3/tests run-script`
Expected: PASS (pf smoke, onTick, stamp positive + negative).

- [ ] **Step 7: CHECKPOINT B — full pf + script suites**

Run:
```bash
make -C ParticleFlowLib test
make -C MatterEngine3/tests run-script
make -C MatterEngine3/tests run-partv2
```
Expected: all PASS. (run-partv2 guards the part-asset path the voxel session feeds; this is the second of only three full-check points.)

- [ ] **Step 8: Commit**

```bash
git add MatterEngine3/src/pf_bindings.cpp MatterEngine3/src/part_base.js.h \
        MatterEngine3/shared-lib/particleflow.js MatterEngine3/tests/script_host_tests.cpp
git commit -m "feat(pf): this.paths() voxel sink (cone+sphere stamping) + shared-lib/particleflow.js wrappers"
```

---

### Task 8: shared-lib/strands.js — attractor clouds + twig anchors

**Files:**
- Create: `MatterEngine3/shared-lib/strands.js`
- Modify: shared-lib module registration IF Task 7 Step 3 found a manifest (add `strands`)
- Modify: `MatterEngine3/tests/script_host_tests.cpp` (append strands test)

**Interfaces:**
- Consumes: `rng(seed)` from `shared-lib/rng` (`{ random(), int(n), range(a,b) }`); `ParticleSim`/`PathRecorder` from `shared-lib/particleflow`; `path_end_dir` semantics via path xyz.
- Produces (Task 9 imports these exact names):
  - `ellipsoidCloud(seed, count, center, radii) -> Float32Array` (xyz triplets, uniform-in-ellipsoid)
  - `coneCloud(seed, count, apex, axis, height, spreadAngle) -> Float32Array`
  - `twigAnchors(sim, recorder, opts) -> [{pos:[x,y,z], normal:[x,y,z], dir:[x,y,z], t:number}]`
    with `opts = { seed, perPath, k (end-bias exponent, default 2), minThickness (skip fat trunk-bottom anchors... actually: skip vertices whose thickness EXCEEDS this — twigs belong on thin branch runs; default 0.25), normalRadius (surfaceNormal query radius, default 0.3), blend (0..1 how much the anchor normal leans toward the path direction as t→1, default 0.6) }`

- [ ] **Step 1: Create shared-lib/strands.js**

```js
import { rng } from 'shared-lib/rng';

// --- attractor clouds (Float32Array xyz triplets, deterministic per seed) ----

export function ellipsoidCloud(seed, count, center, radii) {
  const r = rng(seed);
  const out = new Float32Array(count * 3);
  for (let i = 0; i < count; ++i) {
    // rejection-sample the unit ball, then scale per-axis
    let x, y, z;
    do {
      x = r.range(-1, 1); y = r.range(-1, 1); z = r.range(-1, 1);
    } while (x * x + y * y + z * z > 1);
    out[3 * i]     = center[0] + x * radii[0];
    out[3 * i + 1] = center[1] + y * radii[1];
    out[3 * i + 2] = center[2] + z * radii[2];
  }
  return out;
}

export function coneCloud(seed, count, apex, axis, height, spreadAngle) {
  const r = rng(seed);
  const out = new Float32Array(count * 3);
  // orthonormal frame around axis
  const al = Math.hypot(axis[0], axis[1], axis[2]) || 1;
  const a = [axis[0] / al, axis[1] / al, axis[2] / al];
  const ref = Math.abs(a[1]) < 0.9 ? [0, 1, 0] : [1, 0, 0];
  let n1 = [a[1] * ref[2] - a[2] * ref[1], a[2] * ref[0] - a[0] * ref[2], a[0] * ref[1] - a[1] * ref[0]];
  const n1l = Math.hypot(n1[0], n1[1], n1[2]) || 1;
  n1 = [n1[0] / n1l, n1[1] / n1l, n1[2] / n1l];
  const n2 = [a[1] * n1[2] - a[2] * n1[1], a[2] * n1[0] - a[0] * n1[2], a[0] * n1[1] - a[1] * n1[0]];
  const tanS = Math.tan(spreadAngle);
  for (let i = 0; i < count; ++i) {
    const h = height * Math.cbrt(r.random());        // density ~ area growth
    const rad = h * tanS * Math.sqrt(r.random());
    const th = r.range(0, Math.PI * 2);
    const c = Math.cos(th) * rad, s = Math.sin(th) * rad;
    out[3 * i]     = apex[0] + a[0] * h + n1[0] * c + n2[0] * s;
    out[3 * i + 1] = apex[1] + a[1] * h + n1[1] * c + n2[1] * s;
    out[3 * i + 2] = apex[2] + a[2] * h + n1[2] * c + n2[2] * s;
  }
  return out;
}

// --- twig anchors -------------------------------------------------------------
// Sample points ALONG each recorded path with probability density ~ t^k
// (favoring branch ends), oriented along the deposited-surface isosurface
// normal, blended toward the local growth direction as t -> 1.

function v3(xyz, i) { return [xyz[3 * i], xyz[3 * i + 1], xyz[3 * i + 2]]; }
function norm3(v) {
  const l = Math.hypot(v[0], v[1], v[2]);
  return l > 1e-8 ? [v[0] / l, v[1] / l, v[2] / l] : [0, 1, 0];
}
function lerp3(a, b, f) {
  return norm3([a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, a[2] + (b[2] - a[2]) * f]);
}

export function twigAnchors(sim, recorder, opts) {
  const o = opts || {};
  const seed = o.seed === undefined ? 1 : o.seed;
  const perPath = o.perPath === undefined ? 2 : o.perPath;
  const k = o.k === undefined ? 2 : o.k;
  const maxThickness = o.maxThickness === undefined ? 0.25 : o.maxThickness;
  const normalRadius = o.normalRadius === undefined ? 0.3 : o.normalRadius;
  const blend = o.blend === undefined ? 0.6 : o.blend;
  const r = rng(seed);
  const anchors = [];
  recorder.forEach((path) => {
    const n = path.xyz.length / 3;
    if (n < 3) return;
    const thick = path.channels && path.channels.thickness;
    for (let s = 0; s < perPath; ++s) {
      // inverse-CDF of density ~ t^k on [0,1]: t = u^(1/(k+1)) favors t near 1
      const t = Math.pow(r.random(), 1 / (k + 1));
      const i = Math.min(n - 2, Math.max(1, Math.round(t * (n - 1))));
      if (thick && thick[i] > maxThickness) continue;   // too fat: trunk, not branch
      const pos = v3(path.xyz, i);
      const dir = norm3([
        path.xyz[3 * (i + 1)] - path.xyz[3 * (i - 1)],
        path.xyz[3 * (i + 1) + 1] - path.xyz[3 * (i - 1) + 1],
        path.xyz[3 * (i + 1) + 2] - path.xyz[3 * (i - 1) + 2],
      ]);
      let normal = sim.surfaceNormal(pos, normalRadius);
      if (!normal) continue;                             // no deposited neighbors
      normal = lerp3(norm3(normal), dir, blend * t);     // lean into growth at tips
      anchors.push({ pos, normal, dir, t });
    }
  });
  return anchors;
}
```

- [ ] **Step 2: Append the strands test to script_host_tests.cpp**

Bake-helper pattern; JS body:

```js
import { ParticleSim, PathRecorder } from 'shared-lib/particleflow';
import { ellipsoidCloud, twigAnchors } from 'shared-lib/strands';
class P extends Part {
  build(p) {
    const cloud = ellipsoidCloud(5, 200, [0, 8, 0], [3, 2, 3]);
    if (cloud.length !== 600) throw new Error('cloud size');
    for (let i = 0; i < 200; ++i) {
      const dx = (cloud[3*i] - 0) / 3, dy = (cloud[3*i+1] - 8) / 2, dz = (cloud[3*i+2] - 0) / 3;
      if (dx*dx + dy*dy + dz*dz > 1.0001) throw new Error('point outside ellipsoid');
    }
    const cloud2 = ellipsoidCloud(5, 200, [0, 8, 0], [3, 2, 3]);
    for (let i = 0; i < 600; ++i) if (cloud[i] !== cloud2[i]) throw new Error('cloud not deterministic');

    const rec = new PathRecorder(0.03, ['thickness']);
    const sim = new ParticleSim({
      seed: 9, dt: 1.0, maxTurnRate: 0.3, depositEvery: 0.05,
      maxParticles: 32, hashCell: 0.25, maxAge: 80,
      attributes: ['thickness'],
      emitters: [{ shape: 'disc', center: [0,0,0], axis: [0,1,0], radius: 0.2,
                   rate: 2, vel0: [0,0.06,0], jitter: 0.15, attrInit: [0.1] }],
      fields: [
        { type: 'bias', dir: [0,1,0], weight: 0.5 },
        { type: 'attract', weight: 0.8, influence: 4.0, killRadius: 0.3, killOnConsume: true },
      ],
    }).attach(rec);
    sim.setAttractors(cloud);
    sim.run(200);
    const anchors = twigAnchors(sim, rec, { seed: 2, perPath: 2, maxThickness: 10 });
    if (anchors.length < 1) throw new Error('no twig anchors');
    for (const a of anchors) {
      const nl = Math.hypot(a.normal[0], a.normal[1], a.normal[2]);
      if (Math.abs(nl - 1) > 1e-3) throw new Error('anchor normal not unit');
      if (a.t < 0 || a.t > 1) throw new Error('anchor t out of range');
    }
  }
}
```

C++ assertion: bake succeeds, no error. (No geometry expected — this schema stamps nothing.)

- [ ] **Step 3: Rebuild tests + run-script only**

Run: `make -C MatterEngine3/tests run-script`
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add MatterEngine3/shared-lib/strands.js MatterEngine3/tests/script_host_tests.cpp
git commit -m "feat(pf): shared-lib/strands.js — attractor clouds + end-biased twig anchors"
```

---

### Task 9: Tree.js rewrite — particle-flow trunk + visual gate

**Files:**
- Modify: `MatterEngine3/examples/world_demo/schemas/Tree.js` (full rewrite of `build()`)
- Test: visual — viewer screenshot via the FIFO harness (NO unit test; the schema is validated by bake + eyes)

**Interfaces:**
- Consumes: `ParticleSim`, `PathRecorder` (shared-lib/particleflow); `ellipsoidCloud`, `twigAnchors` (shared-lib/strands); `Part.paths()`; existing modifier stack verbs.
- Produces: the shipping Tree schema. Keep: `class Tree extends Part`, `static requires = [{ module: 'TreeBranch' }]`, `this.fill(MAT.bark)`, the exact modifier stack `[{ simplify: 0.3 }, { smooth: { iterations: 2 } }, { retopo: { target_ratio: 1.0, iterations: 3, seed: 42, timeout_seconds: 120 } }]`, VOX = 0.07.

- [ ] **Step 1: Rewrite Tree.js**

Replace the file body with the particle-flow version. Starting tunables below are a CALIBRATED GUESS — Step 2's visual gate exists to iterate on them; adjust freely, but keep AGE a single top-level knob (static growth parameter; live growth arrives with Phase C machinery, NOT here).

```js
import { ParticleSim, PathRecorder } from 'shared-lib/particleflow';
import { ellipsoidCloud, twigAnchors } from 'shared-lib/strands';

// Particle-flow tree: bundled strands grow upward from a basal disc (Holton-style
// trunk = bundle of strand paths), blending into space colonization as strands
// enter the crown and chase attractor points. Recorded paths are stamped into
// the voxel session as tapered cones (this.paths), so the whole existing
// isosurface -> modifier (simplify/smooth/retopo) pipeline is unchanged.
class Tree extends Part {
  static requires = [{ module: 'TreeBranch' }];

  build(p) {
    const AGE   = 1.0;              // growth knob: scales ticks + strand count
    const SEED  = 42;
    const VOX   = 0.07;

    // --- crown attractor cloud ---------------------------------------------
    const CROWN_C = [0, 11, 0];
    const CROWN_R = [5, 4, 5];
    const attractors = ellipsoidCloud(SEED * 7 + 1, 260, CROWN_C, CROWN_R);

    // --- strand sim ----------------------------------------------------------
    const STRANDS = Math.round(120 * AGE);
    const TICKS   = Math.round(800 * AGE);
    const rec = new PathRecorder(VOX * 0.8, ['thickness']);
    const sim = new ParticleSim({
      seed: SEED, dt: 1.0,
      maxTurnRate: 0.06,            // stiff strands: gentle curvature only
      speedTarget: 0.02, speedRelax: 0.15,
      depositEvery: VOX * 0.9,      // deposited spacing ~ voxel size
      maxParticles: STRANDS + 8, maxAge: TICKS,
      hashCell: 0.25,
      attributes: ['thickness'],
      emitters: [{
        shape: 'disc', center: [0, 0, 0], axis: [0, 1, 0],
        radius: 0.55,               // basal bundle radius
        rate: STRANDS / 40,         // all strands born in the first ~40 ticks
        vel0: [0, 0.02, 0], jitter: 0.06,
        attrInit: [VOX * 1.5],      // strand radius ~ 1.5 voxels
      }],
      fields: [
        // 0: upward bias, fades out through the crown transition band
        { type: 'bias', dir: [0, 1, 0], weight: 0.9,
          fade: { axis: 'y', from: 6, to: 9 } },
        // 1: adhere — strands hug the deposited bundle (the trunk IS the bundle)
        { type: 'adhere', weight: 0.8, radius: 0.35, surfaceOffset: 0.08 },
        // 2: separate — keeps strands from collapsing into one line
        { type: 'separate', weight: 0.5, radius: 0.15 },
        // 3: curl noise — organic wander
        { type: 'curl', weight: 0.25, scale: 2.0, seed: SEED + 3 },
        // 4: attract — space colonization takes over in the crown
        { type: 'attract', weight: 1.2, influence: 2.5, killRadius: 0.35,
          killOnConsume: true,
          fade: { axis: 'y', from: 5, to: 8 } },
      ],
    }).attach(rec);
    sim.setAttractors(attractors);
    sim.run(TICKS);

    // --- stamp strands into the voxel pipeline ------------------------------
    this.fill(MAT.bark);
    this.beginModifier();
    this.beginVoxels(VOX);
    this.paths(rec, { radiusChannel: 'thickness', minRadius: VOX * 0.9 });
    this.endVoxels();
    this.endModifier([
      { simplify: 0.3 },
      { smooth: { iterations: 2 } },
      { retopo: { target_ratio: 1.0, iterations: 3, seed: 42, timeout_seconds: 120 } },
    ]);

    // --- twigs (scaffold; disabled while trunk iterates, same as before) -----
    const PLACE_TWIGS = false;      // Jack disabled branch placement to iterate on the trunk
    if (PLACE_TWIGS) {
      const anchors = twigAnchors(sim, rec, {
        seed: SEED + 9, perPath: 2, k: 2, maxThickness: 0.25,
        normalRadius: 0.3, blend: 0.6,
      });
      const MAX_TWIGS = 10;
      let placed = 0;
      for (const a of anchors) {
        if (placed >= MAX_TWIGS) break;
        this.pushMatrix();
        this.translate(a.pos[0], a.pos[1], a.pos[2]);
        this.lookAt([a.pos[0] + a.normal[0], a.pos[1] + a.normal[1], a.pos[2] + a.normal[2]]);
        this.placeChild('TreeBranch');
        this.popMatrix();
        ++placed;
      }
    }
  }
}
```

NOTE: thickness is constant per strand here (attrInit); trunk taper emerges from the BUNDLE thinning as strands peel off to chase attractors — that is the Holton model and the spec's intent. Do not add per-vertex taper hacks unless the visual gate shows the trunk reads as a cylinder stack.

- [ ] **Step 2: Bake gate — treebake suite**

Run: `make -C MatterEngine3/tests run-treebake`
(If that target doesn't exist, find the tree-bake target: `grep -n "treebake\|run-tree" MatterEngine3/tests/Makefile` and run the one that bakes Tree.js.)
Expected: bake completes, non-empty geometry, no script errors. Iterate on tunables if the bake errors or produces zero triangles.

- [ ] **Step 3: Visual gate — viewer screenshots**

Use the FIFO harness (NEVER leave a viewer window open):

```bash
cd MatterViewer && rm -rf cache/parts/Tree* 
GALLIUM_DRIVER=d3d12 ../tools/viewer_shots.sh   # or the repo's documented shot script
```

(Read `tools/viewer_shots.sh` first for exact usage; it drives cam/shot/reload over MATTER_CMD_FIFO and self-terminates.) Inspect the screenshot(s) with the Read tool. Judge: single coherent trunk from bundled strands, visible strand texture, branching spread into the crown volume, no floating disconnected blobs, no empty world. Iterate tunables (STRANDS, adhere/separate weights, attract influence, crown size) until the silhouette reads as a tree. Budget ~3 iterations; if still off, note findings and continue — Task 10's refinement pass returns here.

- [ ] **Step 4: Commit**

```bash
git add MatterEngine3/examples/world_demo/schemas/Tree.js
git commit -m "feat(tree): particle-flow trunk — bundled strands + space-colonization crown replaces L-system"
```

---

### Task 10: FINAL GATE — build-all registration, Windows viewer wiring, full sweep, correctness refinement

**Files:**
- Modify: `build-all.sh` (register ParticleFlowLib in SIMPLE_PROJECTS ~:32-54; add test hook ~:197 if the test-hook list is per-project)
- Modify: `MatterViewer/Makefile` (Windows target: include path ~:45-49, W_* source vars ~:96-243, vpath ~:190-192)
- No new tests — this task RUNS the full gates and refines correctness.

**Interfaces:**
- Consumes: everything from Tasks 1-9.
- Produces: green full-suite state on Linux; a clean-rebuilt Windows `viewer.exe` carrying the pf sources.

- [ ] **Step 1: Register ParticleFlowLib in build-all.sh**

Read `build-all.sh` (~:32-54) — add `ParticleFlowLib` to the SIMPLE_PROJECTS (or equivalent) list following the existing entry pattern, and wire `make -C ParticleFlowLib test` into the `test` mode the same way MemoryLib's test hook is wired (~:197). Mirror MemoryLib exactly — it is the closest sibling (header lib + ASan test binary).

Run: `./build-all.sh` — expected: all projects build including ParticleFlowLib.

- [ ] **Step 2: Windows viewer wiring**

In `MatterViewer/Makefile` (Windows section): the Windows target compiles engine sources DIRECTLY (no .a). Mirror the existing pattern:

(a) Add `-I../ParticleFlowLib/include` to `WIN_INCLUDE_PATHS` (~:45-49).
(b) Add the five pf sources to the Windows source/object vars (follow the `W_*` naming at ~:96-243):
    `../ParticleFlowLib/src/pf_math.cpp`, `pf_sim.cpp`, `pf_fields.cpp`, `pf_path_recorder.cpp`, `../MatterEngine3/src/pf_bindings.cpp`.
(c) Extend the Windows vpath (~:190-192) with `../ParticleFlowLib/src`.

IMPORTANT (memory: clean-rebuild rule): clear ALL Windows objects before rebuilding — partial rebuilds after header changes produce wandering silent crashes.

Run: `make -C MatterViewer clean-windows 2>/dev/null; make -C MatterViewer windows` (check the actual clean target name first).
Expected: `viewer.exe` links with pf objects.

- [ ] **Step 3: FULL LINUX SWEEP (final gate)**

```bash
make -C ParticleFlowLib clean && make -C ParticleFlowLib && make -C ParticleFlowLib test
make -C MatterEngine3
make -C MatterEngine3/tests run-script
make -C MatterEngine3/tests run-partv2
make -C MatterEngine3/tests run-treebake     # or the Tree bake target found in Task 9
make -C MatterViewer
```
Expected: everything green. GPU suites only if a touched area demands it (`GALLIUM_DRIVER=d3d12`).

- [ ] **Step 4: Correctness refinement pass**

This is where deferred rigor gets paid down. Re-read the SPEC's requirements list (`docs/superpowers/specs/2026-07-09-particle-flow-tree-design.md`) and verify each against the implementation:

1. **Determinism end-to-end**: bake the Tree schema twice in one test process; assert the two voxel-session outputs are byte-identical (same buffer hash the existing determinism tests use). If script_host_tests has a resolved-hash/determinism helper, reuse it.
2. **Incremental equivalence at the JS level**: add one script test — `sim.run(300)` vs `sim.run(120); sim.run(180)` with identical configs → `depositedCount` equal and first path xyz identical.
3. **Append-only accretion**: already covered in Task 4 kernel tests; spot-check at JS level if cheap (path 0 prefix unchanged after extra run).
4. **Budget behavior**: confirm a tiny `time_budget_ms` bake with a huge `run()` fails-closed with the "budget exceeded" error, not a hang (script test with `opts.time_budget_ms` — check how script_host_tests sets bake options).
5. **Error paths**: stale handle (call `__pf_run(999, 10)` → set_error, bake fails cleanly), `paths()` outside session (covered Task 7).
6. **Leak check**: `run-script` already runs under the tests' standard flags; if ASan isn't default there, run the script suite once with `EXTRA_CFLAGS=-fsanitize=address` if the Makefile supports it, else note as accepted gap.
7. Fix everything found; keep fixes small and commit per fix.

- [ ] **Step 5: Visual re-check**

If Step 4 changed any kernel/binding behavior, re-run the Task 9 Step 3 screenshot flow once (d3d12, self-terminating harness) and confirm the tree still reads correctly.

- [ ] **Step 6: Commit + final state**

```bash
git add build-all.sh MatterViewer/Makefile
git commit -m "build(pf): register ParticleFlowLib in build-all; wire pf sources into Windows viewer"
```

Then verify: `git status` clean, all commits present, plan checkboxes all ticked.

---

## Self-Review Notes (writing-plans)

- **Spec coverage**: kernel (T1-4) ✓; DSL blocks configured/wired in JS not hard-coded C++ ✓ (T5-7); velocity-canonical ✓ (T2); paths→voxel sink reusing existing pipeline ✓ (T7); twig anchors along branches, isosurface normals, end-favoring t^k ✓ (T8); Tree.js rewrite with AGE knob, no bake-machinery changes ✓ (T9); determinism/incremental/append-only ✓ (T2/T4/T10); time budget ✓ (T5/T10); parallel-instance safety = no globals/statics in kernel ✓ (Global Constraints, T1-4).
- **Checkpoint discipline**: full suites ONLY at Checkpoint A (T4), Checkpoint B (T7), Final Gate (T10); every other task runs a single targeted binary.
- **Known ADAPT points** (deliberate, flagged inline): DslState sphere/cone exact signatures (T7), quickjs-ng JS_DetachArrayBuffer/JS_NewTypedArray exact signatures (T6), shared-lib module manifest (T7 S3), treebake target name (T9), Windows clean target name (T10). Implementers must read the named file before coding these.
