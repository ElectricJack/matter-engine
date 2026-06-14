# Lattice Particle Stress Scene + Per-Particle Tint Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a per-particle RGBA `tint` that blends into the material albedo, then build a 64×64×128 jittered particle lattice (two opaque + glass) that stress-tests the ray tracer, with each particle slightly tinted.

**Architecture:** A `tint` (RGBA) rides from `StaticParticle` → per-triangle `TriEx` → the spare `.w` slots of the BLAS triangle GPU texture → the ray-tracing shader, which applies `albedo = mix(material.albedo, tint.rgb, tint.a)`. Neutral tint is alpha=0, so all existing geometry is unchanged. The scene is a new `setup_lattice_scene()` in `main.cpp` using deterministic value-noise displacement; the old `setup_matter_system()` call is commented out.

**Tech Stack:** C/C++ (C++17), raylib, custom GLSL ray tracer with a Makefile shader-preprocessing step (`make shaders`).

**Key facts discovered (do not re-derive):**
- Two opaque materials already exist and share a merge group: `stone_light` (id 8) and `stone_dark` (id 9), both `GROUP_STONE`. Glass is id 4 (`GROUP_GLASS`, translucent). No `material_registry.c` change needed.
- The triangle GPU texture uses **6 texel rows per triangle** (`src/blas_manager.cpp:352-460`). Row 0 `.w` holds the material id; rows 1–5 `.w` are currently written as `0.0f` and are free.
- The app loads the **generated** shader `shaders/raytrace_tlas_blas_processed.fs` (`main.cpp:552`). It is produced by `make shaders` from `raytrace_tlas_blas.fs` + `bvh_tlas_common.glsl` (`Makefile:154-156`). Edit the sources, then rebuild.
- Neutral tint = alpha 0 ⇒ `mix(albedo, rgb, 0) = albedo`. `TriEx ex{}` zero-inits tint to `(0,0,0,0)`, which is neutral, so untinted meshes need no special sentinel.

---

### Task 1: Add the `tint` field to the data structures

**Files:**
- Modify: `MatterSurfaceLib/include/bvh.h:25` (add `float4 tint` to `TriEx`)
- Modify: `MatterSurfaceLib/include/cluster.h:18-25` (add `tint` to `StaticParticle`)
- Modify: `MatterSurfaceLib/include/cluster.h:46` and `MatterSurfaceLib/src/cluster.cpp:68-81` (an `add_particle` overload taking a tint)

- [ ] **Step 1: Add `tint` to `TriEx`**

In `MatterSurfaceLib/include/bvh.h`, change line 25 from:

```cpp
struct TriEx { float2 uv0, uv1, uv2; float3 N0, N1, N2; int materialId; };
```

to:

```cpp
// tint is per-triangle RGBA copied from the nearest particle; a (alpha) is the
// blend strength against the material albedo. (0,0,0,0) = no tint (neutral).
struct TriEx { float2 uv0, uv1, uv2; float3 N0, N1, N2; int materialId; float4 tint; };
```

- [ ] **Step 2: Add `tint` to `StaticParticle`**

In `MatterSurfaceLib/include/cluster.h`, replace the struct at lines 18-25 with:

```cpp
// Static particle structure for matter representation
struct StaticParticle {
    Vector3 position;       // Position in local cluster space
    float radius;          // Particle radius
    uint32_t materialId;   // Material identifier
    Vector4 tint;          // RGBA tint; a = blend strength. (1,1,1,0) = no tint.

    StaticParticle(const Vector3& pos = {0,0,0}, float r = 1.0f, uint32_t mat = 0,
                   const Vector4& t = {1.0f, 1.0f, 1.0f, 0.0f})
        : position(pos), radius(r), materialId(mat), tint(t) {}
};
```

- [ ] **Step 3: Declare the `add_particle` tint overload**

In `MatterSurfaceLib/include/cluster.h`, after line 46 (the existing `add_particle` declaration) add:

```cpp
    uint32_t add_particle(const Vector3& local_position, float radius, uint32_t material_id, const Vector4& tint);
```

- [ ] **Step 4: Implement the overload**

In `MatterSurfaceLib/src/cluster.cpp`, after the existing `add_particle` (ends at line 81) add:

```cpp
uint32_t Cluster::add_particle(const Vector3& local_position, float radius, uint32_t material_id, const Vector4& tint) {
    uint32_t particle_id = next_particle_id_++;
    particles_.emplace_back(local_position, radius, material_id, tint);
    mark_cells_dirty_around_particle(local_position, radius);
    return particle_id;
}
```

(Note: this overload intentionally omits the per-particle `printf` — the lattice adds ~500k particles and the print would dominate runtime.)

- [ ] **Step 5: Build the headless test suites to confirm the struct changes compile**

Run: `cd "MatterSurfaceLib/tests" && make blas_refcount_tests cell_bounds_tests`
Expected: both link successfully (these compile `blas_manager.cpp`, `bvh.cpp`, and `cell.cpp`, exercising the changed `TriEx`/`StaticParticle`).

- [ ] **Step 6: Commit**

```bash
git add MatterSurfaceLib/include/bvh.h MatterSurfaceLib/include/cluster.h MatterSurfaceLib/src/cluster.cpp
git commit -m "feat: add per-particle RGBA tint field to particle and triangle structs"
```

---

### Task 2: Pack tint into the BLAS GPU texture + dedup identity (TDD)

**Files:**
- Modify: `MatterSurfaceLib/include/blas_manager.hpp:74-76` (add `pack_tint_w` helper)
- Modify: `MatterSurfaceLib/src/blas_manager.cpp:41-82` (fold tint into hash + equality)
- Modify: `MatterSurfaceLib/src/blas_manager.cpp:418-460` (write tint into spare `.w` slots)
- Create: `MatterSurfaceLib/tests/blas_tint_tests.cpp`
- Modify: `MatterSurfaceLib/tests/Makefile` (new `blas_tint_tests` target)

- [ ] **Step 1: Write the failing test**

Create `MatterSurfaceLib/tests/blas_tint_tests.cpp`:

```cpp
#include "../include/blas_manager.hpp"
#include <cstdio>
#include <cmath>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)

static Tri make_tri(float ox) {
    Tri t;
    t.vertex0 = make_float3(ox + 0.0f, 0.0f, 0.0f);
    t.vertex1 = make_float3(ox + 1.0f, 0.0f, 0.0f);
    t.vertex2 = make_float3(ox + 0.0f, 1.0f, 0.0f);
    t.centroid = make_float3(ox + 0.333f, 0.333f, 0.0f);
    return t;
}

int main() {
    // --- pack_tint_w: reads each channel; null triex packs 0 (neutral alpha). ---
    TriEx ex{};
    ex.materialId = 8;
    ex.tint = make_float4(0.2f, 0.4f, 0.6f, 0.8f);
    CHECK(fabsf(BLASManager::pack_tint_w(&ex, 0, 0) - 0.2f) < 1e-6f, "tint.r pack");
    CHECK(fabsf(BLASManager::pack_tint_w(&ex, 0, 1) - 0.4f) < 1e-6f, "tint.g pack");
    CHECK(fabsf(BLASManager::pack_tint_w(&ex, 0, 2) - 0.6f) < 1e-6f, "tint.b pack");
    CHECK(fabsf(BLASManager::pack_tint_w(&ex, 0, 3) - 0.8f) < 1e-6f, "tint.a pack");
    CHECK(BLASManager::pack_tint_w(nullptr, 0, 3) == 0.0f, "null triex tint packs 0");

    // --- Tint participates in dedup: identical geometry + materialId but
    //     different tint must NOT share a BLAS. ---
    BLASManager mgr;
    Tri tris[2] = { make_tri(0.0f), make_tri(5.0f) };

    TriEx exA[2] = {}; TriEx exB[2] = {};
    for (int i = 0; i < 2; ++i) {
        exA[i].materialId = 8; exB[i].materialId = 8;
        exA[i].N0 = exA[i].N1 = exA[i].N2 = make_float3(0,0,1);
        exB[i].N0 = exB[i].N1 = exB[i].N2 = make_float3(0,0,1);
        exA[i].tint = make_float4(1,0,0,0.5f);
        exB[i].tint = make_float4(0,0,1,0.5f);
    }

    BLASHandle hA  = mgr.register_triangles(tris, 2, exA);
    BLASHandle hB  = mgr.register_triangles(tris, 2, exB);
    BLASHandle hA2 = mgr.register_triangles(tris, 2, exA);

    CHECK(hA != INVALID_BLAS_HANDLE, "register A valid");
    CHECK(hB != INVALID_BLAS_HANDLE, "register B valid");
    CHECK(hA != hB, "different tint must not dedup to same BLAS");
    CHECK(hA == hA2, "identical tint re-registration shares the BLAS");

    if (failures == 0) printf("All blas_tint tests passed\n");
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Add the test target to the Makefile**

In `MatterSurfaceLib/tests/Makefile`, after the `run-blas` target (line 89) add:

```makefile
# Per-triangle tint: GPU pack helper + dedup-identity regression (headless, no GL)
TINT_TARGET = blas_tint_tests
TINT_SOURCES = blas_tint_tests.cpp ../src/blas_manager.cpp ../src/bvh.cpp

$(TINT_TARGET): $(TINT_SOURCES)
	$(CC) $(TINT_SOURCES) -o $(TINT_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)

run-tint: $(TINT_TARGET)
	./$(TINT_TARGET)
```

- [ ] **Step 3: Run the test to verify it fails to compile**

Run: `cd "MatterSurfaceLib/tests" && make blas_tint_tests`
Expected: FAIL — `pack_tint_w` is not a member of `BLASManager`.

- [ ] **Step 4: Add the `pack_tint_w` helper**

In `MatterSurfaceLib/include/blas_manager.hpp`, after `pack_material_w` (ends at line 76) add:

```cpp
    // Per-triangle tint channel packed into a spare row .w of the GPU triangle
    // texture. channel: 0=r,1=g,2=b,3=a. A null triEx packs 0.0f; alpha 0 means
    // "no tint" in the shader, so untinted meshes stay neutral.
    static float pack_tint_w(const TriEx* triex, int index, int channel) {
        if (!triex) return 0.0f;
        const float4& t = triex[index].tint;
        switch (channel) {
            case 0: return t.x;
            case 1: return t.y;
            case 2: return t.z;
            default: return t.w;
        }
    }
```

- [ ] **Step 5: Fold tint into the BLAS hash**

In `MatterSurfaceLib/src/blas_manager.cpp`, in `calculate_hash`, after the materialId folding block (lines 56-58, ends with `hash *= 16777619u;`) and before the closing `}` of the `for` loop, add:

```cpp
        // Fold the per-triangle tint into identity so geometry that differs only
        // by tint is not deduplicated. No triEx -> neutral (0,0,0,0).
        const float4 tnt = triex ? triex[i].tint : make_float4(0.0f, 0.0f, 0.0f, 0.0f);
        const float* tf = reinterpret_cast<const float*>(&tnt);
        for (int k = 0; k < 4; k++) {
            hash ^= *reinterpret_cast<const uint32_t*>(&tf[k]);
            hash *= 16777619u;
        }
```

- [ ] **Step 6: Compare tint in `triangles_equal`**

In `MatterSurfaceLib/src/blas_manager.cpp`, in `triangles_equal`, after the material match block (lines 75-79, the `if (a_mat != b_mat) return false;`) and before the loop's closing `}`, add:

```cpp
        // Tint must match too (a null triEx is neutral (0,0,0,0)).
        const float4 a_tint = a_ex ? a_ex[i].tint : make_float4(0.0f, 0.0f, 0.0f, 0.0f);
        const float4 b_tint = triex ? triex[i].tint : make_float4(0.0f, 0.0f, 0.0f, 0.0f);
        if (std::memcmp(&a_tint, &b_tint, sizeof(float4)) != 0) {
            return false;
        }
```

- [ ] **Step 7: Run the test to verify it passes**

Run: `cd "MatterSurfaceLib/tests" && make blas_tint_tests && ./blas_tint_tests`
Expected: PASS — `All blas_tint tests passed`.

- [ ] **Step 8: Write tint into the GPU texture spare `.w` slots**

In `MatterSurfaceLib/src/blas_manager.cpp`, inside `generate_triangle_texture_data`'s per-triangle write block:

First, just after the row-0 material write (line 425, `texture_data[row0_idx + 3] = pack_material_w(entry->mesh->triEx, ...);`), introduce a local for reuse — replace line 425 region so the triEx pointer is named once. Change line 433 from:

```cpp
                    texture_data[row1_idx + 3] = 0.0f;
```

to:

```cpp
                    texture_data[row1_idx + 3] = pack_tint_w(entry->mesh->triEx, static_cast<int>(triangle_index), 0); // tint.r
```

Change line 440 from:

```cpp
                    texture_data[row2_idx + 3] = 0.0f;
```

to:

```cpp
                    texture_data[row2_idx + 3] = pack_tint_w(entry->mesh->triEx, static_cast<int>(triangle_index), 1); // tint.g
```

Then in the normals loop (lines 454-459) the `.w` is set to `0.0f` for rows 3,4,5. After that loop closes, append:

```cpp
                    // Pack tint.b/.a into the spare .w of normal rows 3 and 4
                    // (row 5 .w stays unused). Shader reads these in decodeHit.
                    {
                        int rowB = texel_off(static_cast<int>(triangle_index), 3);
                        int rowA = texel_off(static_cast<int>(triangle_index), 4);
                        texture_data[rowB + 3] = pack_tint_w(entry->mesh->triEx, static_cast<int>(triangle_index), 2); // tint.b
                        texture_data[rowA + 3] = pack_tint_w(entry->mesh->triEx, static_cast<int>(triangle_index), 3); // tint.a
                    }
```

- [ ] **Step 9: Rebuild the headless suites to confirm everything still compiles**

Run: `cd "MatterSurfaceLib/tests" && make blas_tint_tests blas_refcount_tests && ./blas_tint_tests && ./blas_refcount_tests`
Expected: both PASS (tint test green; refcount regression unaffected).

- [ ] **Step 10: Commit**

```bash
git add MatterSurfaceLib/include/blas_manager.hpp MatterSurfaceLib/src/blas_manager.cpp MatterSurfaceLib/tests/blas_tint_tests.cpp MatterSurfaceLib/tests/Makefile
git commit -m "feat: pack per-triangle tint into BLAS texture and dedup identity"
```

---

### Task 3: Copy the nearest particle's tint onto each triangle

**Files:**
- Modify: `MatterSurfaceLib/src/cell.cpp:317-332` (build a tint array parallel to `particles`)
- Modify: `MatterSurfaceLib/src/cell.cpp:392-407` (tag each triangle with the nearest particle's tint)

- [ ] **Step 1: Build a parallel tint array as particles are gathered**

In `MatterSurfaceLib/src/cell.cpp::generate_mesh_for_group`, find the particle-gathering block (lines 317-332). Add a `tints` vector alongside `particles`. Change:

```cpp
    std::vector<Particle> particles;
    particles.reserve(particle_indices.size());
    float max_radius = 0.0f;
    for (uint32_t idx : particle_indices) {
        if (idx >= cluster_particles.size()) continue;
        const StaticParticle& sp = cluster_particles[idx];
        if (sp.radius < cull_radius) continue; // too small to represent at this LOD
        float r_eff = (sp.radius < vis_radius) ? vis_radius : sp.radius;

        Particle surface_particle;
        surface_particle.position = sp.position;
        surface_particle.radius = r_eff;
        surface_particle.materialId = static_cast<int>(sp.materialId);
        particles.push_back(surface_particle);
        if (r_eff > max_radius) max_radius = r_eff;
    }
```

to:

```cpp
    std::vector<Particle> particles;
    std::vector<float4> particle_tints;   // aligned 1:1 with `particles`
    particles.reserve(particle_indices.size());
    particle_tints.reserve(particle_indices.size());
    float max_radius = 0.0f;
    for (uint32_t idx : particle_indices) {
        if (idx >= cluster_particles.size()) continue;
        const StaticParticle& sp = cluster_particles[idx];
        if (sp.radius < cull_radius) continue; // too small to represent at this LOD
        float r_eff = (sp.radius < vis_radius) ? vis_radius : sp.radius;

        Particle surface_particle;
        surface_particle.position = sp.position;
        surface_particle.radius = r_eff;
        surface_particle.materialId = static_cast<int>(sp.materialId);
        particles.push_back(surface_particle);
        particle_tints.push_back(make_float4(sp.tint.x, sp.tint.y, sp.tint.z, sp.tint.w));
        if (r_eff > max_radius) max_radius = r_eff;
    }
```

- [ ] **Step 2: Tag each triangle with the nearest particle's tint**

In the per-triangle tagging loop (lines 392-407), track the nearest particle's index so both materialId and tint come from the same particle. Replace:

```cpp
                int best = particles[0].materialId;
                float bestD = 3.4e38f;
                for (const Particle& p : particles) {
                    float dx = c.x - p.position.x, dy = c.y - p.position.y, dz = c.z - p.position.z;
                    float d = dx*dx + dy*dy + dz*dz;
                    if (d < bestD) { bestD = d; best = p.materialId; }
                }
                triangle_normals[t].materialId = best;
```

with:

```cpp
                int bestIdx = 0;
                float bestD = 3.4e38f;
                for (size_t pi = 0; pi < particles.size(); ++pi) {
                    const Particle& p = particles[pi];
                    float dx = c.x - p.position.x, dy = c.y - p.position.y, dz = c.z - p.position.z;
                    float d = dx*dx + dy*dy + dz*dz;
                    if (d < bestD) { bestD = d; bestIdx = static_cast<int>(pi); }
                }
                triangle_normals[t].materialId = particles[bestIdx].materialId;
                triangle_normals[t].tint = particle_tints[bestIdx];
```

- [ ] **Step 3: Build the cell test suite to confirm it compiles and still passes**

Run: `cd "MatterSurfaceLib/tests" && make cell_bounds_tests && ./cell_bounds_tests`
Expected: builds and PASSES (geometry/bounds behavior unchanged; tint is additive).

- [ ] **Step 4: Commit**

```bash
git add MatterSurfaceLib/src/cell.cpp
git commit -m "feat: tag triangles with nearest particle tint during meshing"
```

---

### Task 4: Apply tint in the ray-tracing shader

**Files:**
- Modify: `MatterSurfaceLib/shaders/bvh_tlas_common.glsl:66-74` (add tint to `HitResult`)
- Modify: `MatterSurfaceLib/shaders/bvh_tlas_common.glsl:494-523` (read tint, set on hit/miss)
- Modify: `MatterSurfaceLib/shaders/raytrace_tlas_blas.fs:420-421` (mix tint into albedo)

- [ ] **Step 1: Add tint fields to `HitResult`**

In `MatterSurfaceLib/shaders/bvh_tlas_common.glsl`, in the `HitResult` struct (lines 66-74), after `int material;` (line 72) add:

```glsl
    vec3 tint;       // per-triangle tint rgb (from spare .w of rows 1-3)
    float tintAlpha; // blend strength (from spare .w of row 4); 0 = no tint
```

- [ ] **Step 2: Read the tint texels on a hit**

In `bvh_tlas_common.glsl`, after the `effectiveMat` line (line 496) add:

```glsl
        // Per-triangle tint packed in the spare .w of rows 1-4 (see blas_manager.cpp).
        result.tint = vec3(
            texture(trianglesTexture, tiledTexel(trianglesTexture, int(triIdx), 1, 6)).w,
            texture(trianglesTexture, tiledTexel(trianglesTexture, int(triIdx), 2, 6)).w,
            texture(trianglesTexture, tiledTexel(trianglesTexture, int(triIdx), 3, 6)).w);
        result.tintAlpha = texture(trianglesTexture, tiledTexel(trianglesTexture, int(triIdx), 4, 6)).w;
```

- [ ] **Step 3: Zero the tint on a miss**

In `bvh_tlas_common.glsl`, in the `else` branch (lines 517-522), after `result.material = -1;` (line 521) add:

```glsl
        result.tint = vec3(0.0);
        result.tintAlpha = 0.0;
```

- [ ] **Step 4: Blend tint into the primary albedo**

In `MatterSurfaceLib/shaders/raytrace_tlas_blas.fs`, replace line 421:

```glsl
        vec3 albedo = matProps.albedo;
```

with:

```glsl
        vec3 albedo = mix(matProps.albedo, hit.tint, hit.tintAlpha);
```

(Scope note: secondary/indirect/reflected hits keep the untinted material albedo — acceptable for this prototype.)

- [ ] **Step 5: Regenerate the processed shader and build the app**

Run: `cd "MatterSurfaceLib" && make shaders && make`
Expected: `shaders/raytrace_tlas_blas_processed.fs` is regenerated and the app links. (If `make` reports the shader is up to date, run `touch shaders/raytrace_tlas_blas.fs && make shaders` first.)

- [ ] **Step 6: Commit**

```bash
git add MatterSurfaceLib/shaders/bvh_tlas_common.glsl MatterSurfaceLib/shaders/raytrace_tlas_blas.fs MatterSurfaceLib/shaders/raytrace_tlas_blas_processed.fs
git commit -m "feat: apply per-particle tint to albedo in ray tracer"
```

---

### Task 5: Build the lattice scene

**Files:**
- Modify: `MatterSurfaceLib/main.cpp:324` (call `setup_lattice_scene()`; comment out `setup_matter_system()`)
- Modify: `MatterSurfaceLib/main.cpp` (add `setup_lattice_scene()` and a value-noise helper near `setup_matter_system`, ~line 456)

- [ ] **Step 1: Add a deterministic 3D value-noise helper**

In `MatterSurfaceLib/main.cpp`, immediately **above** the demo class method `setup_matter_system()` is not possible (it's a member); instead add these as free functions near the top-level helpers (after the includes block, e.g. after line 33). Add:

```cpp
// --- Deterministic 3D value noise for lattice displacement (no deps) ---
static float lattice_vhash(int x, int y, int z) {
    uint32_t h = (uint32_t)(x * 374761393) ^ (uint32_t)(y * 668265263) ^ (uint32_t)(z * 2147483647);
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (float)(h & 0xFFFFFFu) / (float)0xFFFFFFu; // [0,1]
}
static float lattice_vnoise(float x, float y, float z) {
    int xi = (int)floorf(x), yi = (int)floorf(y), zi = (int)floorf(z);
    float xf = x - xi, yf = y - yi, zf = z - zi;
    auto lerpf = [](float a, float b, float t) { return a + (b - a) * t; };
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
```

- [ ] **Step 2: Add the `setup_lattice_scene()` member**

In `MatterSurfaceLib/main.cpp`, directly after the closing brace of `setup_matter_system()` (line 515) add:

```cpp
    void setup_lattice_scene() {
        printf("Setting up lattice stress scene...\n");

        // --- Tunables (dial DIM down to validate, then crank to 64,64,128) ---
        const int   DIM_X = 64, DIM_Y = 64, DIM_Z = 128;
        const float BASE_RADIUS = 0.4f;
        const float SPACING     = 2.0f * BASE_RADIUS;   // neighbors just touch (hybrid look)
        const float NOISE_SCALE = 0.15f;                // lattice cells per noise period
        const float NOISE_AMP   = 0.9f * BASE_RADIUS;   // perlin drift magnitude
        const float POS_JITTER  = 0.15f * BASE_RADIUS;  // fine per-particle position jitter
        const float RAD_JITTER  = 0.15f;                // +/- fraction on radius
        const float GLASS_FRAC  = 0.15f;                // ~15% glass
        const float TINT_ALPHA  = 0.2f;                 // subtle tint strength
        const uint32_t MAT_OPAQUE_A = 8;  // stone_light (GROUP_STONE)
        const uint32_t MAT_OPAQUE_B = 9;  // stone_dark  (GROUP_STONE)
        const uint32_t MAT_GLASS    = 4;  // glass (GROUP_GLASS, carves)

        SetRandomSeed(1337);
        auto rnd = []() { return (float)GetRandomValue(-1000, 1000) / 1000.0f; }; // [-1,1]
        auto rnd01 = []() { return (float)GetRandomValue(0, 1000) / 1000.0f; };   // [0,1]

        const float halfx = (DIM_X - 1) * SPACING * 0.5f;
        const float halfy = (DIM_Y - 1) * SPACING * 0.5f;
        const float halfz = (DIM_Z - 1) * SPACING * 0.5f;

        for (int ix = 0; ix < DIM_X; ++ix)
        for (int iy = 0; iy < DIM_Y; ++iy)
        for (int iz = 0; iz < DIM_Z; ++iz) {
            // Base lattice position, centered on the origin.
            float bx = ix * SPACING - halfx;
            float by = iy * SPACING - halfy;
            float bz = iz * SPACING - halfz;

            // Coherent perlin-style drift: sample three offset noise fields.
            float nx = lattice_vnoise(ix * NOISE_SCALE + 11.3f, iy * NOISE_SCALE, iz * NOISE_SCALE);
            float ny = lattice_vnoise(ix * NOISE_SCALE, iy * NOISE_SCALE + 27.7f, iz * NOISE_SCALE);
            float nz = lattice_vnoise(ix * NOISE_SCALE, iy * NOISE_SCALE, iz * NOISE_SCALE + 51.1f);
            Vector3 pos = {
                bx + (nx * 2.0f - 1.0f) * NOISE_AMP + rnd() * POS_JITTER,
                by + (ny * 2.0f - 1.0f) * NOISE_AMP + rnd() * POS_JITTER,
                bz + (nz * 2.0f - 1.0f) * NOISE_AMP + rnd() * POS_JITTER
            };

            float radius = BASE_RADIUS * (1.0f + rnd() * RAD_JITTER);

            // Material: ~15% glass, rest split between the two opaque stones.
            uint32_t mat;
            if (rnd01() < GLASS_FRAC)      mat = MAT_GLASS;
            else if (rnd01() < 0.5f)       mat = MAT_OPAQUE_A;
            else                            mat = MAT_OPAQUE_B;

            // Subtle per-particle tint around the base material color.
            Vector4 tint = { rnd01(), rnd01(), rnd01(), TINT_ALPHA };

            test_cluster_->add_particle(pos, radius, mat, tint);
        }

        printf("Added %u particles to lattice\n", test_cluster_->get_particle_count());

        test_cluster_->set_position({0.0f, 2.0f, 0.0f});
        test_cluster_->set_lod_level(0);          // finest detail
        test_cluster_->rebuild_dirty_cells();

        printf("Lattice has %u cells, %u dirty\n",
               test_cluster_->get_cell_count(), test_cluster_->get_dirty_cell_count());
    }
```

- [ ] **Step 3: Switch the scene call**

In `MatterSurfaceLib/main.cpp`, change line 324 from:

```cpp
        setup_matter_system();
```

to:

```cpp
        // setup_matter_system();
        setup_lattice_scene();
```

- [ ] **Step 4: Build the app**

Run: `cd "MatterSurfaceLib" && make`
Expected: links cleanly.

- [ ] **Step 5: Smoke-test with a small lattice (headless capture)**

Temporarily set `DIM_X = 8, DIM_Y = 8, DIM_Z = 16` in `setup_lattice_scene()` to validate quickly, then run a headless capture:

Run: `cd "MatterSurfaceLib" && MSL_CAPTURE=lattice_smoke.png MSL_FRAMES=4 ./build/linux/matter_surface_lib`
Expected: exits cleanly and writes `lattice_smoke.png`. Open it (Read tool) and confirm: a clump of individual tinted spheres, some glass, varied colors. Then restore `DIM_X=64, DIM_Y=64, DIM_Z=128`.

- [ ] **Step 6: Commit**

```bash
git add MatterSurfaceLib/main.cpp
git commit -m "feat: add lattice particle stress-test scene with perlin jitter and tint"
```

---

### Task 6: Full verification

- [ ] **Step 1: Run the headless test suites**

Run: `cd "MatterSurfaceLib/tests" && make blas_tint_tests blas_refcount_tests cell_bounds_tests material_registry_tests && ./blas_tint_tests && ./blas_refcount_tests && ./cell_bounds_tests && ./material_registry_tests`
Expected: all print their pass lines, exit 0.

- [ ] **Step 2: Confirm the top-level build stays green**

Run: `cd "MatterSurfaceLib" && make` (and, if time permits, `cd .. && ./build-all.sh test`)
Expected: app builds; existing suites pass.

- [ ] **Step 3: Full-scale visual / benchmark run (manual)**

Because the GUI app must be launched by the user (the harness reaps backgrounded GUI children), ask the user to run:
`cd "MatterSurfaceLib" && ./build/linux/matter_surface_lib`
or a headless capture at full scale:
`cd "MatterSurfaceLib" && MSL_CAPTURE=lattice_full.png MSL_FRAMES=8 ./build/linux/matter_surface_lib`
Confirm: per-particle tint variation is visible, glass renders/refracts, opaque stones fuse into a brick mass, and note the frame time for the stress-test goal.

---

## Self-Review

- **Spec coverage:** per-particle RGBA tint blended with material (Tasks 1-4) ✓; 64×64×128 lattice (Task 5) ✓; two opaque + glass (Task 5 uses ids 8/9 opaque shared group + 4 glass) ✓; perlin displacement + white-noise jitter (Task 5) ✓; ~15% glass, subtle alpha≈0.2 (Task 5) ✓; new method, old setup commented out (Task 5 Step 3) ✓; unit test for pack + dedup identity (Task 2) ✓; build-all/test + capture verification (Task 6) ✓; no texture growth (tint uses spare `.w`, Task 2 Step 8) ✓.
- **Placeholder scan:** no TBD/TODO; every code step shows full code. ✓
- **Type consistency:** `pack_tint_w(const TriEx*, int, int)` defined (Task 2 Step 4) and called identically in packing (Task 2 Step 8) and test (Task 2 Step 1); `TriEx.tint` is `float4` everywhere; `StaticParticle.tint` is `Vector4` (raylib) and converted to `float4` via `make_float4` at the cell boundary (Task 3 Step 1); `add_particle(..., const Vector4&)` overload declared (Task 1 Step 3) and called in the scene (Task 5). ✓
