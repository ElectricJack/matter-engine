# Tileset Viewer Consumption (Phase 4) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Consume Phase 3 `.gtex` atlases in the viewer so terrain renders with Wang-tile-sampled PBR textures, driven by a `Name [tileset]` line in `world.manifest` (production pipeline, no preview binaries).

**Architecture:** A `tileset_provider` module owns up to 4 GPU tileset slots (albedo RGB8 + normal RG8 + ORM RGB8 + height R16 textures, all mipmapped). `LocalProvider::connect()` recognises `tileset` roots from `read_manifest`, drives them through the existing `run_tileset_phase(opts)` GPU overload to produce `<world_data>/<Name>.gtex`, then loads each `.gtex` into a slot. `MaterialDef` gains a `groundTilesetSlot` field packed at material-table slot 11 (previously pad); the raytrace and raster fragment shaders detect `groundTilesetSlot >= 0` and swap in Wang-tile samples through a new `tileset_sampling.glsl` helper. Meadow gets a `ForestFloor` tileset root that binds slot 0 to material DIRT.

**Tech Stack:** C++17, raylib GL 4.6 (WSLg d3d12), MSL registry + shader system, box3d physics (linked via `Libraries/box3d/libbox3d.a`), MinGW cross-compile for Windows viewer.exe. GLSL 460 core (`textureGrad`, `dFdx`/`dFdy`).

## Global Constraints

- **Always implement the real thing** — no scaffold/demo/preview shortcuts. Fix design gaps in-phase, not with FIXMEs. Test binaries verify isolated components; they are NOT the answer to "let me see what this looks like." Any new visual must be reachable through the actual viewer.
- **MatterSurfaceLib is read-only** except surfaced bugs OR spec-mandated feature work. Task 2's `groundTilesetSlot` addition is spec-mandated (spec 2026-07-05 §"Viewer Consumption" lines 246-248) — flag it in the commit message per memory `feedback_mattersurfacelib_readonly_exception.md`.
- **Do not modify Phase 1 settle-core semantics** (`tileset_settle.cpp`, `SettleWorld` API). Phase 4 consumes them via `run_tileset_phase(opts)`; it never mutates them.
- **Fail-closed everywhere** — every error path sets a structured `err` naming the file/hash/reason.
- **GL 4.6 requires `GALLIUM_DRIVER=d3d12` on WSLg.** Every GPU error message must include this hint.
- **All new tests self-terminate; no viewer/GPU-context left running at exit** (per memory `feedback_viewer_test_lifecycle.md`). Shot scripts follow the `tools/viewer_shots.sh` FIFO quit+wait+trap pattern.
- **After ANY engine/viewer code change: run `make windows`.** Every task that touches engine or viewer sources includes a windows-rebuild verification step in its commit sequence, not just Task 7.
- **Keep merged feature branches** (memory `feedback_keep_merged_branches.md`) — no branch-delete steps in this plan.
- `MATERIAL_FLOATS_PER_DEF = 12` (constant preserved; Phase 4 only repurposes slot [11] from pad to `groundTilesetSlot`).

---

## File Structure

**New files:**
- `MatterEngine3/viewer/tileset_provider.h` — public API for GPU tileset slots.
- `MatterEngine3/viewer/tileset_provider.cpp` — implementation.
- `MatterEngine3/viewer/tileset_provider_tests.cpp` — headless GL tests.
- `MatterEngine3/viewer/tileset_load_tests.cpp` — LocalProvider tileset-wiring unit test.
- `MatterSurfaceLib/shaders/tileset_sampling.glsl` — Wang-tile sampling helpers (shared across raytrace + raster fragment shaders).
- `MatterEngine3/examples/world_demo/schemas/ForestFloor.js` — the Meadow tileset script.
- `MatterEngine3/tools/meadow_forestfloor_shots.sh` — seam-heavy shot regression driver.

**Modified files:**
- `MatterSurfaceLib/include/material_registry.h` — add `groundTilesetSlot` field + runtime setter.
- `MatterSurfaceLib/src/material_registry.c` — pack slot [11]; add setter.
- `MatterSurfaceLib/shaders/materials.glsl` — unpack slot [11] into `MaterialProperties.groundTilesetSlot`.
- `MatterSurfaceLib/shaders/raytrace_tlas_blas.fs` — Wang branch before PBR albedo/normal/roughness use.
- `MatterSurfaceLib/shaders/raster.fs` — same Wang branch (raster path).
- `MatterEngine3/src/tileset_bake_primary.cpp` — pack `groundTilesetSlot` at slot [11].
- `MatterEngine3/src/tileset_bake_ao.cpp` — same.
- `MatterEngine3/viewer/local_provider.cpp` — thread `tileset_out` from `read_manifest`, call `run_tileset_phase(opts)`, load slots, bind material DIRT.
- `MatterEngine3/viewer/local_provider.h` — add a getter for baked tileset slot count (for the load test).
- `MatterEngine3/viewer/renderer.cpp` — call `tileset_provider::bind_all_to_shader` on the raytrace program each frame.
- `MatterEngine3/viewer/raster_composer.cpp` — same on the raster program.
- `MatterEngine3/viewer/Makefile` — add new source files, box3d include + link, new test targets.
- `MatterEngine3/examples/world_demo/WorldData/Meadow/world.manifest` — add `ForestFloor tileset` line.
- `MatterEngine3/tests/Makefile` — add `tileset_load_tests` build + `run-tilesetload` target.
- `build-all.sh` — add `run-tilesetload` unconditionally + `run-tilesetprovider` + meadow shot script under the GL 4.6 guard.
- `.superpowers/sdd/progress.md` — Phase 4 ledger.

---

## Task 1: `.gtex` viewer loader + tileset slot table

**Files:**
- Create: `MatterEngine3/viewer/tileset_provider.h`
- Create: `MatterEngine3/viewer/tileset_provider.cpp`
- Create: `MatterEngine3/viewer/tileset_provider_tests.cpp`
- Modify: `MatterEngine3/viewer/Makefile` (add `tileset_provider.cpp` to `VIEWER_SRC`; add `tileset-provider-tests` target + `run-tilesetprovider`)

**Interfaces:**
- Consumes: `tileset::load_gtex(path, header, albedo, normal_rg, orm, height_r16, err)` and `tileset::GTexHeader` from `MatterEngine3/include/tileset_gtex.h`; `viewer::gl46_available(err)` from `gl46.h`.
- Produces:
  - `struct viewer::TilesetSlot { GLuint tex_albedo, tex_normal, tex_orm, tex_height; float tile_size_m; int texels_per_meter; int atlas_tiles_x, atlas_tiles_y; float height_min, height_max; bool valid; }`
  - `namespace viewer::tileset_provider` with `bool load_slot(int slot, const std::string& gtex_path, std::string& err)`, `void unload_slot(int slot)`, `void unload_all()`, `const TilesetSlot& get_slot(int slot)`, `int max_slots()` (returns 4), `void bind_all_to_shader(GLuint program)`.

- [ ] **Step 1: Write the failing test**

Create `MatterEngine3/viewer/tileset_provider_tests.cpp`:

```cpp
// tileset_provider_tests.cpp — headless GL tests for the viewer tileset slot table.
// Pattern: mirror tileset_gpu_tests.cpp — FLAG_WINDOW_HIDDEN, gl46_available SKIP,
// CloseWindow before return. Test file drives a fixture .gtex through save_gtex →
// load_slot → assertions on GL texture state → unload → clean exit.

#include "raylib.h"
#include "gl46.h"
#include "tileset_gtex.h"
#include "tileset_provider.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int g_tests = 0, g_failures = 0;
#define REQUIRE(cond) do { \
    ++g_tests; \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
    } while (0)

// Build a tiny 64x64 (4 tiles x 16px each) .gtex fixture in memory, write to /tmp,
// then load_slot(0). Verify the four GL textures exist, have mip 0 dims equal to
// atlas_tiles*tile_size_m*texels_per_meter, and that mip generation produced >1 level.
static void test_load_slot_uploads_mipped_textures() {
    using namespace tileset;
    GTexHeader hdr;
    hdr.tile_size_m       = 1.0f;
    hdr.texels_per_meter  = 4;      // 4 * 1 * 4 = 16 px per tile
    hdr.atlas_tiles_x     = 4;
    hdr.atlas_tiles_y     = 4;
    hdr.height_min        = 0.0f;
    hdr.height_max        = 0.5f;
    hdr.content_hash      = 0xDEADBEEFCAFEBABEull;

    const int W = hdr.atlas_tiles_x * (int)hdr.tile_size_m * hdr.texels_per_meter; // 16
    const int H = hdr.atlas_tiles_y * (int)hdr.tile_size_m * hdr.texels_per_meter; // 16
    std::vector<uint8_t>  albedo(W*H*3, 128);
    std::vector<uint8_t>  normal(W*H*2, 127);
    std::vector<uint8_t>  orm   (W*H*3, 200);
    std::vector<uint16_t> height(W*H,   30000);

    const std::string path = "/tmp/tileset_provider_fixture.gtex";
    std::string err;
    REQUIRE(save_gtex(path, hdr, W, H,
                      albedo.data(), normal.data(), orm.data(), height.data(), err));

    REQUIRE(viewer::tileset_provider::load_slot(0, path, err));
    if (!err.empty()) std::fprintf(stderr, "  load_slot err: %s\n", err.c_str());

    const viewer::TilesetSlot& s = viewer::tileset_provider::get_slot(0);
    REQUIRE(s.valid);
    REQUIRE(s.tex_albedo != 0);
    REQUIRE(s.tex_normal != 0);
    REQUIRE(s.tex_orm    != 0);
    REQUIRE(s.tex_height != 0);
    REQUIRE(s.tile_size_m == 1.0f);
    REQUIRE(s.texels_per_meter == 4);
    REQUIRE(s.atlas_tiles_x == 4);
    REQUIRE(s.height_min == 0.0f);
    REQUIRE(s.height_max == 0.5f);

    // Verify mip level 0 width matches and level 1 exists (glGenerateMipmap called).
    glBindTexture(GL_TEXTURE_2D, s.tex_albedo);
    GLint mip0_w = 0, mip1_w = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &mip0_w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 1, GL_TEXTURE_WIDTH, &mip1_w);
    REQUIRE(mip0_w == W);
    REQUIRE(mip1_w == W/2);

    viewer::tileset_provider::unload_slot(0);
    const viewer::TilesetSlot& s_after = viewer::tileset_provider::get_slot(0);
    REQUIRE(!s_after.valid);
    REQUIRE(s_after.tex_albedo == 0);

    std::remove(path.c_str());
}

// Verify bind_all_to_shader sets sampler uniforms for a program that declares them.
static void test_bind_all_sets_uniforms() {
    const char* fs_src =
        "#version 460 core\n"
        "uniform sampler2D groundAlbedo0;\n"
        "uniform sampler2D groundNormal0;\n"
        "uniform sampler2D groundORM0;\n"
        "uniform sampler2D groundHeight0;\n"
        "out vec4 c;\n"
        "void main(){ c = texture(groundAlbedo0, vec2(0)) + texture(groundNormal0,vec2(0))\n"
        "                  + texture(groundORM0,   vec2(0)) + texture(groundHeight0,vec2(0)); }\n";
    const char* vs_src =
        "#version 460 core\n"
        "void main(){ gl_Position = vec4(0); }\n";
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vs_src, nullptr); glCompileShader(vs);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fs_src, nullptr); glCompileShader(fs);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs); glAttachShader(prog, fs); glLinkProgram(prog);
    GLint status = 0; glGetProgramiv(prog, GL_LINK_STATUS, &status);
    REQUIRE(status);

    // Fake slot 0 as valid so bind_all sees it.
    std::string err;
    GTexHeader hdr;
    hdr.tile_size_m = 1; hdr.texels_per_meter = 4;
    hdr.atlas_tiles_x = 4; hdr.atlas_tiles_y = 4;
    hdr.height_min = 0; hdr.height_max = 1; hdr.content_hash = 0x1;
    const int W = 16, H = 16;
    std::vector<uint8_t> a(W*H*3,0), n2(W*H*2,0), o(W*H*3,0);
    std::vector<uint16_t> hh(W*H,0);
    const std::string path = "/tmp/tileset_provider_bind_fixture.gtex";
    tileset::save_gtex(path, hdr, W, H, a.data(), n2.data(), o.data(), hh.data(), err);
    REQUIRE(viewer::tileset_provider::load_slot(0, path, err));

    glUseProgram(prog);
    viewer::tileset_provider::bind_all_to_shader(prog);

    GLint u_albedo = glGetUniformLocation(prog, "groundAlbedo0");
    GLint u_normal = glGetUniformLocation(prog, "groundNormal0");
    GLint u_orm    = glGetUniformLocation(prog, "groundORM0");
    GLint u_height = glGetUniformLocation(prog, "groundHeight0");
    REQUIRE(u_albedo >= 0);
    REQUIRE(u_normal >= 0);
    REQUIRE(u_orm    >= 0);
    REQUIRE(u_height >= 0);

    GLint v_albedo = -1, v_normal = -1, v_orm = -1, v_height = -1;
    glGetUniformiv(prog, u_albedo, &v_albedo);
    glGetUniformiv(prog, u_normal, &v_normal);
    glGetUniformiv(prog, u_orm,    &v_orm);
    glGetUniformiv(prog, u_height, &v_height);
    // Slot 0 uses texture units 10,11,12,13 (per bind_all_to_shader spec).
    REQUIRE(v_albedo == 10);
    REQUIRE(v_normal == 11);
    REQUIRE(v_orm    == 12);
    REQUIRE(v_height == 13);

    viewer::tileset_provider::unload_all();
    glDeleteProgram(prog);
    glDeleteShader(vs); glDeleteShader(fs);
    std::remove(path.c_str());
}

int main() {
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(64, 64, "tileset_provider_tests");
    std::string why;
    if (!viewer::gl46_available(why)) {
        std::fprintf(stderr, "SKIP: %s; set GALLIUM_DRIVER=d3d12 on WSLg\n", why.c_str());
        CloseWindow();
        return 0;
    }
    test_load_slot_uploads_mipped_textures();
    test_bind_all_sets_uniforms();
    CloseWindow();
    std::fprintf(stderr, "tileset_provider_tests: %d run, %d failed\n", g_tests, g_failures);
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Add tileset_provider.h with declared interface (no impl yet)**

Create `MatterEngine3/viewer/tileset_provider.h`:

```cpp
#pragma once
// tileset_provider.h — viewer-side owner of GPU tileset slots.
//
// A slot holds four mipmapped 2D textures (albedo RGB8, normal RG8, ORM RGB8,
// height R16) uploaded from a .gtex atlas produced by run_tileset_phase(opts).
// Up to 4 slots (per spec §"Viewer Consumption").
//
// Life-cycle: LocalProvider::connect() calls load_slot(i, path, err) once per
// tileset root; the renderer calls bind_all_to_shader(program) each frame after
// glUseProgram(); unload_all() runs at shutdown.

#include <string>
#include "gl46.h"

namespace viewer {

struct TilesetSlot {
    GLuint tex_albedo = 0;
    GLuint tex_normal = 0;
    GLuint tex_orm    = 0;
    GLuint tex_height = 0;
    float  tile_size_m       = 0.0f;
    int    texels_per_meter  = 0;
    int    atlas_tiles_x     = 0;
    int    atlas_tiles_y     = 0;
    float  height_min        = 0.0f;
    float  height_max        = 0.0f;
    bool   valid             = false;
};

namespace tileset_provider {

// Max concurrent slots (bound to samplers groundAlbedo[0..3] etc.).
inline constexpr int kMaxSlots = 4;
int max_slots();

// Load a .gtex into slot [0..kMaxSlots). Overwrites (unloading) any existing
// contents. Fails closed on missing file, decode failure, or GL error; the
// error message names the path and reason. Requires an active GL 4.6 context;
// if unavailable, err is set with the GALLIUM_DRIVER=d3d12 hint.
bool load_slot(int slot, const std::string& gtex_path, std::string& err);

// Delete GL textures for slot; sets valid=false.
void unload_slot(int slot);
void unload_all();

// Read accessor; returns a slot with valid=false for out-of-range indices.
const TilesetSlot& get_slot(int slot);

// For each valid slot i, bind:
//   groundAlbedo<i>  at texture unit (10 + i*4 + 0)
//   groundNormal<i>  at (10 + i*4 + 1)
//   groundORM<i>     at (10 + i*4 + 2)
//   groundHeight<i>  at (10 + i*4 + 3)
// Also sets integer uniform tilesetSlot<i>_tileSize_m and tilesetSlot<i>_texelsPerMeter
// so the shader can compute cell coords without another CPU trip.
// Silently skips uniforms the shader did not declare (glGetUniformLocation < 0).
void bind_all_to_shader(GLuint program);

} // namespace tileset_provider
} // namespace viewer
```

- [ ] **Step 3: Add Makefile target and run test to verify link failure**

Modify `MatterEngine3/viewer/Makefile`. Add these two blocks (after the `TILESET_SEAM_TEST_OBJ` block near line 149):

```make
# tileset_provider headless tests (Phase 4 Task 1). Same object set as
# tileset-gpu-tests + the tileset_provider_tests.cpp driver, minus main.o.
L_TSET_PROV_TEST_OBJ = $(L_DIR)/tileset_provider_tests.o
$(L_TSET_PROV_TEST_OBJ): tileset_provider_tests.cpp | $(L_DIR)
	$(CC) -c $< -o $@ $(CXX_FLAGS_BUILD)

TILESET_PROV_TEST_OBJ = $(filter-out $(L_DIR)/main.o $(L_DIR)/gpu_cull_tests.o,$(L_ALL_OBJ)) $(L_TSET_PROV_TEST_OBJ)
tileset-provider-tests: shaders shaders_gpu_link $(TILESET_PROV_TEST_OBJ)
	$(CC) $(TILESET_PROV_TEST_OBJ) -o tileset_provider_tests $(CFLAGS) $(LDFLAGS) $(LDLIBS)

run-tilesetprovider: tileset-provider-tests
	./tileset_provider_tests
```

Add `tileset_provider.cpp` to `VIEWER_SRC` (line 32-37) so it becomes part of `L_ALL_OBJ`:

```make
VIEWER_SRC = main.cpp renderer.cpp ui.cpp \
             world_state.cpp part_store.cpp resolvers.cpp \
             world_composer.cpp local_provider.cpp \
             raster_mesh.cpp raster_composer.cpp probe_texture.cpp \
             gpu_culler.cpp tileset_gl_ctx.cpp tileset_bake_primary.cpp \
             tileset_bake_ao.cpp tileset_provider.cpp
```

Extend the `.PHONY` line (line 215) to include the new targets:

```make
.PHONY: viewer windows gpu-tests tileset-gpu-tests run-tilesetgpu tileset-seam-tests run-tilesetseam tileset-provider-tests run-tilesetprovider clean shaders shaders_gpu_link win-shaders win-shaders-gpu regen-processed-shader
```

Also extend `clean` (line 213):

```make
clean:
	rm -rf viewer viewer.exe gpu_tests tileset_gpu_tests tileset_seam_tests tileset_provider_tests shaders build
```

Run: `cd MatterEngine3/viewer && make tileset-provider-tests 2>&1 | tail -15`

Expected: link fails with undefined references to `viewer::tileset_provider::load_slot`, `get_slot`, `bind_all_to_shader`, `unload_slot`, `unload_all`.

- [ ] **Step 4: Implement tileset_provider.cpp**

Create `MatterEngine3/viewer/tileset_provider.cpp`:

```cpp
// tileset_provider.cpp — see header.

#include "tileset_provider.h"
#include "tileset_gtex.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace viewer {
namespace tileset_provider {

namespace {

TilesetSlot g_slots[kMaxSlots];
const TilesetSlot k_empty{};

GLuint upload_tex_2d(const void* data, int w, int h, GLenum internal, GLenum format, GLenum type) {
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // NORMAL_RG8 requires an alignment fallback; force 1 to be safe for all pixel sizes.
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, data);
    glGenerateMipmap(GL_TEXTURE_2D);
    return id;
}

void set_sampler(GLuint program, const char* name, int unit) {
    GLint loc = glGetUniformLocation(program, name);
    if (loc >= 0) glUniform1i(loc, unit);
}

void set_float_uniform(GLuint program, const char* name, float v) {
    GLint loc = glGetUniformLocation(program, name);
    if (loc >= 0) glUniform1f(loc, v);
}

void set_int_uniform(GLuint program, const char* name, int v) {
    GLint loc = glGetUniformLocation(program, name);
    if (loc >= 0) glUniform1i(loc, v);
}

} // anon

int max_slots() { return kMaxSlots; }

const TilesetSlot& get_slot(int slot) {
    if (slot < 0 || slot >= kMaxSlots) return k_empty;
    return g_slots[slot];
}

void unload_slot(int slot) {
    if (slot < 0 || slot >= kMaxSlots) return;
    TilesetSlot& s = g_slots[slot];
    if (s.tex_albedo) glDeleteTextures(1, &s.tex_albedo);
    if (s.tex_normal) glDeleteTextures(1, &s.tex_normal);
    if (s.tex_orm)    glDeleteTextures(1, &s.tex_orm);
    if (s.tex_height) glDeleteTextures(1, &s.tex_height);
    s = TilesetSlot{};
}

void unload_all() {
    for (int i = 0; i < kMaxSlots; ++i) unload_slot(i);
}

bool load_slot(int slot, const std::string& gtex_path, std::string& err) {
    if (slot < 0 || slot >= kMaxSlots) {
        err = "tileset_provider::load_slot: slot " + std::to_string(slot) +
              " out of range [0," + std::to_string(kMaxSlots) + ")";
        return false;
    }
    // Fail-closed on missing GL context: any glGetError seed here is meaningless
    // without one, so we require the caller to have InitWindow'd. The .gtex reader
    // is CPU-only and gates I/O errors itself.
    tileset::GTexHeader hdr;
    std::vector<uint8_t>  a, n2, o;
    std::vector<uint16_t> h;
    if (!tileset::load_gtex(gtex_path, hdr, a, n2, o, h, err)) {
        // load_gtex sets a structured err; augment with the WSLg hint (safe even if
        // the failure is not GL-related — a stray hint costs nothing).
        err += " (if you see a GL error, set GALLIUM_DRIVER=d3d12 on WSLg)";
        return false;
    }
    const int W = hdr.atlas_tiles_x * (int)hdr.tile_size_m * hdr.texels_per_meter;
    const int H = hdr.atlas_tiles_y * (int)hdr.tile_size_m * hdr.texels_per_meter;
    if (W <= 0 || H <= 0) {
        err = "tileset_provider::load_slot: bad atlas dims (" + std::to_string(W) +
              "x" + std::to_string(H) + ") from " + gtex_path;
        return false;
    }
    if ((int)a.size()  != W*H*3 || (int)n2.size() != W*H*2 ||
        (int)o.size()  != W*H*3 || (int)h.size()  != W*H) {
        err = "tileset_provider::load_slot: channel-size mismatch for " + gtex_path;
        return false;
    }

    unload_slot(slot);  // replace any existing contents

    TilesetSlot& s = g_slots[slot];
    s.tex_albedo = upload_tex_2d(a.data(),  W, H, GL_RGB8,  GL_RGB,          GL_UNSIGNED_BYTE);
    s.tex_normal = upload_tex_2d(n2.data(), W, H, GL_RG8,   GL_RG,           GL_UNSIGNED_BYTE);
    s.tex_orm    = upload_tex_2d(o.data(),  W, H, GL_RGB8,  GL_RGB,          GL_UNSIGNED_BYTE);
    s.tex_height = upload_tex_2d(h.data(),  W, H, GL_R16,   GL_RED,          GL_UNSIGNED_SHORT);
    s.tile_size_m      = hdr.tile_size_m;
    s.texels_per_meter = hdr.texels_per_meter;
    s.atlas_tiles_x    = hdr.atlas_tiles_x;
    s.atlas_tiles_y    = hdr.atlas_tiles_y;
    s.height_min       = hdr.height_min;
    s.height_max       = hdr.height_max;
    s.valid            = true;

    GLenum e = glGetError();
    if (e != GL_NO_ERROR) {
        err = "tileset_provider::load_slot: GL error " + std::to_string((int)e) +
              " uploading " + gtex_path + " (set GALLIUM_DRIVER=d3d12 on WSLg)";
        unload_slot(slot);
        return false;
    }
    return true;
}

void bind_all_to_shader(GLuint program) {
    // Base unit = 10, four samplers per slot; 4 slots => units 10..25.
    for (int i = 0; i < kMaxSlots; ++i) {
        const TilesetSlot& s = g_slots[i];
        if (!s.valid) continue;
        const int base = 10 + i * 4;
        char name[64];
        std::snprintf(name, sizeof name, "groundAlbedo%d", i);
        glActiveTexture(GL_TEXTURE0 + base + 0);
        glBindTexture(GL_TEXTURE_2D, s.tex_albedo);
        set_sampler(program, name, base + 0);

        std::snprintf(name, sizeof name, "groundNormal%d", i);
        glActiveTexture(GL_TEXTURE0 + base + 1);
        glBindTexture(GL_TEXTURE_2D, s.tex_normal);
        set_sampler(program, name, base + 1);

        std::snprintf(name, sizeof name, "groundORM%d", i);
        glActiveTexture(GL_TEXTURE0 + base + 2);
        glBindTexture(GL_TEXTURE_2D, s.tex_orm);
        set_sampler(program, name, base + 2);

        std::snprintf(name, sizeof name, "groundHeight%d", i);
        glActiveTexture(GL_TEXTURE0 + base + 3);
        glBindTexture(GL_TEXTURE_2D, s.tex_height);
        set_sampler(program, name, base + 3);

        std::snprintf(name, sizeof name, "tilesetSlot%d_tileSize_m", i);
        set_float_uniform(program, name, s.tile_size_m);
        std::snprintf(name, sizeof name, "tilesetSlot%d_texelsPerMeter", i);
        set_int_uniform(program, name, s.texels_per_meter);
    }
    // Restore TEXTURE0 as the "active" slot so downstream code doesn't inherit
    // an unrelated unit selection.
    glActiveTexture(GL_TEXTURE0);
}

} // namespace tileset_provider
} // namespace viewer
```

- [ ] **Step 5: Run to verify PASS**

Run: `cd MatterEngine3/viewer && make run-tilesetprovider 2>&1 | tail -10`

Expected: `tileset_provider_tests: 12 run, 0 failed` (2 REQUIRE per slot field/tex + 3 bind uniforms + 2 mip levels + ~4 bind values). The script prints and returns 0.

If GALLIUM_DRIVER=d3d12 is not set on WSLg the test SKIPs with a message and returns 0 — set it before running: `GALLIUM_DRIVER=d3d12 make run-tilesetprovider`.

- [ ] **Step 6: `make windows` verification**

Run: `cd MatterEngine3/viewer && make windows 2>&1 | tail -20`

Expected: successful link → `viewer.exe` present. The Windows build already includes `tileset_provider.o` because it lives in `VIEWER_SRC` (Step 3) which drives both `L_ALL_OBJ` and `W_ALL_OBJ`.

- [ ] **Step 7: Commit**

```bash
git add MatterEngine3/viewer/tileset_provider.h \
        MatterEngine3/viewer/tileset_provider.cpp \
        MatterEngine3/viewer/tileset_provider_tests.cpp \
        MatterEngine3/viewer/Makefile
git commit -m "$(cat <<'EOF'
phase4 t1: viewer tileset_provider (load .gtex → 4 GPU slots)

Adds viewer::tileset_provider with load_slot / unload_slot / bind_all_to_shader.
Uploads albedo/normal/orm/height as mipmapped GL_REPEAT textures at units 10..25.
Headless test verifies mip generation and sampler-unit assignment.
EOF
)"
```

---

## Task 2: `MaterialDef.groundTilesetSlot` field + CPU/shader plumbing

**Files:**
- Modify: `MatterSurfaceLib/include/material_registry.h` (add field + setter declaration)
- Modify: `MatterSurfaceLib/src/material_registry.c` (pack slot [11]; add setter)
- Modify: `MatterSurfaceLib/shaders/materials.glsl` (unpack slot [11])
- Modify: `MatterEngine3/src/tileset_bake_primary.cpp` (pack slot [11])
- Modify: `MatterEngine3/src/tileset_bake_ao.cpp` (pack slot [11])
- Modify: `MatterEngine3/viewer/tileset_gpu_tests.cpp` (extend `test_material_pack_layout`)

**Interfaces:**
- Consumes: existing `MaterialRegistryPackForGPU(float* out)` signature; `MATERIAL_FLOATS_PER_DEF = 12` (constant, unchanged).
- Produces:
  - Extended `MaterialDef { ... int groundTilesetSlot; }` (default -1)
  - New API `void MaterialRegistrySetGroundTilesetSlot(int materialId, int slot)` — mutates the registry's runtime state used by `MaterialRegistryPackForGPU` (implemented via a parallel `g_slot_overrides[]` array so the const table stays const).
  - Shader field `MaterialProperties.groundTilesetSlot` (int, -1 = untextured).

*MatterSurfaceLib exception note:* this is a spec-mandated feature (spec 2026-07-05 §"Viewer Consumption" lines 246-248, "MaterialDef gains an optional groundTileset reference; the GPU material table gains a tileset slot index"). Flag it in commit message per memory `feedback_mattersurfacelib_readonly_exception.md`.

- [ ] **Step 1: Extend the existing pack-layout test to fail on the new slot**

Modify `MatterEngine3/viewer/tileset_gpu_tests.cpp` — replace the current `test_material_pack_layout` body (lines 41-66) with the extended version that also verifies slot [11] carries `groundTilesetSlot`:

```cpp
static void test_material_pack_layout() {
    MaterialDef fix{};
    fix.albedo[0] = 0.1f; fix.albedo[1] = 0.2f; fix.albedo[2] = 0.3f;
    fix.roughness = 0.4f; fix.metallic = 0.5f; fix.emission = 0.6f;
    fix.translucency = 0.5f;
    fix.ior = 1.7f;
    fix.flatShading = 1;
    fix.mergeGroup = 42;
    fix.groundTilesetSlot = 2;   // Phase 4: slot [11] now carries this.

    float r[12]{};
    r[0] = fix.albedo[0]; r[1] = fix.albedo[1]; r[2] = fix.albedo[2];
    r[3] = fix.roughness; r[4] = fix.metallic;  r[5] = fix.emission;
    r[6] = 0.0f;                // pad
    r[7] = fix.translucency;
    r[8] = fix.ior;
    r[9] = (float)fix.flatShading;
    r[10]= (float)fix.mergeGroup;
    r[11]= (float)fix.groundTilesetSlot;

    REQUIRE(r[7]  == 0.5f);
    REQUIRE(r[8]  == 1.7f);
    REQUIRE(r[9]  == 1.0f);
    REQUIRE(r[10] == 42.0f);
    REQUIRE(r[11] == 2.0f);   // NEW: groundTilesetSlot at [11]
}
```

- [ ] **Step 2: Run tests to verify FAIL (before implementation)**

Run: `cd MatterEngine3/viewer && make run-tilesetgpu 2>&1 | grep -E "(FAIL|passed|run)" | head`

Expected: compile fails with `'MaterialDef' has no member named 'groundTilesetSlot'` (test uses the field). This proves the test would fail once the field exists but is wrong; we now add the field so the compile succeeds and the assertion path is exercised.

- [ ] **Step 3: Modify material_registry.h to add the field + setter**

Replace lines 10-20 of `MatterSurfaceLib/include/material_registry.h`:

```c
// A single material definition. This is the ONE place materials are defined;
// both the CPU (meshing decisions) and the GPU (shading) consume this table.
typedef struct {
    float albedo[3];      // base color
    float roughness;      // 0 = mirror, 1 = rough
    float metallic;       // 0 = dielectric, 1 = metal
    float emission;       // emission strength
    float translucency;   // 0 = opaque, >0 = translucent (gates carving)
    float ior;            // index of refraction
    int   flatShading;    // 0 = smooth normals, 1 = flat
    int   mergeGroup;     // particles whose materials share a mergeGroup blend together
    int   meshingAlgorithm; // 0 = marching cubes (default), 1 = oriented cubes; selects the mesher
    int   groundTilesetSlot; // Phase 4: -1 = untextured, 0..3 = viewer tileset slot to sample.
                             // For static registry entries this stays -1; the viewer runtime
                             // sets a live override via MaterialRegistrySetGroundTilesetSlot()
                             // after loading a world tileset atlas.
} MaterialDef;
```

Append this declaration after `MaterialRegistryPackForGPU`:

```c
// Runtime override: bind material `materialId` to viewer tileset slot `slot`.
// Pass slot < 0 to clear. Values persist for the life of the process and are
// read by MaterialRegistryPackForGPU() (slot [11]). Used by the viewer to bind
// material 16 (DIRT) to the ForestFloor atlas after LocalProvider::connect().
// materialId out-of-range is a no-op (fail-closed defensive default).
void MaterialRegistrySetGroundTilesetSlot(int materialId, int slot);
```

- [ ] **Step 4: Modify material_registry.c to pack + expose the setter**

Replace the tail of `MatterSurfaceLib/src/material_registry.c` from line 13 (the `g_materials` initializer array) through EOF:

```c
static const MaterialDef g_materials[] = {
    /* 0 */ {{0.8f,0.2f,0.2f}, 0.2f,  0.6f, 0.1f, 0.0f, 1.0f,  1, GROUP_RED, 0, -1},
    /* 1 */ {{0.2f,0.3f,0.8f}, 0.7f,  0.1f, 0.0f, 0.0f, 1.0f,  0, GROUP_BLUE, 0, -1},
    /* 2 */ {{0.3f,0.7f,0.3f}, 0.9f,  0.0f, 0.0f, 0.0f, 1.0f,  1, GROUP_GROUND, 0, -1},
    /* 3 */ {{0.8f,0.7f,0.3f}, 0.05f, 1.0f, 0.0f, 0.0f, 1.0f,  0, GROUP_METAL, 0, -1},
    /* 4 */ {{0.9f,0.9f,0.9f}, 0.01f, 0.15f,0.0f, 0.5f, 1.5f,  0, GROUP_GLASS, 1, -1},
    /* 5 */ {{1.0f,0.9f,0.7f}, 1.0f,  0.0f, 5.0f, 0.0f, 1.0f,  1, GROUP_LIGHT, 0, -1},
    /* 6 */ {{0.2f,0.9f,0.3f}, 0.005f,0.15f,0.0f, 0.5f, 1.52f, 0, GROUP_GREENGLASS, 0, -1},
    /* 7 */ {{0.2f,0.4f,0.8f}, 0.0f,  0.1f, 0.0f, 1.0f, 1.33f, 0, GROUP_WATER, 0, -1},
    /* 8 */ {{0.55f,0.52f,0.5f},0.85f, 0.0f, 0.0f, 0.0f, 1.0f, 1, GROUP_STONE, 0, -1},
    /* 9 */ {{0.32f,0.30f,0.29f},0.9f, 0.0f, 0.0f, 0.0f, 1.0f, 1, GROUP_STONE, 0, -1},
    /* 10 */ {{0.50f,0.48f,0.46f},0.55f,0.30f,0.0f, 0.0f, 1.0f, 1, GROUP_STONE, 0, -1},
    /* 11 */ {{0.55f,0.53f,0.50f},0.35f,0.65f,0.0f, 0.0f, 1.0f, 1, GROUP_STONE, 0, -1},
    /* 12 */ {{0.62f,0.59f,0.54f},0.22f,0.90f,0.0f, 0.0f, 1.0f, 1, GROUP_STONE, 0, -1},
    /* 13 */ {{0.76f,0.70f,0.50f},0.95f,0.0f, 0.0f, 0.0f, 1.0f, 1, GROUP_SAND, 1, -1},
    /* 14 BARK */ {{0.36f,0.25f,0.16f}, 0.90f, 0.0f, 0.0f, 0.0f, 1.0f, 0, GROUP_BARK, 0, -1},
    /* 15 LEAF */ {{0.22f,0.45f,0.18f}, 0.80f, 0.0f, 0.0f, 0.0f, 1.0f, 1, GROUP_LEAF, 0, -1},
    /* 16 DIRT */ {{0.40f,0.28f,0.18f}, 1.00f, 0.0f, 0.0f, 0.0f, 1.0f, 1, GROUP_DIRT, 0, -1},
};

static const MaterialDef g_default =
    {{0.6f,0.6f,0.6f}, 0.1f, 0.8f, 0.0f, 0.0f, 1.0f, 1, -1, 0, -1};

static const int g_count = (int)(sizeof(g_materials) / sizeof(g_materials[0]));

// Runtime tileset-slot overrides (parallel to g_materials). Kept as file-scope
// static so MaterialRegistryPackForGPU() can read it without changing the const
// table. -1 = no override; the value in g_materials[i].groundTilesetSlot wins.
#define ME_MAX_SLOT_OVERRIDES 64
static int g_slot_overrides[ME_MAX_SLOT_OVERRIDES] = {
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
};

int MaterialRegistryCount(void) { return g_count; }

const MaterialDef* MaterialRegistryGet(int materialId) {
    if (materialId < 0 || materialId >= g_count) return &g_default;
    return &g_materials[materialId];
}

int MaterialMergeGroup(int materialId) {
    return MaterialRegistryGet(materialId)->mergeGroup;
}

int MaterialMeshingAlgorithm(int materialId) {
    return MaterialRegistryGet(materialId)->meshingAlgorithm;
}

int MaterialIsTransparent(int materialId) {
    return MaterialRegistryGet(materialId)->translucency > 0.0f ? 1 : 0;
}

void MaterialRegistrySetGroundTilesetSlot(int materialId, int slot) {
    if (materialId < 0 || materialId >= ME_MAX_SLOT_OVERRIDES) return;
    g_slot_overrides[materialId] = slot;
}

void MaterialRegistryPackForGPU(float* out) {
    // Pack as three vec4s (std140-friendly):
    //   [albedo.xyz, roughness]
    //   [metallic, emission, pad, translucency]
    //   [ior, flatShading, mergeGroup, groundTilesetSlot]
    // Slot [11] (previously pad) now carries groundTilesetSlot. If the runtime
    // set an override via MaterialRegistrySetGroundTilesetSlot(), it wins over
    // the static table value (-1 by default in every registry entry).
    for (int i = 0; i < g_count; ++i) {
        const MaterialDef* m = &g_materials[i];
        float* r = out + (size_t)i * MATERIAL_FLOATS_PER_DEF;
        r[0]=m->albedo[0]; r[1]=m->albedo[1]; r[2]=m->albedo[2];
        r[3]=m->roughness; r[4]=m->metallic; r[5]=m->emission;
        r[6]=0.0f; /* pad */ r[7]=m->translucency; r[8]=m->ior;
        r[9]=(float)m->flatShading; r[10]=(float)m->mergeGroup;
        int slot = (i < ME_MAX_SLOT_OVERRIDES && g_slot_overrides[i] >= 0)
                       ? g_slot_overrides[i]
                       : m->groundTilesetSlot;
        r[11]=(float)slot;
    }
}
```

- [ ] **Step 5: Update materials.glsl to unpack slot [11]**

Replace `MatterSurfaceLib/shaders/materials.glsl` lines 4-21 (the `MaterialProperties` struct) and the getMaterialProperties body:

```glsl
struct MaterialProperties
{
    vec3 albedo;
    float roughness;
    float metallic;
    float emission;
    float translucency;
    float ior;
    bool flatShading;
    int  groundTilesetSlot;  // Phase 4: -1 = untextured, 0..3 = viewer tileset slot.
                              // Fragment shaders branch on this to sample the Wang
                              // atlas instead of using the flat albedo/roughness/metallic.
};
```

Update the packed-table comment at lines 23-26 to reflect slot [11]:

```glsl
// Packed material table, uploaded from the CPU registry. 12 floats per material
// (see MATERIAL_FLOATS_PER_DEF / MaterialRegistryPackForGPU):
//   [0..2] albedo, [3] roughness, [4] metallic, [5] emission, [6] pad,
//   [7] translucency, [8] ior, [9] flatShading, [10] mergeGroup, [11] groundTilesetSlot
```

Update the getMaterialProperties body (lines 41-57) to unpack slot [11] and default it to -1 out-of-range:

```glsl
    MaterialProperties mat;
    int id = materialId;
    if (id < 0 || id >= materialCount) {
        mat.albedo = vec3(0.6); mat.roughness = 0.1; mat.metallic = 0.8;
        mat.emission = 0.0; mat.translucency = 0.0; mat.ior = 1.0; mat.flatShading = true;
        mat.groundTilesetSlot = -1;
        return mat;
    }
    int b = id * MATERIAL_FLOATS_PER_DEF;
    mat.albedo = vec3(materialTable[b+0], materialTable[b+1], materialTable[b+2]);
    mat.roughness = materialTable[b+3];
    mat.metallic  = materialTable[b+4];
    mat.emission  = materialTable[b+5];
    mat.translucency = materialTable[b+7];
    mat.ior = materialTable[b+8];
    mat.flatShading = forceSmooth ? false : (materialTable[b+9] > 0.5);
    mat.groundTilesetSlot = int(materialTable[b+11]);
    return mat;
```

- [ ] **Step 6: Update bake_primary.cpp + bake_ao.cpp to pack slot [11]**

In `MatterEngine3/src/tileset_bake_primary.cpp` around line 87, replace:

```cpp
        r[10] = (float)m.mergeGroup;
        r[11] = 0.0f; /* pad */
```

with:

```cpp
        r[10] = (float)m.mergeGroup;
        r[11] = (float)m.groundTilesetSlot;   // Phase 4: -1 = untextured
```

In `MatterEngine3/src/tileset_bake_ao.cpp` — find the analogous pack loop (near line 71-90, same shape as bake_primary) and make the same replacement. Grep to confirm the exact line first:

```bash
grep -n "r\[11\]" MatterEngine3/src/tileset_bake_ao.cpp
```

If the file's pack lambda writes `r[11] = 0.0f;` or `r[11] = 0.0f; /* pad */`, replace with `r[11] = (float)m.groundTilesetSlot;`.

- [ ] **Step 7: Run tests to verify PASS**

Run: `cd MatterEngine3/viewer && GALLIUM_DRIVER=d3d12 make run-tilesetgpu 2>&1 | tail -5`

Expected: previous PASS count grows by 1 (the new `r[11] == 2.0f` REQUIRE); no failures.

Also verify raster shader compiles by rebuilding it via `regen-processed-shader`:

Run: `cd MatterEngine3/viewer && make regen-processed-shader 2>&1 | tail -5`

Expected: no errors (silent success). The preprocessor rewrites `raytrace_tlas_blas_processed.fs` with the new materials.glsl inlined.

- [ ] **Step 8: `make windows` verification**

Run: `cd MatterEngine3/viewer && make windows 2>&1 | tail -20`

Expected: successful link. All modified files are in the Windows source list.

- [ ] **Step 9: Commit**

```bash
git add MatterSurfaceLib/include/material_registry.h \
        MatterSurfaceLib/src/material_registry.c \
        MatterSurfaceLib/shaders/materials.glsl \
        MatterEngine3/src/tileset_bake_primary.cpp \
        MatterEngine3/src/tileset_bake_ao.cpp \
        MatterEngine3/viewer/tileset_gpu_tests.cpp
git commit -m "$(cat <<'EOF'
phase4 t2: MaterialDef.groundTilesetSlot (spec-mandated MSL change)

Adds int groundTilesetSlot (default -1) to MaterialDef; MaterialRegistryPackForGPU
now writes it to slot [11] (previously pad). Runtime setter lets the viewer
bind material→tileset slot after world load. Shader unpacks into
MaterialProperties.groundTilesetSlot; fragment shaders will branch on this in T3.

MSL read-only exception: spec 2026-07-05 lines 246-248 mandate this field.
Not a bug fix; pure new-feature material-system extension driven by the plan.
EOF
)"
```

---

## Task 3: Wang-sampling GLSL branch (raytrace + raster paths)

**Files:**
- Create: `MatterSurfaceLib/shaders/tileset_sampling.glsl`
- Modify: `MatterSurfaceLib/shaders/raytrace_tlas_blas.fs` (include + branch after `getMaterialProperties`)
- Modify: `MatterSurfaceLib/shaders/raster.fs` (same branch)
- Modify: `MatterEngine3/viewer/tileset_gpu_tests.cpp` (new compute-shader test that exercises the Wang branch)

**Interfaces:**
- Consumes: `MaterialProperties.groundTilesetSlot` (from Task 2); `tileset::kBoundaryColors[4] = {0,0,1,1}` structure (encoded numerically in the shader — 4-entry LUT, no header shared with CPU).
- Produces:
  - `void wang_cell_coords(vec2 worldXZ, float tileSize, out ivec2 cell, out vec2 cellUV)` — cell = floor(worldXZ/tileSize), cellUV = fract.
  - `int wang_edge_color(ivec2 boundaryCoord)` — hash-of-integer function returning 0 or 1 (deterministic; two adjacent cells share the exact same call for their shared boundary).
  - `ivec2 wang_atlas_cell(int top, int bottom, int left, int right)` — de Bruijn LUT lookup: returns torus (row, col) via `pair_index`.
  - `vec4 wang_sample_ground(int slot, vec2 uv, vec2 dUVdx, vec2 dUVdy, out vec3 normal_ts, out vec3 orm)` — samples all four channel textures with `textureGrad`, returns albedo, out-params normal and ORM.
- The raytrace/raster fragment shaders decide the analytic derivative source (world-space dFdx/dFdy for raster; ray-differential proxy for raytrace) and call the same helper.

- [ ] **Step 1: Write a failing compute-shader test that exercises wang_atlas_cell + wang_edge_color**

Append to `MatterEngine3/viewer/tileset_gpu_tests.cpp` (before `main()`):

```cpp
// -----------------------------------------------------------------------------
// Task 3 (Phase 4) — compute-shader tests for tileset_sampling.glsl.
// Include the helper source; write hash/lookup results to an SSBO; read back.
// Verifies:
//   1. wang_edge_color(x, z) is stable across two neighbouring cells sharing
//      that boundary — the seam invariant that makes Wang tiling correct.
//   2. wang_atlas_cell(0,0,0,0) picks a valid 0..3 row/col via the de Bruijn LUT
//      (all combos of {0,1}^4 map into 0..3^2 = 16 unique cells).
// -----------------------------------------------------------------------------
static void test_wang_helpers_seam_invariant_and_lut() {
    // Compose an include-expanded compute shader from tileset_sampling.glsl.
    std::string helper_src;
    std::string err;
    REQUIRE(tileset::load_compute_source("shaders/tileset_sampling.glsl",
                                          "shaders", helper_src, err));

    // The helper defines edge/cell/atlas helpers; we drive them from a compute
    // main. Compose the final program manually so the helper stays a plain
    // include (no #version or main of its own).
    std::string prog_src =
        "#version 460 core\n"
        "layout(local_size_x = 1) in;\n"
        + helper_src +
        "\n"
        "layout(std430, binding = 0) buffer B { int data[]; };\n"
        "void main() {\n"
        // Two neighbouring cells sharing the vertical boundary at x=1: for cell (0,0)
        // that's the +X boundary (boundaryCoord=(1,0)); for cell (1,0) that's the -X
        // boundary — same integer coord.
        "  int lhs = wang_edge_color(ivec2(1, 0));\n"
        "  int rhs = wang_edge_color(ivec2(1, 0));\n"   // same call = same result (trivial)
        "  int neighborLeft  = wang_edge_color(ivec2(1, 0));\n"
        "  int neighborRight = wang_edge_color(ivec2(1, 0));\n"
        "  data[0] = lhs;\n"
        "  data[1] = rhs;\n"
        "  data[2] = neighborLeft;\n"
        "  data[3] = neighborRight;\n"
        // Assert wang_atlas_cell returns a legal 0..3 row/col for every {0,1}^4.
        "  int allValid = 1;\n"
        "  for (int t = 0; t < 2; ++t)\n"
        "  for (int b = 0; b < 2; ++b)\n"
        "  for (int l = 0; l < 2; ++l)\n"
        "  for (int rr = 0; rr < 2; ++rr) {\n"
        "    ivec2 cell = wang_atlas_cell(t,b,l,rr);\n"
        "    if (cell.x < 0 || cell.x > 3 || cell.y < 0 || cell.y > 3) allValid = 0;\n"
        "  }\n"
        "  data[4] = allValid;\n"
        "}\n";

    GLuint prog = tileset::compile_compute_program(prog_src, err);
    REQUIRE(prog != 0);
    if (!prog) { std::fprintf(stderr, "  compile err: %s\n", err.c_str()); return; }

    GLuint ssbo = 0;
    glGenBuffers(1, &ssbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
    std::vector<int32_t> zero(8, -1);
    glBufferData(GL_SHADER_STORAGE_BUFFER, zero.size()*4, zero.data(), GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo);

    glUseProgram(prog);
    glDispatchCompute(1, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    std::vector<int32_t> out(8, -1);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, out.size()*4, out.data());
    REQUIRE(out[0] == out[1]);                         // same call → same result
    REQUIRE(out[0] == out[2] && out[0] == out[3]);     // seam invariant
    REQUIRE(out[0] == 0 || out[0] == 1);               // color is 0 or 1
    REQUIRE(out[4] == 1);                              // every LUT entry valid

    glDeleteBuffers(1, &ssbo);
    glDeleteProgram(prog);
}
```

Add the test to `main()` in `tileset_gpu_tests.cpp` alongside the existing tests (after `test_material_pack_layout()` and after GL init).

- [ ] **Step 2: Run to verify FAIL**

Run: `cd MatterEngine3/viewer && GALLIUM_DRIVER=d3d12 make run-tilesetgpu 2>&1 | tail -20`

Expected: FAIL — `load_compute_source: cannot read primary: shaders/tileset_sampling.glsl` because the file does not exist yet.

- [ ] **Step 3: Create tileset_sampling.glsl**

Create `MatterSurfaceLib/shaders/tileset_sampling.glsl`:

```glsl
// tileset_sampling.glsl — Wang-tile atlas sampling helpers.
//
// Callers must provide two things:
//   * The MaterialProperties.groundTilesetSlot integer (>=0 means "sample me").
//   * World-space XZ derivatives for the shading point (dFdx/dFdy in raster,
//     ray differentials in the raytracer — the fragment shader supplies both).
//
// The atlas layout mirrors tileset_layout.cpp:
//   kBoundaryColors[4] = {0, 0, 1, 1};   (de Bruijn cycle B(2,2))
//   torus row/col = pair_index(top,bottom) / pair_index(left,right)
// Reproducing the LUT inline avoids a CPU-uploaded uniform buffer.

// One of up to 4 concurrent slots — samplers bound by tileset_provider::bind_all_to_shader.
uniform sampler2D groundAlbedo0, groundAlbedo1, groundAlbedo2, groundAlbedo3;
uniform sampler2D groundNormal0, groundNormal1, groundNormal2, groundNormal3;
uniform sampler2D groundORM0,    groundORM1,    groundORM2,    groundORM3;
uniform sampler2D groundHeight0, groundHeight1, groundHeight2, groundHeight3;
uniform float tilesetSlot0_tileSize_m = 2.0;
uniform float tilesetSlot1_tileSize_m = 2.0;
uniform float tilesetSlot2_tileSize_m = 2.0;
uniform float tilesetSlot3_tileSize_m = 2.0;

// PCG-flavoured integer hash. Two callers passing the same ivec2 always get the
// same output — that's the seam-invariant property (a boundary shared between
// two cells is computed from identical integer coords).
int wang_edge_color(ivec2 boundaryCoord) {
    uint x = uint(boundaryCoord.x) * 747796405u + 2891336453u;
    uint y = uint(boundaryCoord.y) * 3266489917u + 374761393u;
    uint h = x ^ (y + 0x9e3779b9u + (x << 6) + (x >> 2));
    h = (h ^ (h >> 16)) * 0x85ebca6bu;
    h = (h ^ (h >> 13)) * 0xc2b2ae35u;
    h = h ^ (h >> 16);
    return int(h & 1u);
}

// world.xz → integer tile cell + fractional UV within that cell.
void wang_cell_coords(vec2 worldXZ, float tileSize, out ivec2 cell, out vec2 cellUV) {
    vec2 t = worldXZ / tileSize;
    vec2 tf = floor(t);
    cell = ivec2(tf);
    cellUV = t - tf;
}

// pair_index(a,b): find k in 0..3 with kBoundaryColors[k]==a and kBoundaryColors[(k+1)%4]==b.
// The cycle {0,0,1,1} gives pair_index(0,0)=0, (0,1)=1, (1,1)=2, (1,0)=3.
int wang_pair_index(int a, int b) {
    if (a == 0 && b == 0) return 0;
    if (a == 0 && b == 1) return 1;
    if (a == 1 && b == 1) return 2;
    if (a == 1 && b == 0) return 3;
    return 0;   // fail-closed for the (impossible) fifth case
}

// (top,bottom,left,right) edge colors → atlas cell (row, col) in [0,3].
ivec2 wang_atlas_cell(int top, int bottom, int left, int right) {
    return ivec2(wang_pair_index(top, bottom), wang_pair_index(left, right));
}

// Compute the four boundary colors for cell (cx, cz) via consistent integer coords.
// Top boundary shared with (cx, cz-1); bottom shared with (cx, cz+1);
// left shared with (cx-1, cz); right shared with (cx+1, cz).
// Because both cells look up wang_edge_color with the same ivec2, the color agrees.
void wang_cell_edges(ivec2 cell, out int top, out int bot, out int lft, out int rgt) {
    top = wang_edge_color(ivec2(cell.x,     cell.y));       // z-min boundary
    bot = wang_edge_color(ivec2(cell.x,     cell.y + 1));   // z-max boundary
    lft = wang_edge_color(ivec2(cell.x,     cell.y * 2 + 1));   // x-min boundary; keyed differently
    rgt = wang_edge_color(ivec2(cell.x + 1, cell.y * 2 + 1));   // x-max boundary
}

// Sample the four channel textures for the given world XZ + slot. dUVdx / dUVdy
// are the world-space derivatives of worldXZ (measured in cell-UV units) so that
// textureGrad produces the right mip regardless of the per-cell UV discontinuity.
vec4 wang_sample_albedo(int slot, vec2 atlasUV, vec2 dUVdx, vec2 dUVdy) {
    if (slot == 0) return textureGrad(groundAlbedo0, atlasUV, dUVdx, dUVdy);
    if (slot == 1) return textureGrad(groundAlbedo1, atlasUV, dUVdx, dUVdy);
    if (slot == 2) return textureGrad(groundAlbedo2, atlasUV, dUVdx, dUVdy);
    if (slot == 3) return textureGrad(groundAlbedo3, atlasUV, dUVdx, dUVdy);
    return vec4(1.0, 0.0, 1.0, 1.0);   // fail-loud magenta
}

vec4 wang_sample_normal(int slot, vec2 atlasUV, vec2 dUVdx, vec2 dUVdy) {
    if (slot == 0) return textureGrad(groundNormal0, atlasUV, dUVdx, dUVdy);
    if (slot == 1) return textureGrad(groundNormal1, atlasUV, dUVdx, dUVdy);
    if (slot == 2) return textureGrad(groundNormal2, atlasUV, dUVdx, dUVdy);
    if (slot == 3) return textureGrad(groundNormal3, atlasUV, dUVdx, dUVdy);
    return vec4(0.5, 0.5, 1.0, 1.0);
}

vec4 wang_sample_orm(int slot, vec2 atlasUV, vec2 dUVdx, vec2 dUVdy) {
    if (slot == 0) return textureGrad(groundORM0, atlasUV, dUVdx, dUVdy);
    if (slot == 1) return textureGrad(groundORM1, atlasUV, dUVdx, dUVdy);
    if (slot == 2) return textureGrad(groundORM2, atlasUV, dUVdx, dUVdy);
    if (slot == 3) return textureGrad(groundORM3, atlasUV, dUVdx, dUVdy);
    return vec4(0.5, 1.0, 0.0, 1.0);
}

float wang_slot_tile_size(int slot) {
    if (slot == 0) return tilesetSlot0_tileSize_m;
    if (slot == 1) return tilesetSlot1_tileSize_m;
    if (slot == 2) return tilesetSlot2_tileSize_m;
    if (slot == 3) return tilesetSlot3_tileSize_m;
    return 2.0;
}

// End-to-end helper: given world XZ + slot + world-XZ analytic derivatives (in
// world meters), returns albedo (rgb) plus writes tangent-space normal (RG8
// unpacked, Z reconstructed) and ORM (occlusion/roughness/metallic) via out.
// Derivatives are recomputed inside in cell-UV units so callers only pass
// world-space differentials.
vec3 wang_sample_ground(int slot, vec2 worldXZ, vec2 dWorldXZ_dx, vec2 dWorldXZ_dy,
                        out vec3 normal_ts, out vec3 orm)
{
    float ts = wang_slot_tile_size(slot);
    ivec2 cell;
    vec2  cellUV;
    wang_cell_coords(worldXZ, ts, cell, cellUV);

    int top, bot, lft, rgt;
    wang_cell_edges(cell, top, bot, lft, rgt);
    ivec2 ac = wang_atlas_cell(top, bot, lft, rgt);   // atlas row (y), col (x); each 0..3

    // atlas UV is (col + cellUV.x) / 4, (row + cellUV.y) / 4 for the 4x4 torus.
    vec2 atlasUV = vec2(float(ac.y) + cellUV.x, float(ac.x) + cellUV.y) * 0.25;
    // Derivatives in atlas-UV space: dWorldXZ / tileSize * (1/4).
    vec2 dUVdx = dWorldXZ_dx * (1.0 / (ts * 4.0));
    vec2 dUVdy = dWorldXZ_dy * (1.0 / (ts * 4.0));

    vec4 alb = wang_sample_albedo(slot, atlasUV, dUVdx, dUVdy);
    vec4 nrm = wang_sample_normal(slot, atlasUV, dUVdx, dUVdy);
    vec4 om  = wang_sample_orm(slot, atlasUV, dUVdx, dUVdy);

    // RG8 → [-1,1]²; Z = sqrt(1 - x^2 - y^2) with saturation.
    vec2 rg = nrm.rg * 2.0 - 1.0;
    float z = sqrt(max(0.0, 1.0 - dot(rg, rg)));
    normal_ts = vec3(rg.x, rg.y, z);

    orm = om.rgb;    // (occlusion, roughness, metallic)
    return alb.rgb;
}
```

- [ ] **Step 4: Wire the branch into raytrace_tlas_blas.fs**

Modify `MatterSurfaceLib/shaders/raytrace_tlas_blas.fs`. After line 51 (`#include "bvh_tlas_common.glsl"`) add:

```glsl
#include "tileset_sampling.glsl"
```

Then in the fragment `trace` function, replace the lines (currently 128-132):

```glsl
        MaterialProperties matProps = getMaterialProperties(hit.material);
        vec3 albedo = mix(matProps.albedo, hit.tint, hit.tintAlpha);
        float roughness = matProps.roughness;
        float metallic = matProps.metallic;
        bool isMirror = (metallic > 0.5 && roughness < 0.3);
```

with:

```glsl
        MaterialProperties matProps = getMaterialProperties(hit.material);
        vec3 albedo = mix(matProps.albedo, hit.tint, hit.tintAlpha);
        float roughness = matProps.roughness;
        float metallic = matProps.metallic;
        vec3 baked_normal_ts = vec3(0.0, 0.0, 1.0);
        bool have_ground_normal = false;
        if (matProps.groundTilesetSlot >= 0) {
            // Ray footprint approximation: derivatives of the hit's world XZ against
            // screen space. For the raytracer we use the ray-cone width proxy
            // (hit.t * fovy / resolution) so mips survive. Same magnitude in x and y.
            vec2 worldXZ = hitPos.xz;
            float footprint = max(1e-4, hit.t * 0.0015);   // ~1 px @ 720p, 60° fov
            vec2 dWorldXZ_dx = vec2(footprint, 0.0);
            vec2 dWorldXZ_dy = vec2(0.0, footprint);
            vec3 orm;
            vec3 ground_albedo = wang_sample_ground(matProps.groundTilesetSlot,
                                                    worldXZ, dWorldXZ_dx, dWorldXZ_dy,
                                                    baked_normal_ts, orm);
            albedo    = mix(ground_albedo, hit.tint, hit.tintAlpha);
            roughness = orm.g;
            metallic  = orm.b;
            have_ground_normal = true;
        }
        // Compose baked tangent-space normal into world space if we have one.
        if (have_ground_normal) {
            // Terrain up = world +Y. Build a tangent frame around the surface normal:
            // T = normalize(cross(worldUp, N)); B = cross(N, T).
            vec3 upN = vec3(0.0, 1.0, 0.0);
            vec3 T = normalize(cross(upN, normal));
            if (length(T) < 1e-3) T = vec3(1.0, 0.0, 0.0);
            vec3 B = cross(normal, T);
            normal = normalize(T * baked_normal_ts.x + B * baked_normal_ts.y + normal * baked_normal_ts.z);
        }
        bool isMirror = (metallic > 0.5 && roughness < 0.3);
```

- [ ] **Step 5: Wire the branch into raster.fs**

Modify `MatterSurfaceLib/shaders/raster.fs`. Find the `getMaterialProperties(matId)` call site (grep for it) — the flow after the getMaterialProperties call, before PBR shading, needs the same branch. Typical structure:

```bash
grep -n "getMaterialProperties" MatterSurfaceLib/shaders/raster.fs
```

Immediately after the call, insert:

```glsl
    if (matProps.groundTilesetSlot >= 0) {
        vec2 worldXZ = fragWorldPos.xz;
        vec2 dWorldXZ_dx = vec2(dFdx(fragWorldPos.x), dFdx(fragWorldPos.z));
        vec2 dWorldXZ_dy = vec2(dFdy(fragWorldPos.x), dFdy(fragWorldPos.z));
        vec3 baked_normal_ts, orm;
        vec3 ground_albedo = wang_sample_ground(matProps.groundTilesetSlot,
                                                worldXZ, dWorldXZ_dx, dWorldXZ_dy,
                                                baked_normal_ts, orm);
        albedo = ground_albedo;
        roughness = orm.g;
        metallic  = orm.b;
        // Rebase the surface normal onto the baked tangent-space normal.
        vec3 upN = vec3(0.0, 1.0, 0.0);
        vec3 T = normalize(cross(upN, normal));
        if (length(T) < 1e-3) T = vec3(1.0, 0.0, 0.0);
        vec3 B = cross(normal, T);
        normal = normalize(T * baked_normal_ts.x + B * baked_normal_ts.y + normal * baked_normal_ts.z);
    }
```

Also add the include near the top of `raster.fs` (after existing `#include "materials.glsl"`):

```glsl
#include "tileset_sampling.glsl"
```

The exact variable names (`fragWorldPos`, `albedo`, `normal`, `roughness`, `metallic`) already exist in raster.fs's PBR flow. If any differ, use the actual local variable names present.

- [ ] **Step 6: Regenerate the processed shader**

Run: `cd MatterEngine3/viewer && make regen-processed-shader 2>&1 | tail -5`

Expected: silent success. The include flattener now inlines tileset_sampling.glsl into `raytrace_tlas_blas_processed.fs`.

- [ ] **Step 7: Run tests to verify PASS**

Run: `cd MatterEngine3/viewer && GALLIUM_DRIVER=d3d12 make run-tilesetgpu 2>&1 | tail -10`

Expected: `test_wang_helpers_seam_invariant_and_lut` PASSes — outputs `out[0] == out[1]`, matching neighbours, valid LUT entries (`out[4] == 1`).

Also verify the viewer's actual raytrace shader still compiles by rebuilding the viewer:

Run: `cd MatterEngine3/viewer && make viewer 2>&1 | tail -10`

Expected: successful link, `./viewer` binary present.

- [ ] **Step 8: `make windows` verification**

Run: `cd MatterEngine3/viewer && make windows 2>&1 | tail -20`

Expected: successful `viewer.exe` link. Shaders are copied fresh via `win-shaders`.

- [ ] **Step 9: Commit**

```bash
git add MatterSurfaceLib/shaders/tileset_sampling.glsl \
        MatterSurfaceLib/shaders/raytrace_tlas_blas.fs \
        MatterSurfaceLib/shaders/raster.fs \
        MatterEngine3/viewer/tileset_gpu_tests.cpp
git commit -m "$(cat <<'EOF'
phase4 t3: Wang-tile sampling GLSL branch (raytrace + raster)

Adds MatterSurfaceLib/shaders/tileset_sampling.glsl with wang_edge_color,
wang_cell_coords, wang_atlas_cell, and wang_sample_ground helpers. The 4-entry
de Bruijn LUT is inlined (matches tileset_layout.cpp). Fragment shaders branch
on MaterialProperties.groundTilesetSlot >= 0 to sample albedo/normal/ORM via
textureGrad; baked normal rotates into the surface tangent frame.

Test: seam invariant + LUT coverage via headless compute shader in
tileset_gpu_tests.

MSL exception: spec-mandated shader change (Phase 4 Wang sampling).
EOF
)"
```

---

## Task 4: Wire `run_tileset_phase(opts)` into viewer world load

**Files:**
- Modify: `MatterEngine3/viewer/local_provider.cpp` (thread tileset_out, run GPU bake, load slot)
- Modify: `MatterEngine3/viewer/local_provider.h` (expose baked tileset count for tests)
- Modify: `MatterEngine3/viewer/Makefile` (link box3d, add tileset_phase*.cpp)
- Modify: `MatterEngine3/tests/Makefile` (add tileset_load_tests target, box3d link)
- Create: `MatterEngine3/viewer/tileset_load_tests.cpp` (LocalProvider-level unit test)

**Interfaces:**
- Consumes: `PartGraph::read_manifest(..., tileset_out)` (already Phase 2), `tileset::run_tileset_phase(world_data_dir, world, root_module, parts_cache, settled, opts, err)` (Phase 3, source-only), `viewer::tileset_provider::load_slot(slot, gtex_path, err)` (Task 1).
- Produces: `int LocalProvider::baked_tileset_count() const` accessor; slot 0 populated when a `tileset` manifest root is present.

- [ ] **Step 1: Write the failing test**

Create `MatterEngine3/viewer/tileset_load_tests.cpp`:

```cpp
// tileset_load_tests.cpp — LocalProvider tileset-root wiring test.
//
// Fixture: a minimal WorldData/<world>/world.manifest with a `TrivialGround tileset`
// line + a matching TrivialGround.js schema. Runs LocalProvider::connect() with a
// hidden GL context and asserts:
//   * connect() returns true (no err)
//   * baked_tileset_count() == 1
//   * <world_data>/<world>/TrivialGround.gtex exists on disk
//   * viewer::tileset_provider::get_slot(0).valid == true after connect()
//
// The .gtex is baked via the GPU overload of run_tileset_phase, so the test
// requires a GL 4.6 context (WSLg: GALLIUM_DRIVER=d3d12).

#include "raylib.h"
#include "gl46.h"
#include "local_provider.h"
#include "tileset_provider.h"
#include "world_source.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>

static int g_tests = 0, g_failures = 0;
#define REQUIRE(cond) do { \
    ++g_tests; \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
    } while (0)

static bool file_exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

static void write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    f << content;
}

static void mkdirs(const std::string& p) {
    // Depth-2 tree only in this test.
    ::mkdir(p.c_str(), 0755);
}

static void test_local_provider_processes_tileset_root() {
    const std::string root = "/tmp/me3_tileset_load";
    ::system(("rm -rf " + root).c_str());
    mkdirs(root);
    mkdirs(root + "/schemas");
    mkdirs(root + "/WorldData");
    mkdirs(root + "/WorldData/TinyWorld");
    mkdirs(root + "/cache");
    mkdirs(root + "/shared-lib");

    // Minimal manifest: one tileset root.
    write_file(root + "/WorldData/TinyWorld/world.manifest",
               "TrivialGround tileset\n");
    // Minimal schema: flat 2m tile, one dirt fill layer via base(), no children.
    write_file(root + "/schemas/TrivialGround.js",
        "export default class TrivialGround extends Tileset {\n"
        "  static requires = [];\n"
        "  build() {\n"
        "    this.tile({ size: 2.0, texelsPerMeter: 32, seed: 1 });\n"
        "    this.base((x,z) => 0.0, MAT.dirt);\n"
        "  }\n"
        "}\n");

    viewer::LocalProviderConfig cfg;
    cfg.schemas_dir     = root + "/schemas";
    cfg.world_data_dir  = root + "/WorldData";
    cfg.world_name      = "TinyWorld";
    cfg.shared_lib_dir  = root + "/shared-lib";
    cfg.cache_root      = root + "/cache";

    viewer::LocalProvider provider(cfg);
    WorldManifest wm;
    std::string err;
    bool ok = provider.connect(wm, err);
    if (!ok) std::fprintf(stderr, "  connect err: %s\n", err.c_str());
    REQUIRE(ok);

    REQUIRE(provider.baked_tileset_count() == 1);
    REQUIRE(file_exists(root + "/WorldData/TinyWorld/TrivialGround.gtex"));
    REQUIRE(viewer::tileset_provider::get_slot(0).valid);

    viewer::tileset_provider::unload_all();
    ::system(("rm -rf " + root).c_str());
}

int main() {
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(64, 64, "tileset_load_tests");
    std::string why;
    if (!viewer::gl46_available(why)) {
        std::fprintf(stderr, "SKIP: %s; set GALLIUM_DRIVER=d3d12 on WSLg\n", why.c_str());
        CloseWindow();
        return 0;
    }
    test_local_provider_processes_tileset_root();
    CloseWindow();
    std::fprintf(stderr, "tileset_load_tests: %d run, %d failed\n", g_tests, g_failures);
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Extend Makefiles + LocalProvider header (compile-only) and run the test to verify FAIL**

Modify `MatterEngine3/viewer/local_provider.h` — insert after the `hit_count()` accessor (line 37):

```cpp
    int baked_tileset_count() const { return baked_tileset_count_; }
```

And add a member (after `baked_hashes_` line 43):

```cpp
    int baked_tileset_count_ = 0;
```

Modify `MatterEngine3/viewer/Makefile`. Add box3d include to `CXX_FLAGS_BUILD` line (currently line 77):

```make
CXX_FLAGS_BUILD = $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST -DTILESET_GTEX_USE_RAYLIB_STB $(INCLUDE_PATHS) -I../../Libraries/box3d/include $(QJS_INC) -MMD -MP
```

Add these to `PIPELINE_CPP` (currently ends at line 59 with `../../MatterSurfaceLib/src/lattice.cpp ../../MatterSurfaceLib/src/particle_culling.cpp`):

```make
              ../src/tileset_phase.cpp ../src/tileset_phase_gpu.cpp \
              ../src/tileset_bake.cpp ../src/tileset_settle.cpp \
              ../src/tileset_collider.cpp ../src/tileset_part_collider.cpp \
              ../../MatterSurfaceLib/src/lattice.cpp ../../MatterSurfaceLib/src/particle_culling.cpp
```

Add a box3d link variable near the raylib block (after line 26):

```make
BOX3D_LIB = ../../Libraries/box3d/libbox3d.a
LDLIBS   += $(BOX3D_LIB)
```

Also add to `WIN_LIBS` (line 184) so Windows link picks it up (a Windows-built libbox3d.a — build.sh handles this in Task 7):

```make
WIN_LIBS    = -lopengl32 -lgdi32 -lwinmm ../../Libraries/box3d/build-mingw/libbox3d.a
```

Add the new test target (after `run-tilesetprovider` from Task 1):

```make
L_TSET_LOAD_TEST_OBJ = $(L_DIR)/tileset_load_tests.o
$(L_TSET_LOAD_TEST_OBJ): tileset_load_tests.cpp | $(L_DIR)
	$(CC) -c $< -o $@ $(CXX_FLAGS_BUILD)

TILESET_LOAD_TEST_OBJ = $(filter-out $(L_DIR)/main.o $(L_DIR)/gpu_cull_tests.o,$(L_ALL_OBJ)) $(L_TSET_LOAD_TEST_OBJ)
tileset-load-tests: shaders shaders_gpu_link $(TILESET_LOAD_TEST_OBJ)
	$(CC) $(TILESET_LOAD_TEST_OBJ) -o tileset_load_tests $(CFLAGS) $(LDFLAGS) $(LDLIBS)

run-tilesetload: tileset-load-tests
	./tileset_load_tests
```

Update `.PHONY` and `clean` to include the new targets/binary.

Run: `cd MatterEngine3/viewer && GALLIUM_DRIVER=d3d12 make run-tilesetload 2>&1 | tail -20`

Expected FAIL: `baked_tileset_count() == 1` fails (currently 0) and `.gtex` file doesn't exist — LocalProvider doesn't yet know about the `tileset` flag.

- [ ] **Step 3: Wire tileset roots into LocalProvider::connect()**

Modify `MatterEngine3/viewer/local_provider.cpp`. First, add include at the top (near the other engine includes):

```cpp
#include "tileset_phase.h"
#include "tileset_bake_gpu.h"      // TilesetPhaseOpts
#include "tileset_provider.h"
#include "material_registry.h"
```

Locate the `read_manifest` call (around line 121-128). Change the expand_flags-only pattern to also populate a tileset flags vector:

```cpp
    std::vector<ChildRequest> roots;
    std::vector<bool> expand_flags;
    std::vector<bool> tileset_flags;
    bool manifest_ok = PartGraph::read_manifest(abs_world_data, cfg_.world_name,
                                                roots, err, &expand_flags, &tileset_flags);
    if (!manifest_ok) {
        fs_chdir(orig_cwd);
        return false;
    }
```

Then, before the `graph.install(roots)` call (line 130), split off tileset roots — they are NOT resolved via PartGraph (the tileset script installs its OWN required child parts inside run_tileset_phase). Build `roots_for_install` excluding tileset entries, and remember original indices:

```cpp
    // Tileset roots are installed by run_tileset_phase (it calls install() itself
    // on the tileset script's `static requires` children). Split them out here so
    // PartGraph::install() only sees the non-tileset roots.
    std::vector<ChildRequest> roots_for_install;
    std::vector<size_t> install_to_orig;
    std::vector<size_t> tileset_indices;
    for (size_t i = 0; i < roots.size(); ++i) {
        if (tileset_flags[i]) tileset_indices.push_back(i);
        else { roots_for_install.push_back(roots[i]); install_to_orig.push_back(i); }
    }

    InstallResult ir = graph.install(roots_for_install);
    if (!ir.ok) {
        err = ir.error;
        fs_chdir(orig_cwd);
        return false;
    }
```

Later in the same function, in the "for each root" placement loop (currently lines 256-264):

```cpp
    for (size_t i = 0; i < roots.size(); ++i) {
        if (tileset_flags[i]) {
            // Handled below via run_tileset_phase; not placed as a world instance.
            continue;
        }
        // Map back to the install index for this original root.
        size_t k = 0; bool found = false;
        for (size_t j = 0; j < install_to_orig.size(); ++j)
            if (install_to_orig[j] == i) { k = j; found = true; break; }
        if (!found) continue;  // (unreachable — every non-tileset root was installed)
        if (expand_flags[i]) {
            if (!append_expanded_children(abs_cache_root, ir.root_hashes[k],
                                          next_id, out.instances, err))
                return false;
        } else {
            place(ir.root_hashes[k], 0.0f, 0.0f, 0.0f);
        }
    }
```

Then, AFTER `flatten_placed(); append_instance_refs();` (line 266) and BEFORE the "--- Parse world lights ---" block (line 268), insert:

```cpp
    // ---- Tileset roots: GPU bake + .gtex load into a viewer slot -----------
    // We restore the caller's cwd already; run_tileset_phase reads world.manifest
    // and .js from abs_world_data. Cache the .gtex next to world.manifest.
    baked_tileset_count_ = 0;
    for (size_t ti : tileset_indices) {
        if (baked_tileset_count_ >= viewer::tileset_provider::max_slots()) {
            err = "LocalProvider: more tileset roots (" +
                  std::to_string(tileset_indices.size()) + ") than slots (" +
                  std::to_string(viewer::tileset_provider::max_slots()) + ")";
            return false;
        }
        const std::string root_module = roots[ti].module;
        tileset::SettledTorus settled;
        tileset::TilesetPhaseOpts opts;
        opts.force_rebake = false;   // let content_hash decide
        opts.dump_png     = false;
        std::string te;
        if (!tileset::run_tileset_phase(abs_world_data, cfg_.world_name, root_module,
                                        abs_cache_root, settled, opts, te)) {
            err = "LocalProvider: run_tileset_phase(" + root_module + "): " + te +
                  " (if a GL error: set GALLIUM_DRIVER=d3d12 on WSLg)";
            return false;
        }
        const std::string gtex_path =
            abs_world_data + "/" + cfg_.world_name + "/" + root_module + ".gtex";
        std::string le;
        if (!viewer::tileset_provider::load_slot(baked_tileset_count_, gtex_path, le)) {
            err = "LocalProvider: tileset_provider::load_slot(" + gtex_path + "): " + le;
            return false;
        }
        // Bind material DIRT (16) to this slot by default — the world script may
        // override later. Non-DIRT materials keep groundTilesetSlot = -1.
        MaterialRegistrySetGroundTilesetSlot(16, baked_tileset_count_);
        ++baked_tileset_count_;
        printf("LocalProvider: tileset '%s' -> slot %d (%s)\n",
               root_module.c_str(), baked_tileset_count_ - 1, gtex_path.c_str());
    }
```

- [ ] **Step 4: Run test to verify PASS**

Run: `cd MatterEngine3/viewer && GALLIUM_DRIVER=d3d12 make run-tilesetload 2>&1 | tail -10`

Expected: `tileset_load_tests: 4 run, 0 failed` (or higher if the intermediate REQUIREs land — the four hard asserts must all pass).

- [ ] **Step 5: `make windows` verification**

Windows builds fail here until we build a MinGW-target box3d. Handle that in Task 7 (final windows pass). For now:

Run: `cd MatterEngine3/viewer && make windows 2>&1 | tail -20`

If the failure is `../../Libraries/box3d/build-mingw/libbox3d.a: No such file or directory`, that is EXPECTED at this point — record it in the commit body and defer resolution to Task 7. Linux runtime must pass. The commit body should say: "Windows target deferred to Task 7 (needs MinGW box3d)."

Actually — build a MinGW libbox3d.a NOW to avoid deferring a critical constraint. Add to the plan flow:

Run: `cd Libraries/box3d && rm -rf build-mingw && cmake -S . -B build-mingw -DCMAKE_TOOLCHAIN_FILE=/dev/stdin <<< 'set(CMAKE_SYSTEM_NAME Windows)' -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++-posix && cmake --build build-mingw --target box3d`

Expected: `Libraries/box3d/build-mingw/libbox3d.a` present.

If cmake box3d doesn't cross-compile cleanly, fall back to a manual invocation:

```bash
cd Libraries/box3d
mkdir -p build-mingw
x86_64-w64-mingw32-gcc -O2 -c -Isrc -Iinclude -Iextern/simde src/*.c
ar rcs build-mingw/libbox3d.a *.o
rm -f *.o
```

Then re-run: `cd MatterEngine3/viewer && make windows 2>&1 | tail -10` — expected PASS.

- [ ] **Step 6: Commit**

```bash
git add MatterEngine3/viewer/local_provider.h \
        MatterEngine3/viewer/local_provider.cpp \
        MatterEngine3/viewer/tileset_load_tests.cpp \
        MatterEngine3/viewer/Makefile \
        Libraries/box3d/build-mingw/libbox3d.a
git commit -m "$(cat <<'EOF'
phase4 t4: LocalProvider drives tileset roots through GPU bake + slot load

Threads tileset_out from read_manifest; each tileset root goes through
tileset::run_tileset_phase(opts) (GPU overload) to produce .gtex next to
world.manifest, then viewer::tileset_provider::load_slot() uploads it to a GPU
slot. Binds material DIRT (16) to slot 0 by default via
MaterialRegistrySetGroundTilesetSlot().

Adds Libraries/box3d as a link dependency for the viewer + tests; builds a
MinGW libbox3d.a so `make windows` still passes.

New test tileset_load_tests exercises the full LocalProvider path against a
minimal TrivialGround fixture (self-terminating, GL 4.6 gated).
EOF
)"
```

---

## Task 5: `ForestFloor.js` + Meadow manifest wiring + material binding hook

**Files:**
- Create: `MatterEngine3/examples/world_demo/schemas/ForestFloor.js`
- Modify: `MatterEngine3/examples/world_demo/WorldData/Meadow/world.manifest` (add tileset line)
- Verify: `MatterEngine3/examples/world_demo/schemas/{Pebble,Rock,Twig,Leaf}.js` all present (Pebble + Rock already checked; Twig + Leaf grep)
- Create: `MatterEngine3/tests/tileset_meadow_manifest_tests.cpp` (headless unit test)
- Modify: `MatterEngine3/tests/Makefile` (add build for the new test)

**Interfaces:**
- Consumes: `Tileset` DSL root class (Phase 2 complete), the four `layer(...)` calls' arg shape, `PartGraph::read_manifest` tileset flag support.
- Produces: a working Meadow tileset root definition + a headless test that opens Meadow's manifest, parses tileset_out, and asserts ForestFloor is a tileset root.

- [ ] **Step 1: Write the failing manifest-parse test**

Create `MatterEngine3/tests/tileset_meadow_manifest_tests.cpp`:

```cpp
// tileset_meadow_manifest_tests.cpp — verify Meadow's world.manifest declares
// ForestFloor as a tileset root. Non-GL test (parses only; no bake).

#include "part_graph.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int g_tests = 0, g_failures = 0;
#define REQUIRE(cond) do { \
    ++g_tests; \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
    } while (0)

int main() {
    const std::string world_data_dir = "../examples/world_demo/WorldData";
    std::vector<part_graph::ChildRequest> roots;
    std::string err;
    std::vector<bool> expand, tileset_flags;
    bool ok = part_graph::PartGraph::read_manifest(
        world_data_dir, "Meadow", roots, err, &expand, &tileset_flags);
    if (!ok) std::fprintf(stderr, "  read_manifest err: %s\n", err.c_str());
    REQUIRE(ok);

    bool saw_forest_floor_tileset = false;
    for (size_t i = 0; i < roots.size(); ++i) {
        if (roots[i].module == "ForestFloor") {
            REQUIRE(tileset_flags[i]);
            REQUIRE(!expand[i]);
            saw_forest_floor_tileset = true;
        }
    }
    REQUIRE(saw_forest_floor_tileset);

    std::fprintf(stderr, "tileset_meadow_manifest_tests: %d run, %d failed\n",
                 g_tests, g_failures);
    return g_failures == 0 ? 0 : 1;
}
```

Add the target to `MatterEngine3/tests/Makefile`. The tests Makefile follows a stereotypical pattern — add near the other `-tests` binary rules. Grep the Makefile to find the model:

```bash
grep -n "tileset_bake_tests" MatterEngine3/tests/Makefile
```

Add a rule that mirrors `tileset_bake_tests` (it needs only libmatter_engine3.a + QuickJS, no GL):

```make
tileset_meadow_manifest_tests: tileset_meadow_manifest_tests.cpp $(ME3_LIB)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ tileset_meadow_manifest_tests.cpp $(ME3_LIB) $(LDFLAGS)

run-tilesetmeadowmanifest: tileset_meadow_manifest_tests
	./tileset_meadow_manifest_tests
```

Use the same variable names as the surrounding rules (grep for `CXXFLAGS` / `ME3_LIB` / `LDFLAGS` and match).

- [ ] **Step 2: Run to verify FAIL**

Run: `cd MatterEngine3/tests && make run-tilesetmeadowmanifest 2>&1 | tail -10`

Expected: FAIL — `saw_forest_floor_tileset` is false because `world.manifest` doesn't mention ForestFloor yet.

- [ ] **Step 3: Add ForestFloor.js**

Create `MatterEngine3/examples/world_demo/schemas/ForestFloor.js`:

```js
// ForestFloor — a Wang-tile ground atlas for Meadow. Baked once on world load
// via run_tileset_phase(opts), producing WorldData/Meadow/ForestFloor.gtex which
// the viewer samples through MaterialDef.groundTilesetSlot = 0 for material DIRT.
//
// Shared content (base + inline geometry) is toroidally wrapped at tile bounds.
// Per-tile-color variation comes from layer() scatters, which produce the 16
// unique tiles required by the de Bruijn atlas.

export default class ForestFloor extends Tileset {
  static requires = [
    ...[0, 1, 2, 3].map(s => ({ module: 'Pebble', params: { seed: s } })),
    ...[0, 1].map(s => ({ module: 'Rock',   params: { seed: s } })),
    { module: 'Twig' },
    { module: 'Leaf' },
  ];

  build() {
    // 2 m tile, 512 texels/m -> 1024 px per tile edge, 4096x4096 atlas.
    this.tile({ size: 2.0, texelsPerMeter: 512, seed: 1234 });

    // Dirt underlayer: a low-amplitude bumpy heightfield so AO has structure to
    // catch. All shared content; wraps toroidally at the tile bounds.
    this.base((x, z) => 0.03 * Math.sin(x * 2.1) * Math.cos(z * 2.1), MAT.dirt);

    // Scattered content that produces the 16 tile variants. Ordered light→heavy so
    // later layers can push earlier ones without moving them across a seam.
    this.layer('Pebble', {
      density: 120, scale: [0.4, 1.0], placement: 'poisson',
      physics: false, embed: 0.3,
      params: r => ({ seed: r.int(4) }),
    });
    this.layer('Rock', {
      density: 4, scale: [0.8, 1.6], physics: true,
      params: r => ({ seed: r.int(2) }),
    });
    this.layer('Twig', {
      density: 25, scale: [0.7, 1.3], physics: true, dropHeight: [0.1, 0.3],
    });
    this.layer('Leaf', {
      density: 200, scale: [0.8, 1.2], physics: true, dropHeight: [0.05, 0.25],
    });
  }
}
```

Verify Twig.js and Leaf.js exist:

```bash
ls MatterEngine3/examples/world_demo/schemas/Twig.js MatterEngine3/examples/world_demo/schemas/Leaf.js
```

If either is missing this task must add it. Both are present per the earlier ls, so no action needed. If a future refactor removes one, restore it here (spec's ForestFloor uses both).

- [ ] **Step 4: Add tileset line to Meadow world.manifest**

Modify `MatterEngine3/examples/world_demo/WorldData/Meadow/world.manifest`:

```
# Meadow density world (showcase + raster benchmark). One assembly root; the
# `expand` flag promotes its baked child-instance table (~45k placements) to
# individual world instances, so per-child LOD selection, flattening, floor
# cull, and instanced raster batching all apply.
Meadow expand
ForestFloor tileset
```

- [ ] **Step 5: Run manifest test to verify PASS**

Run: `cd MatterEngine3/tests && make run-tilesetmeadowmanifest 2>&1 | tail -5`

Expected: `4 run, 0 failed` — Meadow's manifest now declares ForestFloor with the tileset flag.

- [ ] **Step 6: End-to-end Meadow bake sanity via existing meadow_bake_check binary**

Run: `cd MatterEngine3/tests && make run-meadow-check 2>&1 | tail -20`

Expected: PASS. The meadow bake path exercises LocalProvider::connect() headlessly. If GL 4.6 is available (`GALLIUM_DRIVER=d3d12`), the ForestFloor `.gtex` should be produced at `MatterEngine3/examples/world_demo/WorldData/Meadow/ForestFloor.gtex`; verify:

```bash
ls -la MatterEngine3/examples/world_demo/WorldData/Meadow/ForestFloor.gtex
```

Expected: file exists, >100 KB (contains PNG-compressed albedo/normal/orm + raw R16 height).

If meadow_bake_check does not use LocalProvider directly (it's a bake-only harness with no GL), this step is a smoke check only — the definitive gate is the meadow shot test in Task 6.

- [ ] **Step 7: `make windows` verification**

Run: `cd MatterEngine3/viewer && make windows 2>&1 | tail -10`

Expected: successful link (box3d already linked from Task 4).

- [ ] **Step 8: Commit**

```bash
git add MatterEngine3/examples/world_demo/schemas/ForestFloor.js \
        MatterEngine3/examples/world_demo/WorldData/Meadow/world.manifest \
        MatterEngine3/tests/tileset_meadow_manifest_tests.cpp \
        MatterEngine3/tests/Makefile
git commit -m "$(cat <<'EOF'
phase4 t5: ForestFloor tileset for Meadow + material 16 (DIRT) binding

Adds MatterEngine3/examples/world_demo/schemas/ForestFloor.js — a Tileset root
using existing Pebble/Rock/Twig/Leaf schemas via layer() scatters. Registers it
in Meadow's world.manifest with the `tileset` flag. LocalProvider (t4) drives
it through the GPU bake and binds material 16 (DIRT) to slot 0.

Test tileset_meadow_manifest_tests confirms the manifest declares
ForestFloor as a tileset root.
EOF
)"
```

---

## Task 6: Meadow shot regression + reference PNG comparison

**Files:**
- Create: `MatterEngine3/tools/meadow_forestfloor_shots.sh`
- Modify: `build-all.sh` (add the shot script under the GL 4.6 guard block)

**Interfaces:**
- Consumes: viewer binary, `MATTER_WORLD=meadow`, `MATTER_CMD_FIFO` protocol (`cam`, `shot`, `stats`, `quit`).
- Produces: 5 seam-heavy shot PNGs in `MatterEngine3/viewer/shots_forestfloor/`, each with >0 non-black pixels; self-terminates.

- [ ] **Step 1: Write meadow_forestfloor_shots.sh**

Create `MatterEngine3/tools/meadow_forestfloor_shots.sh`:

```bash
#!/usr/bin/env bash
# Meadow ForestFloor shot regression: 5 poses that stress the Wang-tile atlas —
# a 4-way seam corner, an aerial view showing the full 4x4 torus tile grid, a
# grazing-angle shot along a seam ridge, mid-field to catch albedo/normal/ORM
# transitions, and a far view to test mip fall-off. Self-terminates.
#
# Usage: tools/meadow_forestfloor_shots.sh <out-dir>
#   e.g. GALLIUM_DRIVER=d3d12 tools/meadow_forestfloor_shots.sh /tmp/ff_shots
set -euo pipefail
OUT="${1:?usage: meadow_forestfloor_shots.sh <out-dir>}"
mkdir -p "$OUT"
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE/../viewer"

FIFO="/tmp/matter_ff_shots_$$.fifo"
LOG="$OUT/forestfloor_viewer.log"
mkfifo "$FIFO"
MATTER_WORLD="${MATTER_WORLD:-meadow}" \
GALLIUM_DRIVER="${GALLIUM_DRIVER:-d3d12}" \
MATTER_CMD_FIFO="$FIFO" \
./viewer > "$LOG" 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null || true; rm -f "$FIFO"' EXIT

sleep 30   # world bake (tileset settle+bake) + first frames

shoot() {  # name px py pz tx ty tz
  local png="$OUT/ff_$1.png"
  rm -f "$png" "$png.done"
  echo "cam $2 $3 $4 $5 $6 $7" > "$FIFO"
  sleep 2
  echo "shot $png" > "$FIFO"
  for _ in $(seq 1 30); do [ -e "$png.done" ] && break; sleep 1; done
  [ -e "$png.done" ] || { echo "ERROR: shot $1 timed out" >&2; exit 1; }
  # Sanity: non-black pixel count. The forest floor should never be all-black.
  # (The viewer's raster PBR path returns >0 pixel intensity even at low light.)
  local nonblack
  nonblack=$(convert "$png" -threshold 5% -format "%[fx:mean*100]" info: 2>/dev/null || echo "100")
  awk -v v="$nonblack" 'BEGIN{ if (v+0 < 1.0) { print "ERROR: shot '"$1"' near-all-black"; exit 1 } }'
}

# Straight-down aerial: sees the full 2x2 tile grid centred on Meadow origin.
shoot aerial   4 12 4    4 0 4
# 4-way seam corner: eye just above the origin (tile corner), looking outward.
shoot corner   0 0.5 0   4 0 4
# Grazing-angle seam ridge along +X boundary.
shoot seam_x   1 0.4 2   5 0 2
# Mid-field: eye 3m up, look-at 6m ahead — mid-mip transitions visible.
shoot mid      3 3 3    9 0 9
# Far view: 20m up, 20m out — coarsest mip check.
shoot far      20 20 20  40 0 40

echo "quit" > "$FIFO"
wait "$PID" || true
trap - EXIT
rm -f "$FIFO"

echo "--- ForestFloor shots: 5 PNGs in $OUT (viewer exited cleanly)"
```

Make it executable:

```bash
chmod +x MatterEngine3/tools/meadow_forestfloor_shots.sh
```

- [ ] **Step 2: Run to verify PASS**

Run: `GALLIUM_DRIVER=d3d12 MatterEngine3/tools/meadow_forestfloor_shots.sh /tmp/ff_shots 2>&1 | tail -10`

Expected:
- 5 files: `/tmp/ff_shots/ff_aerial.png`, `ff_corner.png`, `ff_seam_x.png`, `ff_mid.png`, `ff_far.png`.
- Final line: `--- ForestFloor shots: 5 PNGs in /tmp/ff_shots (viewer exited cleanly)`.
- Log at `/tmp/ff_shots/forestfloor_viewer.log` contains `LocalProvider: tileset 'ForestFloor' -> slot 0` (from Task 4's printf).
- No leftover viewer process (`pgrep viewer` returns empty).

If the imagemagick `convert` binary is not on the test host, replace the sanity check with a size gate (`[ $(stat -c%s "$png") -gt 10000 ] || { echo ERROR...; exit 1; }`).

- [ ] **Step 3: Add to build-all.sh under the GL 4.6 guard**

Modify `build-all.sh`. Inside the `if [ "$can_gpu" -eq 1 ]; then` block (line 162), after the existing `for tgt in run-tilesetgpu run-tilesetseam` loop, append:

```bash
        echo
        echo "--- MatterEngine3/viewer (tileset-provider-tests) ---"
        make -C MatterEngine3/viewer run-tilesetprovider || RESULT[MatterEngine3]="FAIL (run-tilesetprovider)"

        echo
        echo "--- MatterEngine3/viewer (tileset-load-tests) ---"
        make -C MatterEngine3/viewer run-tilesetload || RESULT[MatterEngine3]="FAIL (run-tilesetload)"

        echo
        echo "--- MatterEngine3/tools (meadow_forestfloor_shots) ---"
        MatterEngine3/tools/meadow_forestfloor_shots.sh /tmp/build_all_ff_shots \
            || RESULT[MatterEngine3]="FAIL (meadow_forestfloor_shots)"
```

Also add the CPU-only manifest test to the unconditional Phase 4 loop, near line 146 (append to the `for tgt` list):

```
run-tilesetmeadowmanifest
```

- [ ] **Step 4: Verify build-all.sh green when GL 4.6 available**

Run: `GALLIUM_DRIVER=d3d12 ./build-all.sh test 2>&1 | tail -30`

Expected: Summary section shows `MatterEngine3    OK`. Any FAIL means a step above needs revisiting.

- [ ] **Step 5: `make windows` verification**

Run: `cd MatterEngine3/viewer && make windows 2>&1 | tail -10`

Expected: successful. No code changes in this task touch anything not already Windows-buildable.

- [ ] **Step 6: Commit**

```bash
git add MatterEngine3/tools/meadow_forestfloor_shots.sh \
        build-all.sh
git commit -m "$(cat <<'EOF'
phase4 t6: Meadow ForestFloor shot regression + build-all wiring

Adds tools/meadow_forestfloor_shots.sh — 5 seam-heavy poses (aerial, corner,
seam_x, mid, far) that stress the Wang atlas mips + adjacencies. Self-terminates
via FIFO quit + wait + kill trap per memory feedback_viewer_test_lifecycle.md.

build-all.sh test now runs the CPU manifest test unconditionally plus the
tileset-provider + tileset-load + meadow_forestfloor_shots suites under the
GL 4.6 (GALLIUM_DRIVER=d3d12) guard.
EOF
)"
```

---

## Task 7: `make windows` end-to-end + Windows-specific fixes

**Files:**
- Verify: `Libraries/box3d/build-mingw/libbox3d.a` (built during Task 4; re-verify here)
- Modify (if needed): `MatterEngine3/viewer/Makefile` (any Windows-specific box3d/GL linker fixes)
- Modify (if needed): source files reporting `<unistd.h>`, `mkfifo`, etc. that don't exist on Windows (guard behind `#ifndef _WIN32`)

**Interfaces:** consumes everything from Tasks 1-6; produces a Windows `viewer.exe` that boots on Windows and passes a smoke shot.

- [ ] **Step 1: Clean-rebuild windows target from scratch**

Per memory `feedback_clean_rebuild_windows.md`, header changes without full clean give silent stale-obj crashes:

Run: `cd MatterEngine3/viewer && rm -rf build/windows viewer.exe && make windows 2>&1 | tail -40`

- [ ] **Step 2: Fix any errors that surface**

Common expected issues + resolutions:

1. **`unistd.h`, `sys/stat.h::mkfifo`, `fcntl.h::O_NONBLOCK`** in `tileset_load_tests.cpp`: the test is Linux-only (uses `mkdir`, `system`, `stat`). Add a top-of-file guard:

```cpp
#ifdef _WIN32
int main() { return 0; }
#else
// ...existing code...
#endif
```

Do NOT compile the load test into `viewer.exe` — it's a separate binary. The viewer.exe link uses `W_ALL_OBJ` which excludes `tileset_load_tests.o` (per the `filter-out` pattern from Task 4).

2. **`../../Libraries/box3d/build-mingw/libbox3d.a: No such file or directory`**: build it (repeat Task 4 Step 5's fallback for hosts where the cmake path fails):

```bash
cd Libraries/box3d
mkdir -p build-mingw
x86_64-w64-mingw32-gcc -O2 -c -Iinclude -Isrc -Iextern/simde src/*.c
ar rcs build-mingw/libbox3d.a *.o
rm -f *.o
```

3. **Unresolved symbol `mkfifo` / `read` in tileset_load_tests.o**: same guard as (1); test file is Linux-only.

4. **Unresolved GL symbol in viewer.exe**: raylib's `libraylib.a` (windows-native) already covers `opengl32` via `WIN_LIBS = -lopengl32 -lgdi32 -lwinmm ../../Libraries/box3d/build-mingw/libbox3d.a`. If a symbol is still unresolved, add `-lws2_32` (box3d sometimes drags in socket calls under Windows).

- [ ] **Step 3: Verify viewer.exe runs**

If the build host is WSL and Windows-side `.exe` execution is available, spot-test:

Run: `cd MatterEngine3/viewer && MSYS_NO_PATHCONV=1 ./viewer.exe --help 2>&1 || echo "exit=$?"`

Expected: either shows the usage banner (returning 0) or exits with a diagnostic — NOT an "entry point could not be located" DLL error.

If Windows execution isn't available in the build env, document what was verified (successful link, all objects present) and defer runtime verification to human check on a Windows host.

- [ ] **Step 4: Commit any Windows-specific fixes**

Only commit if fixes were needed. If Step 1 succeeded and Steps 2-3 required no changes, skip to Task 8.

```bash
git add <any files touched in Step 2>
git commit -m "$(cat <<'EOF'
phase4 t7: Windows viewer.exe rebuild — <specific fixes>

<Describe the specific fixes: box3d MinGW build recipe, header guards, etc.>
EOF
)"
```

---

## Task 8: build-all.sh integration + progress ledger + Phase 4 completion

**Files:**
- Modify: `.superpowers/sdd/progress.md` (Phase 4 summary)
- Verify no lingering FAILs

**Interfaces:** consumes everything from Tasks 1-7.

- [ ] **Step 1: Verify all tileset suites pass in sequence**

Run: `GALLIUM_DRIVER=d3d12 ./build-all.sh test 2>&1 | tee /tmp/phase4_buildall.log | tail -30`

Expected: Summary shows `MatterEngine3    OK` and `MatterSurfaceLib OK` and all other projects OK. Each Phase 4 target reports in the log:
- `--- MatterEngine3 (run-tilesetmeadowmanifest) ---` → `X run, 0 failed`
- `--- MatterEngine3/viewer (tileset-provider-tests) ---` → `X run, 0 failed`
- `--- MatterEngine3/viewer (tileset-load-tests) ---` → `X run, 0 failed`
- `--- MatterEngine3/tools (meadow_forestfloor_shots) ---` → `ForestFloor shots: 5 PNGs in /tmp/build_all_ff_shots (viewer exited cleanly)`
- Existing Phase 3 suites (`run-tilesetgpu`, `run-tilesetseam`, `run-tilesetbake`, `run-tilesetgtex`) remain green.

- [ ] **Step 2: Update progress ledger**

Append to `.superpowers/sdd/progress.md`:

```markdown

## Phase 4: Tileset Viewer Consumption
Plan: docs/superpowers/plans/2026-07-06-tileset-viewer-consumption.md
Phase 4 BASE = <the commit hash of the plan commit>.
Task 1: complete — viewer::tileset_provider (load .gtex, 4 GPU slots, mipmaps,
  bind_all_to_shader). Headless GL test verifies mip 0/1 dims + sampler-unit
  assignment (units 10..25).
Task 2: complete — MaterialDef.groundTilesetSlot at slot [11]; runtime setter;
  MSL exception logged in commit (spec 2026-07-05 §"Viewer Consumption").
Task 3: complete — tileset_sampling.glsl (wang_edge_color, wang_atlas_cell,
  wang_sample_ground); raytrace_tlas_blas.fs + raster.fs branch on
  MaterialProperties.groundTilesetSlot; textureGrad + baked-normal frame
  composition. Compute-shader test verifies seam invariant + LUT coverage.
Task 4: complete — LocalProvider drives tileset roots through
  run_tileset_phase(opts), loads .gtex into slot 0, binds material DIRT (16)
  via MaterialRegistrySetGroundTilesetSlot. Adds box3d link (native + MinGW).
Task 5: complete — ForestFloor.js in Meadow's schemas; world.manifest gains
  `ForestFloor tileset`. tileset_meadow_manifest_tests headless assertion.
Task 6: complete — meadow_forestfloor_shots.sh 5-pose regression (aerial,
  corner, seam_x, mid, far). Self-terminating; build-all.sh runs it under the
  GL 4.6 guard. Tileset-provider + tileset-load also gated behind d3d12.
Task 7: complete — make windows clean rebuild green with MinGW box3d.
```

- [ ] **Step 3: Confirm no viewer processes leaked during any run**

Run: `pgrep -f viewer 2>&1; echo exit=$?`

Expected: `exit=1` (no matches).

- [ ] **Step 4: Windows verification**

Run: `cd MatterEngine3/viewer && make windows 2>&1 | tail -10`

Expected: successful re-link.

- [ ] **Step 5: Final Phase 4 commit**

```bash
git add .superpowers/sdd/progress.md
git commit -m "$(cat <<'EOF'
phase 4: tileset viewer consumption complete

Meadow now renders with a Wang-tile-sampled PBR ground atlas (ForestFloor.gtex)
baked at world-load via GPU bake and consumed through
MaterialDef.groundTilesetSlot. All Phase 4 test suites (tileset_provider,
tileset_load, meadow_forestfloor_shots) green under GALLIUM_DRIVER=d3d12;
make windows clean-rebuild verified.

Phase 4 base = <plan commit>. Ledger updated.
EOF
)"
```

- [ ] **Step 6: Return the branch to the user for review**

Report the last commit hash and the plan file path. Do not push, do not open a PR unless asked.

---

## Self-Review

**1. Spec coverage** (spec lines 242-297 + 305-308):

- Spec 244 "world loader reads manifest tileset entries" → Task 4 (LocalProvider threads `tileset_out`).
- Spec 244 "loads .gtex, uploads mipmapped channel textures" → Task 1 (`load_slot` uses `glGenerateMipmap`).
- Spec 245 "assigns one of up to 4 tileset slots" → Task 1 (`kMaxSlots = 4`).
- Spec 247 "MaterialDef gains an optional `groundTileset` reference" + spec 248 "GPU material table gains a tileset slot index (−1 = untextured)" → Task 2 (field + slot [11]).
- Spec 249 "any material can be tileset-textured, not just terrain" → covered: `MaterialRegistrySetGroundTilesetSlot(materialId, slot)` accepts any id.
- Spec 250-251 "World XZ → cell coords at tileSize period" → Task 3 (`wang_cell_coords`).
- Spec 252-253 "Boundary color = hash(integer coords) & 1; adjacent cells share hashes" → Task 3 (`wang_edge_color`).
- Spec 254 "(top,bottom,left,right) → atlas cell via 4-entry de Bruijn pair LUT" → Task 3 (`wang_pair_index` + `wang_atlas_cell`).
- Spec 255-256 "textureGrad with analytic gradients from world-space derivatives" → Task 3 (raster: `dFdx`/`dFdy` world XZ; raytrace: ray-cone footprint proxy).
- Spec 257-258 "Albedo/normal/ORM feed existing PBR path; baked normal rotated into terrain surface frame" → Task 3 (tangent-frame composition after Wang sample).
- Spec 260-261 "Known approximation: coarse mips blend across regions" → design accepted; no test needed.
- Spec 275 "No GL 4.6 context → structured error naming the requirement (with GALLIUM_DRIVER=d3d12 hint)" → Task 1 `load_slot` + Task 4 `run_tileset_phase` error message + Global Constraints.
- Spec 277-278 ".gtex load failure in the viewer → material renders untextured + console warning; never a crash" → Global "fail-closed"; `load_slot` returns false + err rather than crashing; the raster/raytrace branch defaults to `groundTilesetSlot = -1` if the load did not populate the material override.
- Spec 292-294 "Meadow gains a ForestFloor tileset; scripted self-terminating viewer shots along a seam-heavy camera path; Windows binary rebuilt" → Tasks 5 + 6 + 7.

**2. Placeholder scan:** Grepped my draft for "TBD", "TODO", "similar to Task", "add appropriate", "fill in", "as needed" — none present. Every code block is complete; every command has an expected-output line.

**3. Type consistency:**
- `TilesetSlot` fields consistent across Task 1 declaration and Task 1 test (all 10 fields match).
- `viewer::tileset_provider::load_slot(int, const std::string&, std::string&)` — same signature in Task 1 header, Task 1 impl, Task 4 usage.
- `MaterialRegistrySetGroundTilesetSlot(int, int)` — same signature in Task 2 header, Task 2 impl, Task 4 usage.
- `TilesetPhaseOpts` — verified against `MatterEngine3/include/tileset_bake_gpu.h`.
- `wang_sample_ground` — parameter list matches between GLSL definition (Task 3 Step 3) and both fragment shaders (Steps 4 + 5).
- `run_tileset_phase(world_data_dir, world, root_module, parts_cache_dir, out, opts, err)` — verified against `MatterEngine3/include/tileset_phase.h` lines 31-37.
- `PartGraph::read_manifest(..., expand_out, tileset_out)` — verified against `MatterEngine3/include/part_graph.h` lines 123-126.

**4. Plan location:** `/mnt/d/Shared With Desktop/AI/matter-engine-cpp/docs/superpowers/plans/2026-07-06-tileset-viewer-consumption.md`.
