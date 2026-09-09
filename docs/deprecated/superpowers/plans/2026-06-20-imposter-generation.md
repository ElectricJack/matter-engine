# Imposter Generation (cage + displacement proxy) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bake a part's heavy triangle BVH into a lighter cage + scalar-displacement + baked-radiance imposter (`.imp`), and relief-march it in the existing ray tracer so a baked imposter can be placed next to a real part in the same frame.

**Architecture:** A GL-free testable core (`imposter_asset.h/.cpp`) owns the on-disk format, the cage (decimated + inflated enclosing hull via the existing mesh-simplifier), the per-triangle UV atlas, and a **CPU** displacement bake (deterministic ray-cast inward, unit-tested per spec §6). A GPU half (`imposter_bake.cpp` + `shaders/imposter_bake.*`) bakes the **color** atlas by rasterizing the cage in UV space and reusing the existing shading GLSL. Runtime adds an `is_imposter` branch + relief march to the traversal shader. The real `.part` BLAS remains the up-close fidelity fallback (LOD swap is deferred spec #3).

**Tech Stack:** C++17, raylib (Mesh/RenderTexture/Shader), GLSL 330 (existing `bvh_tlas_common.glsl` + `materials.glsl`), the headless CHECK-macro test harness (gcc for `extern "C"` C, g++ for C++), WSL→Windows cross-compile as the default GUI target.

**Key references:**
- Spec: `docs/superpowers/specs/2026-06-20-imposter-generation-design.md`
- Format/robustness pattern to mirror: `MatterSurfaceLib/src/part_asset.cpp`, `MatterSurfaceLib/include/part_asset.h`
- Test pattern to mirror: `MatterSurfaceLib/tests/part_asset_tests.cpp`
- Makefile patterns: `MatterSurfaceLib/tests/Makefile:221-236` (PART_TARGET), `:67` (.PHONY), `:70` (clean)
- Simplifier API: `MatterSurfaceLib/include/mesh_simplifier.hpp`
- Geometry types: `MatterSurfaceLib/include/bvh.h` (`Tri`, `TriEx`, `BVHNode`, `BVH`, `BvhMesh`)
- Managers: `MatterSurfaceLib/include/blas_manager.hpp`, `tlas_manager.hpp`

**Deliberate refinements vs. spec (both spec-sanctioned):**
- **Displacement is baked CPU-side**, not in the GPU MRT pass. Spec §6 explicitly permits this ("the geometry half of the bake is CPU even though color is GPU"); it makes the displacement geometry fully unit-testable and removes a GPU/CPU disagreement risk. The GPU pass bakes **only** the color (radiance) atlas. Both cast inward against the same geometry, so they agree.
- **Atlas packing is per-triangle right-triangle cells** (one cage triangle per square grid cell, occupying the cell's lower-left right triangle, with texel padding). Deterministic, testable, "good enough for v1" silhouette/area fidelity.

---

## File Structure

| File | Responsibility | New/Modified |
|---|---|---|
| `MatterSurfaceLib/include/imposter_asset.h` | `ImpGenParams`, `CageVert`, `CageTri`, `ImposterAsset`, format constants, hashing, `save`/`load`, cage build, atlas pack, CPU displacement bake decls | Create |
| `MatterSurfaceLib/src/imposter_asset.cpp` | GL-free implementation of all of the above | Create |
| `MatterSurfaceLib/src/imposter_bake.cpp` | GPU color-atlas bake orchestration (RenderTexture MRT-free single target, readback, dilation driver) | Create |
| `MatterSurfaceLib/shaders/imposter_bake.vs` | UV-space vertex shader (`gl_Position = uv`) carrying world pos + normal | Create |
| `MatterSurfaceLib/shaders/imposter_bake.fs` | Cast inward, shade hit via `materials.glsl`, output radiance + coverage | Create |
| `MatterSurfaceLib/shaders/bvh_tlas_common.glsl` | `is_imposter` instance flag decode + `reliefMarch()` helper + atlas samplers | Modify |
| `MatterSurfaceLib/shaders/raytrace_tlas_blas.fs` | branch imposter hits to baked-color read (closest) / first-crossing (any-hit) | Modify |
| `MatterSurfaceLib/include/tlas_manager.hpp` + `src/tlas_manager.cpp` | per-instance `is_imposter` + atlas handle plumbed into instance metadata | Modify |
| `MatterSurfaceLib/main.cpp` | `MSL_BAKE_IMPOSTER` app-mode; render path that drops a baked imposter beside a real part; atlas texture upload/bind | Modify |
| `MatterSurfaceLib/tests/imposter_asset_tests.cpp` | headless CPU tests (hash, format round-trip+guards, cage enclosure, atlas, displacement reconstruction, source-hash link) | Create |
| `MatterSurfaceLib/tests/Makefile` | `IMP_TARGET` + `run-imp` + clean/.PHONY entries | Modify |

---

## Task 1: Asset header, hashing, cache path, and test/Makefile wiring

**Files:**
- Create: `MatterSurfaceLib/include/imposter_asset.h`
- Create: `MatterSurfaceLib/src/imposter_asset.cpp`
- Create: `MatterSurfaceLib/tests/imposter_asset_tests.cpp`
- Modify: `MatterSurfaceLib/tests/Makefile` (lines 67, 70, append after 236)

- [ ] **Step 1: Write the header**

Create `MatterSurfaceLib/include/imposter_asset.h`:

```cpp
#pragma once

#include "part_asset.h"   // reuse part_asset::fnv1a64
#include "bvh.h"          // Tri (geometry source for the cage + displacement cast)

#include <cstdint>
#include <string>
#include <vector>

// Per-part cage + scalar-displacement + baked-radiance imposter. See
// docs/superpowers/specs/2026-06-20-imposter-generation-design.md
namespace imposter_asset {

constexpr uint32_t kMagic = 0x494D504Fu;   // 'IMPO'
constexpr uint32_t kFormatVersion = 1u;

// Bake parameters; padding-free so it hashes deterministically by bytes.
struct ImpGenParams {
    float    cageRatio;       // simplify_mesh target_ratio for the cage (0..1]
    int      atlasW, atlasH;  // displacement + color atlas dimensions
    float    inflation;       // cage outward inflation along normals (world units)
    int      dispBits;        // 8 or 16: displacement texel precision
    uint32_t seed;            // reserved for determinism / future jitter
};
static_assert(sizeof(ImpGenParams) == 24,
              "ImpGenParams must be padding-free for stable byte hashing");

// 32-byte cage vertex: position, normal, uv. Padding-free.
struct CageVert {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
};
static_assert(sizeof(CageVert) == 32, "CageVert layout guard");

struct CageTri { uint32_t i0, i1, i2; };
static_assert(sizeof(CageTri) == 12, "CageTri layout guard");

// In-memory imposter. disp holds atlasW*atlasH*(dispBits/8) bytes; color holds
// atlasW*atlasH*4 bytes (RGBA8, A = coverage mask: 255 covered, 0 gutter).
struct ImposterAsset {
    float    bounds_min[3] = {0,0,0};
    float    bounds_max[3] = {0,0,0};
    float    max_disp = 0.0f;          // shell thickness == inflation
    float    parallax_radius = 0.0f;   // #3 hint; set to a multiple of bounds extent
    uint32_t atlas_w = 0, atlas_h = 0;
    int      disp_bits = 16;
    uint64_t source_part_hash = 0;
    std::vector<CageVert> verts;
    std::vector<CageTri>  tris;
    std::vector<uint8_t>  disp;        // atlas_w*atlas_h*(disp_bits/8)
    std::vector<uint8_t>  color;       // atlas_w*atlas_h*4
};

// Cache key: FNV-1a of the params XOR the format version.
uint64_t compute_imp_hash(const ImpGenParams& p);

// "imposters/<16-hex>.imp"
std::string cache_path(uint64_t hash);

// Serialize (atomic temp+rename). GL-free. Returns false on I/O failure.
bool save(const std::string& path, const ImposterAsset& a, uint64_t imp_hash);

// Reconstruct. Returns false (caller regenerates) on any header/layout/corruption
// mismatch, imp_hash mismatch, or source_part_hash mismatch. GL-free.
bool load(const std::string& path, uint64_t expected_imp_hash,
          uint64_t expected_source_hash, ImposterAsset& out);

// --- CPU geometry (GL-free, unit-tested) ---

// Build the cage from the merged part triangles: decimate via simplify_mesh to
// p.cageRatio, inflate each vertex outward along its normal by p.inflation so the
// cage encloses the part, pack a per-triangle UV atlas, and fill metadata
// (bounds, max_disp = inflation). Leaves disp/color empty (baked separately).
// Returns false on degenerate input.
bool build_cage(const std::vector<Tri>& part_tris, const ImpGenParams& p,
                uint64_t source_part_hash, ImposterAsset& out);

// Bake the scalar displacement atlas on the CPU: for each covered atlas texel,
// cast a ray from the cage surface inward (-normal) against part_tris and store
// the normalized inward distance in [0,maxDisp]. Sets out.disp and the color
// alpha coverage mask (color rgb left 0 here; GPU fills rgb later). Reuses a
// BVH over part_tris for the cast. Returns false on degenerate input.
bool bake_displacement_cpu(const std::vector<Tri>& part_tris, ImposterAsset& out);

} // namespace imposter_asset
```

- [ ] **Step 2: Write the failing hashing/path test**

Create `MatterSurfaceLib/tests/imposter_asset_tests.cpp`:

```cpp
#include "../include/imposter_asset.h"
#include <cstdio>
#include <cstring>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)

static imposter_asset::ImpGenParams sample_params() {
    imposter_asset::ImpGenParams p{};
    p.cageRatio = 0.1f;
    p.atlasW = 256; p.atlasH = 256;
    p.inflation = 0.05f;
    p.dispBits = 16;
    p.seed = 7u;
    return p;
}

static void test_hash_and_path() {
    using namespace imposter_asset;
    ImpGenParams a = sample_params();
    ImpGenParams b = sample_params();
    CHECK(compute_imp_hash(a) == compute_imp_hash(b), "same params same hash");
    b.seed = 99u;
    CHECK(compute_imp_hash(a) != compute_imp_hash(b), "seed change rehashes");
    b = sample_params(); b.atlasW = 512;
    CHECK(compute_imp_hash(a) != compute_imp_hash(b), "atlasW change rehashes");
    CHECK(cache_path(0x1ull) == "imposters/0000000000000001.imp", "cache_path zero-padded hex");
}

int main() {
    test_hash_and_path();
    if (failures == 0) printf("All imposter_asset tests passed\n");
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 3: Wire the test target into `tests/Makefile`**

Append after line 236 (after the `run-part` rule):

```make
# Imposter asset round-trip + cage/atlas/displacement tests (headless, GL-free).
# material_registry.c via gcc to keep its extern "C" symbols unmangled.
IMP_TARGET = imposter_asset_tests
IMP_CPP = imposter_asset_tests.cpp ../src/imposter_asset.cpp ../src/part_asset.cpp \
          ../src/blas_manager.cpp ../src/bvh.cpp ../src/tlas_manager.cpp \
          ../src/vertex_ao.cpp ../src/occupancy.cpp ../src/mesh_simplifier.cpp
IMP_C   = ../src/material_registry.c
IMP_C_OBJ = material_registry.o

$(IMP_TARGET): $(IMP_CPP) $(IMP_C)
	gcc -c $(IMP_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(IMP_CPP) $(IMP_C_OBJ) -o $(IMP_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)
	rm -f $(IMP_C_OBJ)

run-imp: $(IMP_TARGET)
	./$(IMP_TARGET)
```

Change line 67 (`.PHONY`) to add `run-imp` at the end of the list. Change line 70 (`clean`) to add `$(IMP_TARGET)` at the end of the `rm -f` list.

- [ ] **Step 4: Implement hashing + cache path (minimal, to compile + pass)**

Create `MatterSurfaceLib/src/imposter_asset.cpp` with the namespace and these two functions implemented; stub the rest (`save`/`load`/`build_cage`/`bake_displacement_cpu`) to `return false;` for now so the file links:

```cpp
#include "../include/imposter_asset.h"
#include "../include/mesh_simplifier.hpp"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <sys/stat.h>

namespace imposter_asset {

uint64_t compute_imp_hash(const ImpGenParams& p) {
    return part_asset::fnv1a64(&p, sizeof(p)) ^ static_cast<uint64_t>(kFormatVersion);
}

std::string cache_path(uint64_t hash) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
    return std::string("imposters/") + buf + ".imp";
}

bool save(const std::string&, const ImposterAsset&, uint64_t) { return false; }
bool load(const std::string&, uint64_t, uint64_t, ImposterAsset&) { return false; }
bool build_cage(const std::vector<Tri>&, const ImpGenParams&, uint64_t, ImposterAsset&) { return false; }
bool bake_displacement_cpu(const std::vector<Tri>&, ImposterAsset&) { return false; }

} // namespace imposter_asset
```

- [ ] **Step 5: Build and run the test**

Run: `cd MatterSurfaceLib/tests && make run-imp`
Expected: compiles, prints `All imposter_asset tests passed`.

- [ ] **Step 6: Commit**

```bash
git add MatterSurfaceLib/include/imposter_asset.h MatterSurfaceLib/src/imposter_asset.cpp MatterSurfaceLib/tests/imposter_asset_tests.cpp MatterSurfaceLib/tests/Makefile
git commit -m "feat: imposter asset header, hashing, cache path, test wiring"
```

---

## Task 2: Serialization (save/load) with format guards

**Files:**
- Modify: `MatterSurfaceLib/src/imposter_asset.cpp` (replace `save`/`load` stubs)
- Modify: `MatterSurfaceLib/tests/imposter_asset_tests.cpp` (add round-trip + guard tests)

**On-disk layout** (mirrors `part_asset` discipline — magic, version, sizeof guards, content hash, atomic write):

```
Header (44 bytes)
  magic            u32  'IMPO'
  format_version   u32
  imp_hash         u64
  source_part_hash u64
  sizeof_CageVert  u32
  sizeof_CageTri   u32
  content_hash     u64   // FNV-1a over all bytes after the header
Body
  bounds_min[3] f32, bounds_max[3] f32, max_disp f32, parallax_radius f32
  atlas_w u32, atlas_h u32, disp_bits u32
  vert_count u32, CageVert[vert_count]
  tri_count  u32, CageTri[tri_count]
  disp_byte_count u32, disp bytes
  color_byte_count u32, color bytes
```

- [ ] **Step 1: Write the failing round-trip + guard test**

Add to `imposter_asset_tests.cpp` (call both from `main` before the success print):

```cpp
#include <vector>
#include <cstdint>

static imposter_asset::ImposterAsset sample_asset() {
    using namespace imposter_asset;
    ImposterAsset a;
    a.bounds_min[0]=-1; a.bounds_min[1]=-1; a.bounds_min[2]=-1;
    a.bounds_max[0]= 1; a.bounds_max[1]= 1; a.bounds_max[2]= 1;
    a.max_disp = 0.05f; a.parallax_radius = 4.0f;
    a.atlas_w = 4; a.atlas_h = 4; a.disp_bits = 16;
    a.source_part_hash = 0xDEADBEEFCAFEull;
    a.verts = { {0,0,0, 0,0,1, 0,0}, {1,0,0, 0,0,1, 1,0}, {0,1,0, 0,0,1, 0,1} };
    a.tris  = { {0,1,2} };
    a.disp.assign(a.atlas_w*a.atlas_h*2, 0); for (size_t i=0;i<a.disp.size();++i) a.disp[i]=(uint8_t)(i*7);
    a.color.assign(a.atlas_w*a.atlas_h*4, 0); for (size_t i=0;i<a.color.size();++i) a.color[i]=(uint8_t)(i*3+1);
    return a;
}

static uint32_t rd_u32(const std::vector<uint8_t>& b, size_t off){ uint32_t v; memcpy(&v,b.data()+off,4); return v; }
static std::vector<uint8_t> read_file(const char* p){ FILE* f=fopen(p,"rb"); if(!f) return {}; fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET); std::vector<uint8_t> b(n); size_t g=fread(b.data(),1,n,f); fclose(f); b.resize(g); return b; }
static void write_file(const char* p, const std::vector<uint8_t>& b){ FILE* f=fopen(p,"wb"); fwrite(b.data(),1,b.size(),f); fclose(f); }

static void test_round_trip() {
    using namespace imposter_asset;
    ImposterAsset a = sample_asset();
    const char* path = "test.imp";
    remove(path);
    CHECK(save(path, a, 0xABCDull), "save ok");

    ImposterAsset b;
    CHECK(load(path, 0xABCDull, a.source_part_hash, b), "load ok");
    CHECK(b.atlas_w==a.atlas_w && b.atlas_h==a.atlas_h && b.disp_bits==a.disp_bits, "meta scalars");
    CHECK(b.max_disp==a.max_disp && b.parallax_radius==a.parallax_radius, "meta floats");
    CHECK(b.verts.size()==a.verts.size() && memcmp(b.verts.data(),a.verts.data(),a.verts.size()*sizeof(CageVert))==0, "verts bytes");
    CHECK(b.tris.size()==a.tris.size() && memcmp(b.tris.data(),a.tris.data(),a.tris.size()*sizeof(CageTri))==0, "tris bytes");
    CHECK(b.disp==a.disp, "disp bytes");
    CHECK(b.color==a.color, "color bytes");

    // imp_hash mismatch rejected.
    ImposterAsset c; CHECK(!load(path, 0x9999ull, a.source_part_hash, c), "rejects imp_hash mismatch");
    // source_part_hash mismatch rejected (stale imposter for changed part).
    ImposterAsset d; CHECK(!load(path, 0xABCDull, 0x1234ull, d), "rejects source_part_hash mismatch");
    remove(path);
}

static void test_guards() {
    using namespace imposter_asset;
    ImposterAsset a = sample_asset();
    const char* path = "testg.imp";
    remove(path);
    CHECK(save(path, a, 0x1234ull), "guard save ok");
    std::vector<uint8_t> good = read_file(path);
    { ImposterAsset b; CHECK(load(path, 0x1234ull, a.source_part_hash, b), "unmodified loads"); }
    // sizeof_CageVert is at offset 24 (4+4+8+8).
    { auto bad=good; uint32_t v=rd_u32(bad,24)+1; memcpy(bad.data()+24,&v,4); write_file(path,bad);
      ImposterAsset b; CHECK(!load(path,0x1234ull,a.source_part_hash,b), "rejects sizeof_CageVert mismatch"); }
    // format_version at offset 4.
    { auto bad=good; uint32_t v=rd_u32(bad,4)+1; memcpy(bad.data()+4,&v,4); write_file(path,bad);
      ImposterAsset b; CHECK(!load(path,0x1234ull,a.source_part_hash,b), "rejects version mismatch"); }
    // body corruption (offset 44 = first body byte).
    { auto bad=good; bad[44]^=0xFF; write_file(path,bad);
      ImposterAsset b; CHECK(!load(path,0x1234ull,a.source_part_hash,b), "rejects body corruption"); }
    // magic.
    { auto bad=good; bad[0]^=0xFF; write_file(path,bad);
      ImposterAsset b; CHECK(!load(path,0x1234ull,a.source_part_hash,b), "rejects bad magic"); }
    remove(path);
}
```

Add `test_round_trip(); test_guards();` to `main()`.

- [ ] **Step 2: Run test to verify it fails**

Run: `cd MatterSurfaceLib/tests && make run-imp`
Expected: FAIL (save/load are stubs returning false).

- [ ] **Step 3: Implement save/load**

Replace the `save`/`load` stubs in `imposter_asset.cpp`. Add this anonymous-namespace helper block near the top of the file (after the includes), mirroring `part_asset.cpp`:

```cpp
namespace {
template <class T> void put(std::vector<uint8_t>& b, const T& v){
    const uint8_t* p=reinterpret_cast<const uint8_t*>(&v); b.insert(b.end(),p,p+sizeof(T));
}
void put_bytes(std::vector<uint8_t>& b, const void* d, size_t n){
    const uint8_t* p=static_cast<const uint8_t*>(d); b.insert(b.end(),p,p+n);
}
void ensure_parent_dir(const std::string& path){
    auto pos=path.find_last_of('/'); if(pos==std::string::npos) return;
#ifdef _WIN32
    mkdir(path.substr(0,pos).c_str());
#else
    mkdir(path.substr(0,pos).c_str(), 0755);
#endif
}
struct Reader {
    const uint8_t* p; const uint8_t* end; bool ok=true;
    template <class T> T get(){ T v{}; if(p+sizeof(T)>end){ok=false;return v;} std::memcpy(&v,p,sizeof(T)); p+=sizeof(T); return v; }
    const uint8_t* take(size_t n){ if(p+n>end){ok=false;return nullptr;} const uint8_t* r=p; p+=n; return r; }
};
} // namespace
```

Then implement:

```cpp
bool save(const std::string& path, const ImposterAsset& a, uint64_t imp_hash) {
    std::vector<uint8_t> body;
    put_bytes(body, a.bounds_min, 3*sizeof(float));
    put_bytes(body, a.bounds_max, 3*sizeof(float));
    put<float>(body, a.max_disp);
    put<float>(body, a.parallax_radius);
    put<uint32_t>(body, a.atlas_w);
    put<uint32_t>(body, a.atlas_h);
    put<uint32_t>(body, static_cast<uint32_t>(a.disp_bits));
    put<uint32_t>(body, static_cast<uint32_t>(a.verts.size()));
    put_bytes(body, a.verts.data(), a.verts.size()*sizeof(CageVert));
    put<uint32_t>(body, static_cast<uint32_t>(a.tris.size()));
    put_bytes(body, a.tris.data(), a.tris.size()*sizeof(CageTri));
    put<uint32_t>(body, static_cast<uint32_t>(a.disp.size()));
    put_bytes(body, a.disp.data(), a.disp.size());
    put<uint32_t>(body, static_cast<uint32_t>(a.color.size()));
    put_bytes(body, a.color.data(), a.color.size());

    const uint64_t content_hash = part_asset::fnv1a64(body.data(), body.size());
    std::vector<uint8_t> head;
    put<uint32_t>(head, kMagic);
    put<uint32_t>(head, kFormatVersion);
    put<uint64_t>(head, imp_hash);
    put<uint64_t>(head, a.source_part_hash);
    put<uint32_t>(head, static_cast<uint32_t>(sizeof(CageVert)));
    put<uint32_t>(head, static_cast<uint32_t>(sizeof(CageTri)));
    put<uint64_t>(head, content_hash);

    ensure_parent_dir(path);
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(head.data(),1,head.size(),f)==head.size() &&
              std::fwrite(body.data(),1,body.size(),f)==body.size();
    std::fclose(f);
    if (!ok) { std::remove(tmp.c_str()); return false; }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

bool load(const std::string& path, uint64_t expected_imp_hash,
          uint64_t expected_source_hash, ImposterAsset& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f,0,SEEK_END); long sz=std::ftell(f); std::fseek(f,0,SEEK_SET);
    if (sz < 44) { std::fclose(f); return false; }
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    bool read_ok = std::fread(buf.data(),1,buf.size(),f)==buf.size();
    std::fclose(f);
    if (!read_ok) return false;

    Reader r{ buf.data(), buf.data()+buf.size() };
    const uint32_t magic   = r.get<uint32_t>();
    const uint32_t version = r.get<uint32_t>();
    const uint64_t ihash   = r.get<uint64_t>();
    const uint64_t shash   = r.get<uint64_t>();
    const uint32_t s_vert  = r.get<uint32_t>();
    const uint32_t s_tri   = r.get<uint32_t>();
    const uint64_t content = r.get<uint64_t>();
    if (!r.ok) return false;
    if (magic != kMagic)               return false;
    if (version != kFormatVersion)     return false;
    if (s_vert != sizeof(CageVert))    return false;
    if (s_tri  != sizeof(CageTri))     return false;
    if (ihash != expected_imp_hash)    return false;
    if (shash != expected_source_hash) return false;
    if (part_asset::fnv1a64(r.p, static_cast<size_t>(r.end - r.p)) != content) return false;

    out = ImposterAsset{};
    out.source_part_hash = shash;
    const uint8_t* bmin = r.take(3*sizeof(float));
    const uint8_t* bmax = r.take(3*sizeof(float));
    if (!r.ok) return false;
    std::memcpy(out.bounds_min, bmin, 3*sizeof(float));
    std::memcpy(out.bounds_max, bmax, 3*sizeof(float));
    out.max_disp = r.get<float>();
    out.parallax_radius = r.get<float>();
    out.atlas_w = r.get<uint32_t>();
    out.atlas_h = r.get<uint32_t>();
    out.disp_bits = static_cast<int>(r.get<uint32_t>());
    const uint32_t vc = r.get<uint32_t>();
    if (!r.ok) return false;
    const uint8_t* vp = r.take(vc*sizeof(CageVert));
    if (!r.ok) return false;
    out.verts.resize(vc); std::memcpy(out.verts.data(), vp, vc*sizeof(CageVert));
    const uint32_t tc = r.get<uint32_t>();
    if (!r.ok) return false;
    const uint8_t* tp = r.take(tc*sizeof(CageTri));
    if (!r.ok) return false;
    out.tris.resize(tc); std::memcpy(out.tris.data(), tp, tc*sizeof(CageTri));
    const uint32_t dc = r.get<uint32_t>();
    if (!r.ok) return false;
    const uint8_t* dp = r.take(dc);
    if (!r.ok) return false;
    out.disp.assign(dp, dp+dc);
    const uint32_t cc = r.get<uint32_t>();
    if (!r.ok) return false;
    const uint8_t* cp = r.take(cc);
    if (!r.ok) return false;
    out.color.assign(cp, cp+cc);
    return true;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd MatterSurfaceLib/tests && make run-imp`
Expected: PASS — `All imposter_asset tests passed`.

- [ ] **Step 5: Commit**

```bash
git add MatterSurfaceLib/src/imposter_asset.cpp MatterSurfaceLib/tests/imposter_asset_tests.cpp
git commit -m "feat: imposter .imp save/load with format guards + content hash"
```

---

## Task 3: Build the cage (decimate + inflate + enclose) and pack the UV atlas

**Files:**
- Modify: `MatterSurfaceLib/src/imposter_asset.cpp` (replace `build_cage` stub)
- Modify: `MatterSurfaceLib/tests/imposter_asset_tests.cpp` (synthetic sphere + cage test)

**Design notes (read before coding):**
- The simplifier (`mesh_simplifier.cpp:97`) accepts a **non-indexed** input Mesh (`indices == nullptr`, `vertices` = 3 corners per triangle) and welds co-located corners internally, so there is **no 65535-vertex input limit**. Build the input from `Tri` corners directly; pass `indices=nullptr`, `normals=nullptr`. The decimated **output** uses `unsigned short` indices (fine — the cage is small).
- The imposter cage is stored **non-indexed**: emit 3 `CageVert` per cage triangle (`tris[i] = {3i, 3i+1, 3i+2}`) so each triangle carries its own atlas UVs and tangent frame.
- Inflate each cage vertex outward along its **smoothed vertex normal** (accumulate adjacent face normals on the simplified mesh, normalize) by `p.inflation`. On a closed surface this moves vertices radially outward, guaranteeing enclosure. `out.max_disp = p.inflation`.
- Atlas packing: `grid = ceil(sqrt(cage_tri_count))`, `cell = atlasW/grid` (assume square atlas), `pad = 2` texels. Cage triangle `i` occupies cell `(i%grid, i/grid)`; its 3 UVs are the cell's padded lower-left right-triangle corners: `(cx+pad,cy+pad)`, `(cx+cell-pad,cy+pad)`, `(cx+pad,cy+cell-pad)`, normalized by atlas size.

- [ ] **Step 1: Write the failing cage test**

Add to `imposter_asset_tests.cpp`:

```cpp
#include "../include/bvh.h"
#include <cmath>
#include <vector>

// A UV-sphere of Tri (radius R, centered at origin) for synthetic tests.
static std::vector<Tri> make_sphere_tris(float R, int rings, int sectors) {
    auto P = [&](int ri, int si){
        float v = (float)ri/rings, u = (float)si/sectors;
        float theta = v*3.14159265f, phi = u*2.0f*3.14159265f;
        return make_float3(R*sinf(theta)*cosf(phi), R*cosf(theta), R*sinf(theta)*sinf(phi));
    };
    std::vector<Tri> out;
    for (int ri=0; ri<rings; ++ri) for (int si=0; si<sectors; ++si) {
        float3 a=P(ri,si), b=P(ri+1,si), c=P(ri+1,si+1), d=P(ri,si+1);
        Tri t0; t0.vertex0=a; t0.vertex1=b; t0.vertex2=c;
        t0.centroid=make_float3((a.x+b.x+c.x)/3,(a.y+b.y+c.y)/3,(a.z+b.z+c.z)/3);
        Tri t1; t1.vertex0=a; t1.vertex1=c; t1.vertex2=d;
        t1.centroid=make_float3((a.x+c.x+d.x)/3,(a.y+c.y+d.y)/3,(a.z+c.z+d.z)/3);
        out.push_back(t0); out.push_back(t1);
    }
    return out;
}

static void test_build_cage() {
    using namespace imposter_asset;
    const float R = 1.0f;
    std::vector<Tri> part = make_sphere_tris(R, 24, 24);
    ImpGenParams p{}; p.cageRatio=0.1f; p.atlasW=256; p.atlasH=256; p.inflation=0.08f; p.dispBits=16; p.seed=0;

    ImposterAsset a;
    CHECK(build_cage(part, p, 0xABCull, a), "build_cage ok");
    CHECK(a.source_part_hash == 0xABCull, "cage stores source hash");
    CHECK(a.max_disp == p.inflation, "max_disp == inflation");
    CHECK((int)a.tris.size() < (int)part.size(), "cage decimated below source");
    CHECK(a.verts.size() == a.tris.size()*3, "non-indexed cage (3 verts/tri)");

    // Enclosure: every cage vertex is radially outside the sphere surface.
    bool enclosed = true;
    for (const auto& v : a.verts) {
        float d = sqrtf(v.px*v.px + v.py*v.py + v.pz*v.pz);
        if (d < R - 1e-3f) { enclosed = false; break; }
    }
    CHECK(enclosed, "inflated cage encloses the sphere (all verts >= R)");

    // UVs in [0,1].
    bool uv_ok = true;
    for (const auto& v : a.verts) if (v.u<0||v.u>1||v.v<0||v.v>1) { uv_ok=false; break; }
    CHECK(uv_ok, "all cage UVs in [0,1]");

    // Each triangle occupies a distinct atlas cell (centroids differ).
    bool distinct = true;
    for (size_t i=0;i<a.tris.size() && distinct;++i)
        for (size_t j=i+1;j<a.tris.size() && distinct;++j) {
            float ci_u=(a.verts[3*i].u+a.verts[3*i+1].u+a.verts[3*i+2].u)/3;
            float ci_v=(a.verts[3*i].v+a.verts[3*i+1].v+a.verts[3*i+2].v)/3;
            float cj_u=(a.verts[3*j].u+a.verts[3*j+1].u+a.verts[3*j+2].u)/3;
            float cj_v=(a.verts[3*j].v+a.verts[3*j+1].v+a.verts[3*j+2].v)/3;
            if (fabsf(ci_u-cj_u)<1e-5f && fabsf(ci_v-cj_v)<1e-5f) distinct=false;
        }
    CHECK(distinct, "each cage triangle maps to a distinct atlas cell");
}
```

Add `test_build_cage();` to `main()`.

- [ ] **Step 2: Run test to verify it fails**

Run: `cd MatterSurfaceLib/tests && make run-imp`
Expected: FAIL (`build_cage` stub returns false).

- [ ] **Step 3: Implement `build_cage`**

Add `#include "raylib.h"` near the top of `imposter_asset.cpp` (for `Mesh`/`MemAlloc`/`MemFree`), then replace the stub:

```cpp
static float3 v3(float x,float y,float z){ return make_float3(x,y,z); }
static float3 sub3(float3 a,float3 b){ return make_float3(a.x-b.x,a.y-b.y,a.z-b.z); }
static float3 cross3(float3 a,float3 b){ return make_float3(a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x); }
static float3 norm3(float3 a){ float l=sqrtf(a.x*a.x+a.y*a.y+a.z*a.z); return l>1e-12f?make_float3(a.x/l,a.y/l,a.z/l):make_float3(0,0,0); }

bool build_cage(const std::vector<Tri>& part_tris, const ImpGenParams& p,
                uint64_t source_part_hash, ImposterAsset& out) {
    if (part_tris.empty() || p.atlasW <= 0 || p.atlasH <= 0) return false;

    // Non-indexed input mesh: 3 corners per triangle, simplifier welds internally.
    Mesh in{}; 
    in.triangleCount = (int)part_tris.size();
    in.vertexCount   = in.triangleCount * 3;
    in.vertices = (float*)MemAlloc(sizeof(float)*3*in.vertexCount);
    for (int t=0;t<in.triangleCount;++t) {
        const Tri& tr = part_tris[t];
        const float3 c[3] = { tr.vertex0, tr.vertex1, tr.vertex2 };
        for (int k=0;k<3;++k){ in.vertices[(t*3+k)*3+0]=c[k].x; in.vertices[(t*3+k)*3+1]=c[k].y; in.vertices[(t*3+k)*3+2]=c[k].z; }
    }

    SimplifyOptions opt; opt.target_ratio = p.cageRatio; opt.lock_boundary = false;
    Mesh cage = simplify_mesh(in, opt, nullptr);
    MemFree(in.vertices);
    if (cage.vertexCount == 0 || cage.triangleCount == 0) {
        if (cage.vertices) MemFree(cage.vertices);
        if (cage.normals)  MemFree(cage.normals);
        if (cage.indices)  MemFree(cage.indices);
        return false;
    }

    // Smoothed per-vertex normals on the simplified mesh.
    std::vector<float3> vn(cage.vertexCount, make_float3(0,0,0));
    auto getv = [&](int i){ return make_float3(cage.vertices[i*3+0],cage.vertices[i*3+1],cage.vertices[i*3+2]); };
    for (int t=0;t<cage.triangleCount;++t) {
        int i0=cage.indices[t*3+0], i1=cage.indices[t*3+1], i2=cage.indices[t*3+2];
        float3 fn = cross3(sub3(getv(i1),getv(i0)), sub3(getv(i2),getv(i0)));
        vn[i0]=make_float3(vn[i0].x+fn.x,vn[i0].y+fn.y,vn[i0].z+fn.z);
        vn[i1]=make_float3(vn[i1].x+fn.x,vn[i1].y+fn.y,vn[i1].z+fn.z);
        vn[i2]=make_float3(vn[i2].x+fn.x,vn[i2].y+fn.y,vn[i2].z+fn.z);
    }
    for (auto& n : vn) n = norm3(n);

    // Atlas packing grid.
    const int nt = cage.triangleCount;
    int grid = (int)ceilf(sqrtf((float)nt)); if (grid < 1) grid = 1;
    const float cell = (float)p.atlasW / (float)grid; // assume square atlas
    const float pad = 2.0f;
    const float aw = (float)p.atlasW, ah = (float)p.atlasH;

    out = ImposterAsset{};
    out.source_part_hash = source_part_hash;
    out.atlas_w = (uint32_t)p.atlasW;
    out.atlas_h = (uint32_t)p.atlasH;
    out.disp_bits = p.dispBits;
    out.max_disp = p.inflation;
    out.verts.reserve(nt*3);
    out.tris.reserve(nt);

    float bmin[3]={1e30f,1e30f,1e30f}, bmax[3]={-1e30f,-1e30f,-1e30f};
    for (int t=0;t<nt;++t) {
        int idx[3] = { (int)cage.indices[t*3+0], (int)cage.indices[t*3+1], (int)cage.indices[t*3+2] };
        int gx = t % grid, gy = t / grid;
        float cx = gx*cell, cy = gy*cell;
        float uv[3][2] = {
            {(cx+pad)/aw,        (cy+pad)/ah},
            {(cx+cell-pad)/aw,   (cy+pad)/ah},
            {(cx+pad)/aw,        (cy+cell-pad)/ah},
        };
        for (int k=0;k<3;++k) {
            float3 pos = getv(idx[k]);
            float3 n   = vn[idx[k]];
            float3 ip  = make_float3(pos.x + n.x*p.inflation, pos.y + n.y*p.inflation, pos.z + n.z*p.inflation);
            CageVert cv; cv.px=ip.x; cv.py=ip.y; cv.pz=ip.z;
            cv.nx=n.x; cv.ny=n.y; cv.nz=n.z; cv.u=uv[k][0]; cv.v=uv[k][1];
            out.verts.push_back(cv);
            bmin[0]=fminf(bmin[0],ip.x); bmin[1]=fminf(bmin[1],ip.y); bmin[2]=fminf(bmin[2],ip.z);
            bmax[0]=fmaxf(bmax[0],ip.x); bmax[1]=fmaxf(bmax[1],ip.y); bmax[2]=fmaxf(bmax[2],ip.z);
        }
        out.tris.push_back({ (uint32_t)(t*3), (uint32_t)(t*3+1), (uint32_t)(t*3+2) });
    }
    for (int i=0;i<3;++i){ out.bounds_min[i]=bmin[i]; out.bounds_max[i]=bmax[i]; }
    float ext = fmaxf(bmax[0]-bmin[0], fmaxf(bmax[1]-bmin[1], bmax[2]-bmin[2]));
    out.parallax_radius = ext * 6.0f; // #3 hint; tune later

    MemFree(cage.vertices); MemFree(cage.normals); MemFree(cage.indices);
    return true;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd MatterSurfaceLib/tests && make run-imp`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add MatterSurfaceLib/src/imposter_asset.cpp MatterSurfaceLib/tests/imposter_asset_tests.cpp
git commit -m "feat: cage build via simplifier with inflation + per-triangle UV atlas"
```

---

## Task 4: CPU displacement bake + coverage mask

**Files:**
- Modify: `MatterSurfaceLib/src/imposter_asset.cpp` (replace `bake_displacement_cpu` stub)
- Modify: `MatterSurfaceLib/tests/imposter_asset_tests.cpp` (reconstruction test)

**Design notes:**
- Build one `BVH` over the part triangles (`BvhMesh` + `BVH(mesh)`; see `blas_manager.cpp:156-181` and `bvh.cpp:74`). Cast with a `BVHRay` (`O`, `D`, `rD = 1/D`, `hit.t = 1e30f`), call `bvh.Intersect(ray, 0)`.
- For each atlas texel: recompute the same `grid`/`cell`/`pad` as `build_cage`; map texel → cage triangle `t` and barycentric `(w0,w1,w2)`; interpolate the inflated cage position + normal; cast inward along `-normal`. Covered texels (inside the padded right-triangle and ray hits) store the inward distance; gutter/miss texels get coverage 0.
- Track the observed max inward distance and set `out.max_disp` to it (so the cage truly bounds the surface and the [0,max_disp] range is used without clipping). Store displacement normalized to `[0,1]` as `dispBits` (8 → 1 byte, 16 → 2 bytes LE). Color alpha = coverage (255/0); rgb left 0 (GPU bake fills rgb later).

- [ ] **Step 1: Write the failing reconstruction test**

Add to `imposter_asset_tests.cpp`:

```cpp
static void test_displacement_reconstruction() {
    using namespace imposter_asset;
    const float R = 1.0f;
    std::vector<Tri> part = make_sphere_tris(R, 32, 32);
    ImpGenParams p{}; p.cageRatio=0.2f; p.atlasW=128; p.atlasH=128; p.inflation=0.12f; p.dispBits=16; p.seed=0;

    ImposterAsset a;
    CHECK(build_cage(part, p, 0x1ull, a), "cage for displacement ok");
    CHECK(bake_displacement_cpu(part, a), "displacement bake ok");
    CHECK(a.disp.size() == (size_t)a.atlas_w*a.atlas_h*2, "disp sized for R16");
    CHECK(a.color.size() == (size_t)a.atlas_w*a.atlas_h*4, "color sized RGBA8");
    CHECK(a.max_disp >= p.inflation, "max_disp at least inflation");

    // Reconstruct surface points from covered texels; assert they lie on the sphere.
    int covered = 0; float max_err = 0.0f;
    const int W=a.atlas_w, H=a.atlas_h;
    int grid = (int)ceilf(sqrtf((float)a.tris.size()));
    float cell = (float)W/(float)grid; float pad=2.0f;
    for (int y=0;y<H;++y) for (int x=0;x<W;++x) {
        if (a.color[(y*W+x)*4+3] == 0) continue; // gutter/miss
        int gx=(int)((x+0.5f)/cell), gy=(int)((y+0.5f)/cell);
        int t = gy*grid+gx; if (t<0 || t>=(int)a.tris.size()) continue;
        float fu = ((x+0.5f)-(gx*cell+pad))/(cell-2*pad);
        float fv = ((y+0.5f)-(gy*cell+pad))/(cell-2*pad);
        float w1=fu, w2=fv, w0=1.0f-fu-fv;
        const CageVert& A=a.verts[3*t], &B=a.verts[3*t+1], &C=a.verts[3*t+2];
        float px=w0*A.px+w1*B.px+w2*C.px, py=w0*A.py+w1*B.py+w2*C.py, pz=w0*A.pz+w1*B.pz+w2*C.pz;
        float nx=w0*A.nx+w1*B.nx+w2*C.nx, ny=w0*A.ny+w1*B.ny+w2*C.ny, nz=w0*A.nz+w1*B.nz+w2*C.nz;
        float nl=sqrtf(nx*nx+ny*ny+nz*nz); nx/=nl; ny/=nl; nz/=nl;
        uint16_t raw; memcpy(&raw, &a.disp[(y*W+x)*2], 2);
        float d = (raw/65535.0f)*a.max_disp;
        float sx=px-nx*d, sy=py-ny*d, sz=pz-nz*d;
        float err = fabsf(sqrtf(sx*sx+sy*sy+sz*sz) - R);
        if (err>max_err) max_err=err;
        ++covered;
    }
    CHECK(covered > 100, "displacement covered a meaningful texel count");
    CHECK(max_err < 0.05f, "reconstructed surface within 5% of sphere radius");
}
```

Add `test_displacement_reconstruction();` to `main()`.

- [ ] **Step 2: Run test to verify it fails**

Run: `cd MatterSurfaceLib/tests && make run-imp`
Expected: FAIL (`bake_displacement_cpu` stub returns false).

- [ ] **Step 3: Implement `bake_displacement_cpu`**

Replace the stub in `imposter_asset.cpp`:

```cpp
bool bake_displacement_cpu(const std::vector<Tri>& part_tris, ImposterAsset& out) {
    if (part_tris.empty() || out.tris.empty() || out.atlas_w==0 || out.atlas_h==0) return false;

    // BVH over the part (BvhMesh owns a copy so the BVH's lifetime is self-contained).
    BvhMesh mesh{};
    mesh.triCount = (int)part_tris.size();
    mesh.tri = (Tri*)MALLOC64(sizeof(Tri)*mesh.triCount);
    for (int i=0;i<mesh.triCount;++i) mesh.tri[i] = part_tris[i];
    BVH bvh(&mesh);

    const int W=(int)out.atlas_w, H=(int)out.atlas_h;
    const int nt=(int)out.tris.size();
    int grid=(int)ceilf(sqrtf((float)nt)); if(grid<1) grid=1;
    const float cell=(float)W/(float)grid, pad=2.0f;
    const int bytes = out.disp_bits/8;

    // First pass: cast all covered texels, record raw distances + max.
    std::vector<float> dist((size_t)W*H, -1.0f);
    float observed_max = 1e-6f;
    for (int y=0;y<H;++y) for (int x=0;x<W;++x) {
        int gx=(int)((x+0.5f)/cell), gy=(int)((y+0.5f)/cell);
        int t=gy*grid+gx; if(t<0||t>=nt) continue;
        float fu=((x+0.5f)-(gx*cell+pad))/(cell-2*pad);
        float fv=((y+0.5f)-(gy*cell+pad))/(cell-2*pad);
        if (fu<0||fv<0||fu+fv>1.0f) continue; // gutter (outside the padded right-tri)
        float w1=fu,w2=fv,w0=1.0f-fu-fv;
        const CageVert& A=out.verts[3*t]; const CageVert& B=out.verts[3*t+1]; const CageVert& C=out.verts[3*t+2];
        float3 pos=make_float3(w0*A.px+w1*B.px+w2*C.px, w0*A.py+w1*B.py+w2*C.py, w0*A.pz+w1*B.pz+w2*C.pz);
        float3 n=norm3(make_float3(w0*A.nx+w1*B.nx+w2*C.nx, w0*A.ny+w1*B.ny+w2*C.ny, w0*A.nz+w1*B.nz+w2*C.nz));
        float3 dir=make_float3(-n.x,-n.y,-n.z);
        BVHRay ray; ray.O=pos; ray.D=dir; ray.rD=make_float3(1.0f/dir.x,1.0f/dir.y,1.0f/dir.z);
        ray.hit.t=1e30f;
        bvh.Intersect(ray, 0);
        if (ray.hit.t < 1e29f && ray.hit.t > 0.0f) {
            dist[(size_t)y*W+x]=ray.hit.t;
            if (ray.hit.t>observed_max) observed_max=ray.hit.t;
        }
    }
    out.max_disp = fmaxf(out.max_disp, observed_max);

    // Second pass: normalize + write disp + coverage.
    out.disp.assign((size_t)W*H*bytes, 0);
    out.color.assign((size_t)W*H*4, 0);
    for (int i=0;i<W*H;++i) {
        float d=dist[i];
        if (d<0.0f) { out.color[i*4+3]=0; continue; } // miss/gutter
        float nrm = d/out.max_disp; if(nrm>1.0f) nrm=1.0f; if(nrm<0.0f) nrm=0.0f;
        if (bytes==2) { uint16_t v=(uint16_t)(nrm*65535.0f+0.5f); memcpy(&out.disp[(size_t)i*2], &v, 2); }
        else          { out.disp[i]=(uint8_t)(nrm*255.0f+0.5f); }
        out.color[i*4+3]=255; // coverage
    }

    MemFree(mesh.tri);
    return true;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd MatterSurfaceLib/tests && make run-imp`
Expected: PASS — all imposter_asset tests pass.

- [ ] **Step 5: Commit**

```bash
git add MatterSurfaceLib/src/imposter_asset.cpp MatterSurfaceLib/tests/imposter_asset_tests.cpp
git commit -m "feat: CPU displacement bake with coverage mask + reconstruction test"
```

---

## Task 5: Gutter dilation (CPU, unit-tested)

Bilinear sampling at chart edges bleeds the black gutter into covered texels. Dilation spreads covered colors outward into adjacent uncovered texels (coverage stays the authoritative mask). Pure CPU → TDD.

**Files:**
- Modify: `MatterSurfaceLib/include/imposter_asset.h` (declare `dilate_atlas`)
- Modify: `MatterSurfaceLib/src/imposter_asset.cpp` (implement)
- Modify: `MatterSurfaceLib/tests/imposter_asset_tests.cpp` (test)

- [ ] **Step 1: Declare the function**

Add to `imposter_asset.h` after `bake_displacement_cpu`:

```cpp
// Spread covered color (rgb) into uncovered neighbor texels, `passes` times, using
// the alpha coverage mask. Coverage values themselves are NOT changed (they remain
// the runtime hit/miss authority); only rgb in gutter texels is filled so bilinear
// sampling near chart edges does not pull in black. GL-free.
void dilate_atlas(ImposterAsset& a, int passes);
```

- [ ] **Step 2: Write the failing test**

Add to `imposter_asset_tests.cpp`:

```cpp
static void test_dilate_atlas() {
    using namespace imposter_asset;
    ImposterAsset a;
    a.atlas_w=4; a.atlas_h=1; a.disp_bits=8;
    a.color.assign(4*4, 0);
    // texel 0 covered, red; texels 1..3 uncovered black.
    a.color[0]=200; a.color[1]=10; a.color[2]=10; a.color[3]=255;
    dilate_atlas(a, 1);
    // texel 1 should have picked up texel 0's rgb (neighbor), coverage still 0.
    CHECK(a.color[1*4+0]==200 && a.color[1*4+1]==10 && a.color[1*4+2]==10, "dilate fills neighbor rgb");
    CHECK(a.color[1*4+3]==0, "dilate leaves coverage unchanged");
    CHECK(a.color[0]==200 && a.color[3]==255, "dilate preserves covered texel");
    CHECK(a.color[3*4+3]==0 && a.color[3*4+0]==0, "dilate stops beyond reach in one pass");
}
```

Add `test_dilate_atlas();` to `main()`.

- [ ] **Step 3: Run test to verify it fails**

Run: `cd MatterSurfaceLib/tests && make run-imp`
Expected: FAIL (undefined `dilate_atlas`).

- [ ] **Step 4: Implement `dilate_atlas`**

Add to `imposter_asset.cpp`:

```cpp
void dilate_atlas(ImposterAsset& a, int passes) {
    if (a.color.empty() || a.atlas_w==0 || a.atlas_h==0) return;
    const int W=(int)a.atlas_w, H=(int)a.atlas_h;
    for (int pass=0; pass<passes; ++pass) {
        std::vector<uint8_t> cov(W*H);
        for (int i=0;i<W*H;++i) cov[i]=a.color[i*4+3];
        std::vector<uint8_t> next = a.color;
        const int dx[8]={-1,1,0,0,-1,-1,1,1}, dy[8]={0,0,-1,1,-1,1,-1,1};
        for (int y=0;y<H;++y) for (int x=0;x<W;++x) {
            int i=y*W+x;
            if (cov[i]!=0) continue; // already covered: keep
            int rs=0,gs=0,bs=0,n=0;
            for (int k=0;k<8;++k) {
                int nx=x+dx[k], ny=y+dy[k];
                if (nx<0||ny<0||nx>=W||ny>=H) continue;
                int j=ny*W+nx;
                if (cov[j]==0) continue;
                rs+=a.color[j*4+0]; gs+=a.color[j*4+1]; bs+=a.color[j*4+2]; ++n;
            }
            if (n>0) {
                next[i*4+0]=(uint8_t)(rs/n); next[i*4+1]=(uint8_t)(gs/n); next[i*4+2]=(uint8_t)(bs/n);
                next[i*4+3]=1; // mark as "filled gutter" so the next pass can spread further
            }
        }
        a.color.swap(next);
    }
    // Reset the temporary fill markers (1) back to 0 so coverage stays {0,255}.
    for (int i=0;i<W*H;++i) if (a.color[i*4+3]==1) a.color[i*4+3]=0;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cd MatterSurfaceLib/tests && make run-imp`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add MatterSurfaceLib/include/imposter_asset.h MatterSurfaceLib/src/imposter_asset.cpp MatterSurfaceLib/tests/imposter_asset_tests.cpp
git commit -m "feat: CPU gutter dilation for imposter color atlas"
```

---

## Task 6: GPU color (radiance) atlas bake

Fills the color atlas rgb by rasterizing the cage in UV space and, per texel, casting a ray inward against the **already-uploaded** part BVH and shading the hit with the existing GLSL (so the bake cannot disagree with how parts render). CPU coverage (Task 4) stays authoritative; the GPU fills only rgb for covered texels; then dilate (Task 5).

**Not unit-testable** (needs a GL context). Verified by building the GUI target and inspecting the baked atlas + the round-trip `.imp`. The geometry math it depends on is already covered by Tasks 3-4.

**Files:**
- Create: `MatterSurfaceLib/shaders/imposter_bake.vs`
- Create: `MatterSurfaceLib/shaders/imposter_bake.fs`
- Create: `MatterSurfaceLib/src/imposter_bake.cpp`
- Modify: `MatterSurfaceLib/Makefile` (preprocess rule + add `imposter_bake.cpp` to sources)

- [ ] **Step 1: Write the bake vertex shader**

Create `shaders/imposter_bake.vs`:

```glsl
#version 330 core
in vec3 vertexPosition;   // cage world-space (part-space) position
in vec3 vertexNormal;     // cage outward normal
in vec2 vertexTexCoord;   // cage atlas UV

out vec3 cageWorldPos;
out vec3 cageNormal;

void main() {
    cageWorldPos = vertexPosition;
    cageNormal   = vertexNormal;
    // Rasterize directly in UV space: UV [0,1] -> NDC [-1,1].
    gl_Position = vec4(vertexTexCoord * 2.0 - 1.0, 0.0, 1.0);
}
```

- [ ] **Step 2: Write the bake fragment shader**

Create `shaders/imposter_bake.fs`. It reuses the shading + traversal includes (expanded by the existing shader preprocessor, see `Makefile:154`):

```glsl
#version 330 core
in vec3 cageWorldPos;
in vec3 cageNormal;
out vec4 fragColor;

uniform float maxDisp;       // shell thickness; ray marches at most this far inward

#include "materials.glsl"
#include "bvh_tlas_common.glsl"

void main() {
    vec3 n = normalize(cageNormal);
    vec3 origin = cageWorldPos;
    vec3 dir = -n;                       // inward
    HitResult hit = intersectScene(origin, dir, maxDisp * 1.5);
    if (!hit.hit) { fragColor = vec4(0.0, 0.0, 0.0, 0.0); return; }

    vec3 hitPos = origin + dir * hit.t;
    vec3 hn = normalize(hit.normal);
    MaterialProperties matProps = getMaterialProperties(hit.material);
    vec3 albedo = mix(matProps.albedo, hit.tint, hit.tintAlpha);

    // Fixed outward view (no live camera): bake radiance as seen from outside the cage.
    vec3 viewDir = dir;
    uint seed = uint(gl_FragCoord.x) * 1973u + uint(gl_FragCoord.y) * 9277u + 1u;
    vec3 radiance = calculatePBR(hitPos, hn, viewDir, albedo, matProps.roughness,
                                 matProps.metallic, hit.ao, true, seed);
    if (matProps.emission > 0.0) radiance += matProps.albedo * matProps.emission;
    fragColor = vec4(radiance, 1.0);
}
```

- [ ] **Step 3: Add the preprocess rule + source to `MatterSurfaceLib/Makefile`**

After the existing `shaders/raytrace_tlas_blas_processed.fs:` rule (around line 154-156), add:

```make
shaders/imposter_bake_processed.fs: shaders/imposter_bake.fs shaders/bvh_tlas_common.glsl shaders/materials.glsl $(PREPROCESSOR)
	@echo "Processing imposter bake shader with includes (C++)..."
	$(PREPROCESSOR) shaders/imposter_bake.fs shaders/imposter_bake_processed.fs
```

Change the `shaders:` target (line 141) to also depend on `shaders/imposter_bake_processed.fs`. Add `imposter_bake.cpp` and `imposter_asset.cpp` to the C++ source list used by the `$(BIN)` link (find where `part_asset.cpp` / `tlas_manager.cpp` are listed in the OBJ/SRC variables and add `src/imposter_asset.cpp src/imposter_bake.cpp` alongside). Add the two `_processed.fs` to the `clean` rule's `rm -f` (line 319/336).

- [ ] **Step 4: Write the bake orchestration**

Create `MatterSurfaceLib/src/imposter_bake.cpp`:

```cpp
extern "C" {
    #include "raylib.h"
    #include "rlgl.h"
    #include "raymath.h"
}
#include "../include/imposter_asset.h"
#include "../include/blas_manager.hpp"
#include "../include/tlas_manager.hpp"
#include <vector>

namespace imposter_asset {

// Build the full imposter: CPU cage + displacement, then GPU radiance, then dilate.
// blas/tlas must already hold the source part's geometry and be GPU-uploaded
// (caller does part_asset::load + ensure_gpu_textures_ready + bind). part_tris is
// the flattened part geometry (same set used to build blas). Requires a live GL
// context + window. Returns false on failure.
bool bake_imposter(const ImpGenParams& p, const std::vector<Tri>& part_tris,
                   uint64_t source_part_hash,
                   BLASManager& blas, TLASManager& tlas, ImposterAsset& out) {
    if (!build_cage(part_tris, p, source_part_hash, out)) return false;
    if (!bake_displacement_cpu(part_tris, out)) return false;

    // Upload the cage as a raylib Mesh (positions/normals/texcoords from out.verts).
    const int vc = (int)out.verts.size();
    Mesh cage{};
    cage.vertexCount = vc;
    cage.triangleCount = (int)out.tris.size();
    cage.vertices  = (float*)MemAlloc(sizeof(float)*3*vc);
    cage.normals   = (float*)MemAlloc(sizeof(float)*3*vc);
    cage.texcoords = (float*)MemAlloc(sizeof(float)*2*vc);
    for (int i=0;i<vc;++i) {
        cage.vertices[i*3+0]=out.verts[i].px; cage.vertices[i*3+1]=out.verts[i].py; cage.vertices[i*3+2]=out.verts[i].pz;
        cage.normals[i*3+0]=out.verts[i].nx;  cage.normals[i*3+1]=out.verts[i].ny;  cage.normals[i*3+2]=out.verts[i].nz;
        cage.texcoords[i*2+0]=out.verts[i].u; cage.texcoords[i*2+1]=out.verts[i].v;
    }
    UploadMesh(&cage, false);

    Shader bake = LoadShader("shaders/imposter_bake.vs", "shaders/imposter_bake_processed.fs");
    RenderTexture2D rt = LoadRenderTexture((int)out.atlas_w, (int)out.atlas_h);

    // Bind the part BVH textures + material table + lighting uniforms to the bake shader.
    blas.bind_to_shader(bake);
    tlas.bind_to_shader(bake, blas);
    int maxDispLoc = GetShaderLocation(bake, "maxDisp");
    SetShaderValue(bake, maxDispLoc, &out.max_disp, SHADER_UNIFORM_FLOAT);
    // intersectionMode = TLAS/BLAS traversal (1); set any lighting flags the
    // shading path reads (giStrength/shadowStrength/aoEnabled) to their bake values.
    int modeLoc = GetShaderLocation(bake, "intersectionMode"); int one = 1;
    SetShaderValue(bake, modeLoc, &one, SHADER_UNIFORM_INT);
    float fOne = 1.0f;
    SetShaderValue(bake, GetShaderLocation(bake, "giStrength"), &fOne, SHADER_UNIFORM_FLOAT);
    SetShaderValue(bake, GetShaderLocation(bake, "shadowStrength"), &fOne, SHADER_UNIFORM_FLOAT);
    int aoOn = 1; SetShaderValue(bake, GetShaderLocation(bake, "aoEnabled"), &aoOn, SHADER_UNIFORM_INT);

    Material mat = LoadMaterialDefault();
    mat.shader = bake;

    BeginTextureMode(rt);
        ClearBackground(BLANK);
        rlDisableBackfaceCulling();
        rlDisableDepthTest();
        DrawMesh(cage, mat, MatrixIdentity());
        rlEnableDepthTest();
    EndTextureMode();

    // Read back rgb into out.color, preserving the CPU coverage alpha.
    Image img = LoadImageFromTexture(rt.texture);     // RGBA8, y-flipped
    ImageFlipVertical(&img);
    const unsigned char* px = (const unsigned char*)img.data;
    const int W=(int)out.atlas_w, H=(int)out.atlas_h;
    for (int i=0;i<W*H;++i) {
        out.color[i*4+0]=px[i*4+0];
        out.color[i*4+1]=px[i*4+1];
        out.color[i*4+2]=px[i*4+2];
        // out.color[i*4+3] keeps CPU coverage from bake_displacement_cpu
    }
    UnloadImage(img);

    dilate_atlas(out, 4);

    UnloadRenderTexture(rt);
    UnloadShader(bake);
    mat.shader = (Shader){0};   // do not let UnloadMaterial free the shared bake shader
    UnloadMaterial(mat);
    UnloadMesh(cage);
    return true;
}

} // namespace imposter_asset
```

Add the `bake_imposter` declaration to `imposter_asset.h` guarded so the GL-free test build never sees it pull in raylib GPU symbols — declare it in the header (declaration only is fine; the test target simply does not link `imposter_bake.cpp`):

```cpp
// GPU bake (defined in imposter_bake.cpp; requires a live GL context). Combines
// the CPU cage + displacement with a GPU radiance pass and dilation.
bool bake_imposter(const ImpGenParams& p, const std::vector<Tri>& part_tris,
                   uint64_t source_part_hash,
                   class BLASManager& blas, class TLASManager& tlas, ImposterAsset& out);
```

- [ ] **Step 5: Build the GUI target**

Run: `cd MatterSurfaceLib && make` (default = WSL→Windows cross-compile producing `gpu_raytrace.exe`).
Expected: compiles and links; `shaders/imposter_bake_processed.fs` is generated.

- [ ] **Step 6: Commit**

```bash
git add MatterSurfaceLib/shaders/imposter_bake.vs MatterSurfaceLib/shaders/imposter_bake.fs MatterSurfaceLib/src/imposter_bake.cpp MatterSurfaceLib/include/imposter_asset.h MatterSurfaceLib/Makefile
git commit -m "feat: GPU radiance bake for imposter color atlas"
```

---

## Task 7: Runtime relief-march + baked-color read

Render an imposter as a full participant: when a camera or GI ray hits an imposter cage triangle, relief-march the displacement atlas to the true surface and return the **baked radiance** texel (bypassing `calculatePBR`). A coverage miss passes through (no hit).

**Scope (v1):** one bound imposter atlas pair + **global** imposter params (the render test places one baked imposter beside a real part — spec Scope #2). Per-instance only carries the `is_imposter` flag. Multi-imposter atlas arrays are deferred (#3). **Shadow/GI any-hit** treats the imposter at **cage granularity** in v1 (the cage triangle is already in the BVH, so `shadowQuery` already returns occluded on a cage hit — a slightly inflated but correct-direction shadow); exact first-crossing shadow marching is a documented follow-up. Because the atlas UV packing is deterministic (Task 3), the shader **recomputes** each cage triangle's UVs from its index — no UV channel is added to the triangle texture.

**Not unit-testable** (GLSL); verified via `MSL_CAPTURE`.

**Files:**
- Modify: `MatterSurfaceLib/include/tlas_manager.hpp` + `src/tlas_manager.cpp` (per-instance `is_imposter`)
- Modify: `MatterSurfaceLib/shaders/bvh_tlas_common.glsl` (instance flag decode, atlas uniforms, `reliefMarch`, HitResult fields, closest-hit branch)
- Modify: `MatterSurfaceLib/shaders/raytrace_tlas_blas.fs` (use baked color for imposter hits)

- [ ] **Step 1: Plumb per-instance `is_imposter` (CPU)**

In `tlas_manager.hpp`, add `bool is_imposter = false;` to both `DrawInstance` and `DrawRecord` (and to the `DrawRecord` constructor / `draw_batch` copy). In `src/tlas_manager.cpp`, in `generate_instance_texture_data` where row 8 metadata is written (around line 396-398), replace the two padding writes:

```cpp
output_data[metadataIdx + 1] = static_cast<float>(materialId);
output_data[metadataIdx + 2] = draw_records_[i].is_imposter ? 1.0f : 0.0f; // is_imposter flag
output_data[metadataIdx + 3] = 0.0f; // reserved
```

- [ ] **Step 2: Decode the flag + add atlas uniforms (common.glsl)**

In `bvh_tlas_common.glsl`, add to `struct BVHInstance` (after `uint materialId;`):

```glsl
    bool isImposter;
```

In `decodeInstance`, after `inst.materialId = uint(metadata.y);` (line ~270) add:

```glsl
    inst.isImposter = metadata.z > 0.5;
```

Near the other uniforms (top of file, after line 13) add:

```glsl
// --- Imposter (v1: single bound atlas + global params) ---
uniform sampler2D imposterColorTex;   // baked radiance (rgb) + coverage (a)
uniform sampler2D imposterDispTex;    // scalar inward depth, normalized [0,1]
uniform int   imposterGrid;           // atlas cell grid (ceil(sqrt(cageTriCount)))
uniform int   imposterTriBase;        // global triangle index of the cage's first triangle
uniform float imposterMaxDisp;        // shell thickness (denormalizes displacement)
uniform vec2  imposterAtlasSize;      // (atlasW, atlasH) for padding math
uniform float imposterPad;            // gutter padding in texels (matches build_cage)
```

Add to `struct HitResult` (after `int instanceId;`):

```glsl
    bool isImposter;
    vec3 bakedColor;   // valid when isImposter && hit (baked radiance to display)
```

- [ ] **Step 3: Add the relief-march helper (common.glsl)**

Add this function **above** `intersectScene` (e.g. before line 601). It recomputes the cage triangle UVs deterministically (matching `build_cage`), builds the world↔UV mapping, marches, and returns whether a covered crossing was found:

```glsl
// Deterministic per-cage-triangle UVs (must match build_cage in imposter_asset.cpp).
void imposterTriUVs(int localTri, out vec2 uv0, out vec2 uv1, out vec2 uv2) {
    int gx = localTri % imposterGrid;
    int gy = localTri / imposterGrid;
    float cU = 1.0 / float(imposterGrid);
    float cV = 1.0 / float(imposterGrid);
    float pU = imposterPad / imposterAtlasSize.x;
    float pV = imposterPad / imposterAtlasSize.y;
    float bx = float(gx) * cU, by = float(gy) * cV;
    uv0 = vec2(bx + pU,        by + pU);
    uv1 = vec2(bx + cU - pU,   by + pU);
    uv2 = vec2(bx + pU,        by + cV - pV);
}

// Relief-march from the cage entry into the displacement shell. Returns true and
// the hit UV when a covered crossing is found within maxDisp; false = pass through.
bool reliefMarch(vec3 entryPos, vec3 rayDir,
                 vec3 v0, vec3 v1, vec3 v2,
                 vec2 uv0, vec2 uv1, vec2 uv2,
                 vec3 cageN, out vec2 hitUV) {
    vec3 dpdu, dpdv;
    {
        vec3 e1 = v1 - v0, e2 = v2 - v0;
        vec2 d1 = uv1 - uv0, d2 = uv2 - uv0;
        float det = d1.x * d2.y - d2.x * d1.y;
        if (abs(det) < 1e-12) return false;
        float r = 1.0 / det;
        dpdu = r * ( d2.y * e1 - d1.y * e2);
        dpdv = r * (-d2.x * e1 + d1.x * e2);
    }
    // Barycentric entry UV.
    vec3 e1 = v1 - v0, e2 = v2 - v0, ep = entryPos - v0;
    float d00=dot(e1,e1), d01=dot(e1,e2), d11=dot(e2,e2), d20=dot(ep,e1), d21=dot(ep,e2);
    float den = d00*d11 - d01*d01;
    float bv = (d11*d20 - d01*d21) / den;
    float bw = (d00*d21 - d01*d20) / den;
    float bu = 1.0 - bv - bw;
    vec2 entryUV = bu*uv0 + bv*uv1 + bw*uv2;

    // World ray -> (du, dv, dn) rates. dn<0 = going inward (below cage along N).
    mat3 M = mat3(dpdu, dpdv, cageN);
    vec3 duvn = inverse(M) * rayDir;
    float inward = -duvn.z;
    if (inward <= 1e-5) return false; // ray not entering the shell

    const int LIN = 32;
    const int BIN = 6;
    float sMax = imposterMaxDisp / inward;   // arc length to reach full depth
    float ds = sMax / float(LIN);
    float prevS = 0.0, prevDiff = 0.0; bool have_prev = false;
    for (int i = 1; i <= LIN; ++i) {
        float s = ds * float(i);
        vec2 uvc = entryUV + duvn.xy * s;
        float pen = inward * s;                       // penetration below cage
        float d = texture(imposterDispTex, uvc).r * imposterMaxDisp;
        float cov = texture(imposterColorTex, uvc).a;
        float diff = pen - d;                          // >0 once we pass the surface
        if (cov > 0.5 && diff >= 0.0) {
            // Binary refine between prevS and s.
            float lo = have_prev ? prevS : 0.0, hi = s;
            for (int b = 0; b < BIN; ++b) {
                float mid = 0.5*(lo+hi);
                vec2 um = entryUV + duvn.xy*mid;
                float dm = texture(imposterDispTex, um).r * imposterMaxDisp;
                if (inward*mid - dm >= 0.0) hi = mid; else lo = mid;
            }
            hitUV = entryUV + duvn.xy*hi;
            return texture(imposterColorTex, hitUV).a > 0.5;
        }
        prevS = s; prevDiff = diff; have_prev = true;
    }
    return false; // reached maxDisp without a covered crossing -> pass through
}
```

- [ ] **Step 4: Branch imposter hits in the closest-hit decode (common.glsl)**

Inside `intersectScene`, at the end of the `if (result.hit)` block (after `result.instanceId = int(instIdx);`, line ~689), add:

```glsl
        result.isImposter = inst.isImposter;
        result.bakedColor = vec3(0.0);
        if (inst.isImposter) {
            int localTri = int(triIdx) - imposterTriBase;
            vec2 uv0, uv1, uv2; imposterTriUVs(localTri, uv0, uv1, uv2);
            // Cage verts in world space.
            vec3 w0 = transformPosition(tri.v0, inst.transform);
            vec3 w1 = transformPosition(tri.v1, inst.transform);
            vec3 w2 = transformPosition(tri.v2, inst.transform);
            vec3 cageN = normalize(result.normal);
            vec2 hitUV;
            if (reliefMarch(result.position, normalize(rayDir), w0, w1, w2, uv0, uv1, uv2, cageN, hitUV)) {
                result.bakedColor = texture(imposterColorTex, hitUV).rgb;
            } else {
                result.hit = false;          // coverage miss: ray passes through
                result.material = -1;
                result.instanceId = -1;
            }
        }
```

Also initialize the new fields in the `else` (miss) branch:

```glsl
        result.isImposter = false;
        result.bakedColor = vec3(0.0);
```

(Use the existing `transformPosition(...)` helper; confirm its name in this file and match it.)

- [ ] **Step 5: Use baked color in `trace()` (raytrace_tlas_blas.fs)**

In `trace()`, right after the hit is fetched (`HitResult hit = intersectScene(rayPos, rayDir);` at line 366) and the `if (!hit.hit)` sky block, add an imposter short-circuit before the material/PBR work:

```glsl
        if (hit.isImposter) {
            color += attenuation * hit.bakedColor;   // baked radiance, no live PBR
            break;
        }
```

Do the same in the reflection/GI secondary-hit handling if those paths read `intersectScene` results directly (search for other `intersectScene(` calls in this file and, where the result is shaded, add the same `if (reflectionHit.isImposter) { ... use bakedColor ... }` guard). For v1 it is acceptable for GI/reflection bounces to read `hit.bakedColor` as incoming radiance.

- [ ] **Step 6: Build the GUI target**

Run: `cd MatterSurfaceLib && make`
Expected: compiles; `raytrace_tlas_blas_processed.fs` regenerates with the new code.

- [ ] **Step 7: Commit**

```bash
git add MatterSurfaceLib/include/tlas_manager.hpp MatterSurfaceLib/src/tlas_manager.cpp MatterSurfaceLib/shaders/bvh_tlas_common.glsl MatterSurfaceLib/shaders/raytrace_tlas_blas.fs
git commit -m "feat: imposter relief-march + baked-color read in traversal shader"
```

---

## Task 8: App-mode wiring, atlas upload, side-by-side demo + capture verification

Ties the pieces together: load-or-bake the imposter, register its cage BLAS, upload the atlas textures, bind the imposter uniforms, and place one imposter instance beside the real part so a capture compares them. This is the honest end-to-end test of the bake (spec Scope #2).

**Not unit-testable**; verified via `MSL_CAPTURE`.

**Files:**
- Modify: `MatterSurfaceLib/src/imposter_bake.cpp` (flatten + cage->Tri helpers, declared in `imposter_asset.h`)
- Modify: `MatterSurfaceLib/main.cpp` (load-or-bake flow, upload, uniforms, instance placement)

- [ ] **Step 1: Add flatten + cage->Tri helpers**

Add to `imposter_asset.h`:

```cpp
// Flatten all TLAS instances' triangles into one part-space triangle list (the
// geometry source for the cage + displacement cast). GL-free.
std::vector<Tri> flatten_part_triangles(const class BLASManager& blas,
                                        const class TLASManager& tlas);

// Convert the cage (verts/tris) into a Tri list suitable for register_triangles.
// GL-free.
std::vector<Tri> cage_to_tris(const ImposterAsset& a);
```

Implement in `imposter_bake.cpp` (these are GL-free but live here to keep the asset core dependency-light; alternatively place in `imposter_asset.cpp` — either is fine, keep them in one place):

```cpp
std::vector<Tri> flatten_part_triangles(const BLASManager& blas, const TLASManager& tlas) {
    std::vector<Tri> out;
    const auto& recs = tlas.get_draw_records();
    for (const auto& r : recs) {
        const BLASManager::BLASEntry* e = blas.get_entry(r.blas_handle);
        if (!e) continue;
        const float* m = r.transform.m; // row-major 4x4
        auto xf = [&](float3 p){
            return make_float3(
                m[0]*p.x + m[1]*p.y + m[2]*p.z + m[3],
                m[4]*p.x + m[5]*p.y + m[6]*p.z + m[7],
                m[8]*p.x + m[9]*p.y + m[10]*p.z + m[11]);
        };
        for (const Tri& t : e->triangles) {
            Tri w;
            w.vertex0 = xf(t.vertex0); w.vertex1 = xf(t.vertex1); w.vertex2 = xf(t.vertex2);
            w.centroid = make_float3((w.vertex0.x+w.vertex1.x+w.vertex2.x)/3.0f,
                                     (w.vertex0.y+w.vertex1.y+w.vertex2.y)/3.0f,
                                     (w.vertex0.z+w.vertex1.z+w.vertex2.z)/3.0f);
            out.push_back(w);
        }
    }
    return out;
}

std::vector<Tri> cage_to_tris(const ImposterAsset& a) {
    std::vector<Tri> out; out.reserve(a.tris.size());
    for (const auto& ct : a.tris) {
        const CageVert& A=a.verts[ct.i0]; const CageVert& B=a.verts[ct.i1]; const CageVert& C=a.verts[ct.i2];
        Tri t;
        t.vertex0=make_float3(A.px,A.py,A.pz);
        t.vertex1=make_float3(B.px,B.py,B.pz);
        t.vertex2=make_float3(C.px,C.py,C.pz);
        t.centroid=make_float3((A.px+B.px+C.px)/3.0f,(A.py+B.py+C.py)/3.0f,(A.pz+B.pz+C.pz)/3.0f);
        out.push_back(t);
    }
    return out;
}
```

Add `#include "../include/blas_manager.hpp"` / `tlas_manager.hpp` already present in `imposter_bake.cpp`.

- [ ] **Step 2: Load-or-bake flow + atlas upload in main.cpp**

Add a method on the app (near `setup_lattice_scene` / the part cache block) that, when `getenv("MSL_SHOW_IMPOSTER")` is set, runs after the part scene is built:

```cpp
// Build ImpGenParams for the current part.
imposter_asset::ImpGenParams ip{};
ip.cageRatio = 0.08f; ip.atlasW = 1024; ip.atlasH = 1024;
ip.inflation = 0.15f; ip.dispBits = 16; ip.seed = 1u;
uint64_t source_hash = part_asset::compute_param_hash(brick_gen_params());
uint64_t imp_hash = imposter_asset::compute_imp_hash(ip);
std::string imp_path = imposter_asset::cache_path(imp_hash);

imposter_asset::ImposterAsset imp;
if (!imposter_asset::load(imp_path, imp_hash, source_hash, imp)) {
    std::vector<Tri> part_tris = imposter_asset::flatten_part_triangles(blas_manager_, tlas_manager_);
    if (imposter_asset::bake_imposter(ip, part_tris, source_hash, blas_manager_, tlas_manager_, imp)) {
        imposter_asset::save(imp_path, imp, imp_hash);
        printf("[imposter] baked + saved %s\n", imp_path.c_str());
    } else {
        printf("[imposter] bake FAILED\n");
    }
} else {
    printf("[imposter] loaded %s\n", imp_path.c_str());
}

// Register the cage BLAS and place one imposter instance beside the real part.
std::vector<Tri> cage_tris = imposter_asset::cage_to_tris(imp);
imposter_cage_blas_ = blas_manager_.register_triangles(cage_tris.data(), (int)cage_tris.size(), nullptr);
{
    TLASManager::DrawInstance di;
    di.blas_handle = imposter_cage_blas_;
    di.material_id = 0;
    di.is_imposter = true;
    di.transform = Matrix4x4();
    di.transform.m[3] = 24.0f; // +X offset beside the real part
    std::vector<TLASManager::DrawInstance> one{di};
    tlas_manager_.draw_batch(one);
    tlas_manager_.build(blas_manager_);
}

// Upload atlas textures (color RGBA8, displacement as R32 float).
{
    Image cimg{}; cimg.data=(void*)imp.color.data(); cimg.width=(int)imp.atlas_w; cimg.height=(int)imp.atlas_h;
    cimg.mipmaps=1; cimg.format=PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    imposter_color_tex_ = LoadTextureFromImage(cimg);
    SetTextureFilter(imposter_color_tex_, TEXTURE_FILTER_BILINEAR);

    std::vector<float> df((size_t)imp.atlas_w*imp.atlas_h);
    if (imp.disp_bits==16) for (size_t i=0;i<df.size();++i){ uint16_t v; memcpy(&v,&imp.disp[i*2],2); df[i]=v/65535.0f; }
    else                   for (size_t i=0;i<df.size();++i) df[i]=imp.disp[i]/255.0f;
    Image dimg{}; dimg.data=df.data(); dimg.width=(int)imp.atlas_w; dimg.height=(int)imp.atlas_h;
    dimg.mipmaps=1; dimg.format=PIXELFORMAT_UNCOMPRESSED_R32;
    imposter_disp_tex_ = LoadTextureFromImage(dimg);
    SetTextureFilter(imposter_disp_tex_, TEXTURE_FILTER_BILINEAR);

    imposter_grid_ = (int)ceilf(sqrtf((float)imp.tris.size()));
    imposter_tri_base_ = blas_manager_.get_offsets(imposter_cage_blas_).triangle_offset;
    imposter_max_disp_ = imp.max_disp;
    imposter_atlas_w_ = (float)imp.atlas_w; imposter_atlas_h_ = (float)imp.atlas_h;
    imposter_enabled_ = true;
}
```

Add the member fields to the app class: `BLASHandle imposter_cage_blas_ = 0; Texture2D imposter_color_tex_{}; Texture2D imposter_disp_tex_{}; int imposter_grid_=0, imposter_tri_base_=0; float imposter_max_disp_=0, imposter_atlas_w_=0, imposter_atlas_h_=0; bool imposter_enabled_=false;`. Add `#include "include/imposter_asset.h"` to main.cpp's includes.

- [ ] **Step 3: Bind imposter uniforms each frame**

Where the raytracing shader uniforms are set before the fullscreen draw (search for existing `SetShaderValue(raytracing_shader_, ...)` for `giStrength`/`shadowStrength`), add:

```cpp
if (imposter_enabled_) {
    SetShaderValueTexture(raytracing_shader_, GetShaderLocation(raytracing_shader_, "imposterColorTex"), imposter_color_tex_);
    SetShaderValueTexture(raytracing_shader_, GetShaderLocation(raytracing_shader_, "imposterDispTex"),  imposter_disp_tex_);
    SetShaderValue(raytracing_shader_, GetShaderLocation(raytracing_shader_, "imposterGrid"), &imposter_grid_, SHADER_UNIFORM_INT);
    SetShaderValue(raytracing_shader_, GetShaderLocation(raytracing_shader_, "imposterTriBase"), &imposter_tri_base_, SHADER_UNIFORM_INT);
    SetShaderValue(raytracing_shader_, GetShaderLocation(raytracing_shader_, "imposterMaxDisp"), &imposter_max_disp_, SHADER_UNIFORM_FLOAT);
    float as[2]={imposter_atlas_w_, imposter_atlas_h_};
    SetShaderValue(raytracing_shader_, GetShaderLocation(raytracing_shader_, "imposterAtlasSize"), as, SHADER_UNIFORM_VEC2);
    float pad=2.0f; SetShaderValue(raytracing_shader_, GetShaderLocation(raytracing_shader_, "imposterPad"), &pad, SHADER_UNIFORM_FLOAT);
}
```

- [ ] **Step 4: Build the GUI target**

Run: `cd MatterSurfaceLib && make`
Expected: compiles and links `gpu_raytrace.exe`.

- [ ] **Step 5: Manual capture verification** (user runs the GUI per memory — harness reaps backgrounded GUI children)

Ask the user to run, from their own terminal / via `!`:

```bash
cd MatterSurfaceLib && MSL_SHOW_IMPOSTER=1 MSL_CAPTURE=imposter_cmp.png MSL_FRAMES=24 ./gpu_raytrace.exe
```

Inspect `imposter_cmp.png`: the real part (origin) and the imposter (+24 X) should look close from the capture camera. Also confirm console prints `[imposter] baked + saved imposters/<hash>.imp` on first run and `[imposter] loaded ...` on the second run (cache hit). Verify the imposter casts a shadow (cage-granular in v1) and is not see-through where the part is solid (coverage), and passes through where the cage is empty.

- [ ] **Step 6: Commit**

```bash
git add MatterSurfaceLib/src/imposter_bake.cpp MatterSurfaceLib/include/imposter_asset.h MatterSurfaceLib/main.cpp
git commit -m "feat: imposter load-or-bake app-mode, atlas upload, side-by-side demo"
```

---

## Self-Review (completed during planning)

- **Spec coverage:** §1 code layout → Tasks 1/6/7/8 (all files created). §2 format + validation → Task 2 (magic, version, sizeof_CageVert/Tri, content_hash, imp_hash, source_part_hash all checked). §3 cache key (ImpGenParams hash ^ version + source-part binding) → Tasks 1-2. §4 bake pipeline: assemble/flatten → Task 8 Step 1; cage → Task 3; atlas → Task 3; GPU fill → Task 6; CPU post/dilate → Task 5; (displacement moved to CPU per spec §6 allowance — noted in header). §5 runtime relief-march (registration, march, baked-color closest-hit, coverage miss pass-through) → Task 7; any-hit shadow scoped to cage-granular v1 (documented). §6 testing (cage enclosure, displacement reconstruction, format round-trip+guards, source-hash link → Tasks 2-5 CPU; GPU/visual via capture → Tasks 6/8).
- **Type consistency:** `ImpGenParams`/`CageVert`/`CageTri`/`ImposterAsset` fields are identical across all tasks; `build_cage` / `bake_displacement_cpu` / `dilate_atlas` / `bake_imposter` / `flatten_part_triangles` / `cage_to_tris` signatures match between header and use; shader uniform names (`imposterColorTex`, `imposterDispTex`, `imposterGrid`, `imposterTriBase`, `imposterMaxDisp`, `imposterAtlasSize`, `imposterPad`) match between common.glsl declarations (Task 7 Step 2) and the C++ binding (Task 8 Step 3); the deterministic UV formula in `imposterTriUVs` (GLSL) mirrors `build_cage` (C++) corner math.
- **Known v1 simplifications (intentional, documented in-task):** displacement baked CPU not in the MRT pass; single bound imposter atlas + global params (one imposter in the demo); shadow/GI any-hit at cage granularity. All are spec-sanctioned or explicitly deferred to #3.
- **Verify-while-implementing notes:** confirm the exact helper names `transformPosition`/`transformNormal` in `bvh_tlas_common.glsl` (used in Task 7 Step 4); confirm the main Makefile source-list variable name when adding `imposter_asset.cpp`/`imposter_bake.cpp` (Task 6 Step 3); confirm the per-frame uniform-set site in main.cpp (Task 8 Step 3).

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-06-20-imposter-generation.md`. Two execution options:

1. **Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review (spec compliance then code quality) between tasks, fast iteration. The GL-free Tasks 1-5 are fully TDD'd; the GPU Tasks 6-8 are implementation + manual capture checks.
2. **Inline Execution** — I execute tasks in this session with checkpoints for your review.

Which approach?
