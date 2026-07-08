# Phase A: Kernel Extraction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split MatterEngine3 into an engine-kernel library (world/bake/render behind a `matter::` public API) and a top-level MatterViewer app that owns the window, GL 4.6 context, and frame loop — with pixel-identical Meadow output.

**Architecture:** Facade-first. Build `matter::EngineContext`/`WorldSession` over the code where it sits today, convert the viewer to the API, gate on screenshot diffs, then do pure `git mv` commits last. Spec: `docs/superpowers/specs/2026-07-07-phase-a-kernel-extraction-design.md` (read it before starting any task).

**Tech Stack:** C++17, Make, raylib (POD math types allowed in the public API; no raylib window/frame calls inside the kernel), OpenGL 4.6 compute, Python 3 for codegen/diff tools.

## Global Constraints

- **Every GPU/viewer run needs `GALLIUM_DRIVER=d3d12`** in the environment (WSLg; without it Mesa falls back to llvmpipe GL 4.5 and the viewer FATALs).
- **MatterSurfaceLib is read-only.** Do not edit anything under `MatterSurfaceLib/` (genuine bug fixes only, and surface them as a scope decision in your report).
- **Scripted viewer runs must self-terminate.** Use `MatterEngine3/tools/viewer_shots.sh`; never leave a viewer window running when a task ends.
- **The `STATS,` printf line format is append-only** — scripts parse fields by position. Never reorder or remove fields.
- **No parallel scaffolding.** Extend the real pipeline; no demo/preview binaries.
- **Windows target:** after any header/struct change that lands in the Windows build, the verify task does a full clean rebuild (`rm -rf` of the windows obj dir first). Individual tasks only need the Linux build unless stated.
- Working directory notes: the viewer binary runs with cwd = its own directory (`MatterEngine3/viewer` before Task 8, `MatterViewer/` after). Relative paths like `../examples`, `cache`, `shaders/…` resolve from there.
- All paths below are relative to the repo root unless absolute.
- Commit after every task (specific paths only, never `git add -A` — other agents may share history).

## Interface Reference (used across tasks)

Facts a task implementer needs but cannot see from their own task alone:

- `viewer::LocalProviderConfig` (`MatterEngine3/viewer/local_provider.h`): fields `schemas_dir, world_data_dir, world_name, shared_lib_dir, cache_root` (std::string). Task 5 adds `on_part` callback.
- `viewer::WorldProvider` (`MatterEngine3/viewer/world_source.h`): `connect(WorldManifest&, std::string& err)`, `reconcile(manifest, PartStore&) -> want list`, `fetch_parts(want, PartStore&, err)`, `poll_deltas(WorldDelta&)`. `WorldManifest{world_root_hash, instances, lights, probes}`, `WorldManifestEntry{instance_id, part_hash, transform[16] row-major}`.
- `world_tracer::WorldTracer` (`MatterEngine3/include/world_tracer.h`): `build(cache_root, vector<TraceInstance>, err)`, `trace(origin[3], dir[3], max_t, Hit&)`. `Hit{t, normal[3], material_id, emission, albedo[3]}` — Task 7 extends it with instance identity.
- `matter::shader_text` (created Task 2): `bool matter::shader_text(const char* logical_path, std::string& out, std::string& err)` in `MatterEngine3/include/shader_source.h`. Logical paths are exactly the strings used today: `"shaders/raster.fs"`, `"shaders_gpu/cull.comp"`, etc.
- Public API headers (created Task 4): `MatterEngine3/include/matter/{engine_context,world_session,events,query}.h` — full code in Task 4; Tasks 5–7 implement against them verbatim.
- The facade implementation lives at `MatterEngine3/viewer/matter_engine.cpp` until Task 8 moves it to `MatterEngine3/src/`.
- `MatterEngine3/viewer/main.cpp` (pre-conversion) is the reference recipe for the facade: the `connect_sequence` lambda is lines ~170–264, the per-frame resolve/cull/draw block lines ~392–457. Task 5 relocates that logic; main.cpp itself is only rewritten in Task 6.

---

### Task 1: Stage 0 — reference screenshots + image-diff tool

**Files:**
- Create: `MatterEngine3/tools/img_diff.py`
- Output (not committed): `/home/jkern/phase-a-refs/ref_{aerial,corner,midfield,far,empty}.png`, `/home/jkern/phase-a-refs/ref_stats.log`

**Interfaces:**
- Produces: `img_diff.py A.png B.png` → exit 0 if images match (≤0.5% of pixels differ by >2/255 per channel), exit 1 + summary line otherwise. Tasks 3, 6, 8–10 run it against these reference PNGs.

- [ ] **Step 1: Write the diff tool**

```python
#!/usr/bin/env python3
"""img_diff.py A.png B.png [--max-diff-pct 0.5] — exit 0 iff images match."""
import sys, argparse

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a"); ap.add_argument("b")
    ap.add_argument("--max-diff-pct", type=float, default=0.5)
    ap.add_argument("--channel-tol", type=int, default=2)
    args = ap.parse_args()
    try:
        from PIL import Image
    except ImportError:
        sys.exit("img_diff: pip3 install --user pillow")
    ia, ib = Image.open(args.a).convert("RGB"), Image.open(args.b).convert("RGB")
    if ia.size != ib.size:
        print(f"DIFF size {ia.size} vs {ib.size}"); sys.exit(1)
    pa, pb = ia.tobytes(), ib.tobytes()
    npx = ia.size[0] * ia.size[1]
    bad = 0
    for i in range(0, len(pa), 3):
        if (abs(pa[i]-pb[i]) > args.channel_tol or
            abs(pa[i+1]-pb[i+1]) > args.channel_tol or
            abs(pa[i+2]-pb[i+2]) > args.channel_tol):
            bad += 1
    pct = 100.0 * bad / npx
    print(f"{'DIFF' if pct > args.max_diff_pct else 'MATCH'} "
          f"{bad}/{npx} px ({pct:.3f}%) exceed tol {args.channel_tol}")
    sys.exit(0 if pct <= args.max_diff_pct else 1)

if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Make it executable, verify PIL is available**

Run: `chmod +x MatterEngine3/tools/img_diff.py && python3 -c "import PIL; print('pil ok')"`
If PIL missing: `pip3 install --user pillow`.

- [ ] **Step 3: Self-test the tool**

Run: `python3 MatterEngine3/tools/img_diff.py <any png> <same png>` (use any PNG in the repo, e.g. one under `Libraries/`). Expected: `MATCH … (0.000%)`, exit 0.

- [ ] **Step 4: Build the viewer**

Run: `make -C MatterEngine3/viewer -j$(nproc)` — expected: `viewer` binary, no errors.

- [ ] **Step 5: Capture the reference set**

```bash
mkdir -p /home/jkern/phase-a-refs
GALLIUM_DRIVER=d3d12 MatterEngine3/tools/viewer_shots.sh ref /home/jkern/phase-a-refs
```

The script bakes Meadow (~25 s sleep built in), takes the 5 standard poses, writes `ref_<pose>.png` + `ref_stats.log`, and self-terminates.

- [ ] **Step 6: Verify outputs**

Run: `ls /home/jkern/phase-a-refs/` — expected: 5 PNGs + `ref_stats.log`; `grep -c '^STATS,' /home/jkern/phase-a-refs/ref_stats.log` ≥ 5. Open one PNG (Read tool) and confirm it is not black/empty.

- [ ] **Step 7: Commit**

```bash
git add MatterEngine3/tools/img_diff.py
git commit -m "feat(phase-a): image diff tool for the screenshot exit gate (Stage 0)"
```

---

### Task 2: Stage 1a — shader embedding codegen + `matter::shader_text`

**Files:**
- Create: `MatterEngine3/tools/embed_shaders.py`
- Create: `MatterEngine3/include/shader_source.h`
- Create: `MatterEngine3/src/shader_source.cpp`
- Create: `MatterEngine3/tests/shader_source_tests.cpp`
- Modify: `MatterEngine3/Makefile` (codegen rule + new source + test target), `MatterEngine3/viewer/Makefile` (codegen dependency), `MatterEngine3/.gitignore` (ignore `shaders_gen/`)

**Interfaces:**
- Produces: `bool matter::shader_text(const char* logical_path, std::string& out, std::string& err)` and `void matter::set_shader_override_dir(const char* dir_or_null)`. Lookup order: (1) `MATTER_SHADER_DIR` env, (2) override dir set via the setter, (3) embedded table. Logical paths are the literal strings used at today's load sites (e.g. `"shaders/raster.fs"`, `"shaders_gpu/cull.comp"`).
- Produces: generated header `MatterEngine3/shaders_gen/embedded_shaders.h` defining `matter_embedded::Entry { const char* path; const char* text; }` and `extern const Entry kEmbeddedShaders[]; extern const int kEmbeddedShaderCount;` (definitions in the header as `inline`).

**Embedded shader list** (source file → logical path). `MatterEngine3/viewer/shaders` is a **symlink** to `../../MatterSurfaceLib/shaders`; codegen reads through it (MSL stays untouched):

| source file (relative to `MatterEngine3/viewer/`) | logical path |
|---|---|
| `shaders/raster.vs` | `shaders/raster.vs` |
| `shaders/raster.fs` | `shaders/raster.fs` |
| `shaders/tileset_sampling.glsl` | `shaders/tileset_sampling.glsl` |
| `shaders/raytrace_tlas_blas_processed.fs` | `shaders/raytrace_tlas_blas_processed.fs` |
| `shaders_gpu/cull.comp` | `shaders_gpu/cull.comp` |
| `shaders_gpu/hiz_downsample.comp` | `shaders_gpu/hiz_downsample.comp` |
| `shaders_gpu/raster_gpu_driven.vs` | `shaders_gpu/raster_gpu_driven.vs` |
| `shaders_gpu/tileset_bake_primary.comp` | `shaders_gpu/tileset_bake_primary.comp` |
| `shaders_gpu/tileset_bake_ao.comp` | `shaders_gpu/tileset_bake_ao.comp` |

Before writing the codegen, `ls MatterEngine3/viewer/shaders/` and confirm the four `shaders/` files exist; if additional `.glsl` include files exist there (grep the `.fs`/`.comp` sources for `#include`), add them to the table too — runtime include-flatteners will request them through `shader_text` after Task 3.

- [ ] **Step 1: Write the codegen script**

```python
#!/usr/bin/env python3
"""embed_shaders.py <out_header> <base_dir> <logical_path>...
Reads <base_dir>/<logical_path> for each entry, emits a C++ header embedding
the text as escaped string literals keyed by logical path."""
import sys, os

def c_escape(text):
    out = []
    for line in text.split("\n"):
        e = line.replace("\\", "\\\\").replace('"', '\\"')
        out.append(f'"{e}\\n"')
    return "\n    ".join(out)

def main():
    out_path, base = sys.argv[1], sys.argv[2]
    entries = []
    for lp in sys.argv[3:]:
        src = os.path.join(base, lp)
        with open(src, "r") as f:
            entries.append((lp, f.read()))
    with open(out_path, "w") as f:
        f.write("// GENERATED by tools/embed_shaders.py — do not edit.\n")
        f.write("#pragma once\n\nnamespace matter_embedded {\n")
        f.write("struct Entry { const char* path; const char* text; };\n")
        for i, (lp, text) in enumerate(entries):
            f.write(f"inline const char* kText{i} =\n    {c_escape(text)};\n")
        f.write("inline const Entry kEmbeddedShaders[] = {\n")
        for i, (lp, _) in enumerate(entries):
            f.write(f'    {{ "{lp}", kText{i} }},\n')
        f.write("};\n")
        f.write(f"inline const int kEmbeddedShaderCount = {len(entries)};\n")
        f.write("} // namespace matter_embedded\n")

if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Write the runtime lookup header + impl**

`MatterEngine3/include/shader_source.h`:

```cpp
#pragma once
// Embedded-shader lookup with disk override. Internal kernel header (moves to
// src/ in the Stage 5 file moves).
#include <string>

namespace matter {

// Lookup order: MATTER_SHADER_DIR env, then override dir set below, then the
// embedded table. Returns false + err if the logical path is unknown everywhere.
bool shader_text(const char* logical_path, std::string& out, std::string& err);

// EngineDesc::shader_dir plumbs through here; nullptr clears the override.
void set_shader_override_dir(const char* dir_or_null);

} // namespace matter
```

`MatterEngine3/src/shader_source.cpp`:

```cpp
#include "shader_source.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../shaders_gen/embedded_shaders.h"

namespace matter {

static std::string g_override_dir;

void set_shader_override_dir(const char* dir_or_null) {
    g_override_dir = dir_or_null ? dir_or_null : "";
}

static bool read_file(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize((size_t)(n < 0 ? 0 : n));
    size_t rd = n > 0 ? fread(&out[0], 1, (size_t)n, f) : 0;
    fclose(f);
    out.resize(rd);
    return true;
}

bool shader_text(const char* logical_path, std::string& out, std::string& err) {
    if (const char* env = getenv("MATTER_SHADER_DIR")) {
        if (read_file(std::string(env) + "/" + logical_path, out)) return true;
    }
    if (!g_override_dir.empty()) {
        if (read_file(g_override_dir + "/" + logical_path, out)) return true;
    }
    for (int i = 0; i < matter_embedded::kEmbeddedShaderCount; ++i) {
        if (strcmp(matter_embedded::kEmbeddedShaders[i].path, logical_path) == 0) {
            out = matter_embedded::kEmbeddedShaders[i].text;
            return true;
        }
    }
    err = std::string("shader_text: unknown shader '") + logical_path +
          "' (not on disk override, not embedded)";
    return false;
}

} // namespace matter
```

- [ ] **Step 3: Wire codegen into the Makefiles**

Read `MatterEngine3/Makefile` and `MatterEngine3/viewer/Makefile` first; follow their existing style. Add to `MatterEngine3/Makefile`:

```makefile
SHADER_LOGICAL := shaders/raster.vs shaders/raster.fs shaders/tileset_sampling.glsl \
    shaders/raytrace_tlas_blas_processed.fs \
    shaders_gpu/cull.comp shaders_gpu/hiz_downsample.comp \
    shaders_gpu/raster_gpu_driven.vs shaders_gpu/tileset_bake_primary.comp \
    shaders_gpu/tileset_bake_ao.comp
SHADER_FILES := $(addprefix viewer/,$(SHADER_LOGICAL))

shaders_gen/embedded_shaders.h: tools/embed_shaders.py $(SHADER_FILES)
	mkdir -p shaders_gen
	python3 tools/embed_shaders.py $@ viewer $(SHADER_LOGICAL)
```

(extend `SHADER_LOGICAL` with any include files discovered in the pre-step). Make `shader_source.o` (add `src/shader_source.cpp` to the library source list) depend on the generated header; in `MatterEngine3/viewer/Makefile` add an order-only rule so any viewer build first runs `$(MAKE) -C .. shaders_gen/embedded_shaders.h`, and add `src/shader_source.cpp` to the engine sources the viewer compiles (`PIPELINE_CPP` list). Add `shaders_gen/` to `MatterEngine3/.gitignore` (create the gitignore entry if absent).

- [ ] **Step 4: Write the failing test**

`MatterEngine3/tests/shader_source_tests.cpp` — follow the harness style of an existing test in `MatterEngine3/tests/` (read one first; they are plain asserts or a tiny REQUIRE macro):

```cpp
#include "shader_source.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>

int main() {
    std::string text, err;
    // 1. embedded lookup works and matches the on-disk source
    assert(matter::shader_text("shaders_gpu/cull.comp", text, err));
    assert(text.find("layout") != std::string::npos);
    // 2. unknown path fails with a useful error
    std::string t2;
    assert(!matter::shader_text("shaders/nope.fs", t2, err));
    assert(err.find("nope.fs") != std::string::npos);
    // 3. override dir wins: write a marker file, point the override at it
    system("mkdir -p /tmp/shader_override_test/shaders_gpu");
    FILE* f = fopen("/tmp/shader_override_test/shaders_gpu/cull.comp", "w");
    fputs("// OVERRIDE MARKER\n", f); fclose(f);
    matter::set_shader_override_dir("/tmp/shader_override_test");
    std::string t3;
    assert(matter::shader_text("shaders_gpu/cull.comp", t3, err));
    assert(t3.find("OVERRIDE MARKER") != std::string::npos);
    matter::set_shader_override_dir(nullptr);
    printf("shader_source_tests: all passed\n");
    return 0;
}
```

Add a `shader-source-tests` target to the tests Makefile (read `MatterEngine3/tests/Makefile`, mirror an existing headless target; it needs `-I../include -I..` for the shaders_gen include and links `src/shader_source.o` or compiles the .cpp directly).

- [ ] **Step 5: Run test — expect failure first, then pass**

Before implementing: the target won't build (missing generated header) — that is the failing state. Then run `make -C MatterEngine3 shaders_gen/embedded_shaders.h` and the test target. Expected: `shader_source_tests: all passed`.

- [ ] **Step 6: Full builds stay green**

Run: `make -C MatterEngine3 -j$(nproc) && make -C MatterEngine3/viewer -j$(nproc)`. Expected: both succeed.

- [ ] **Step 7: Commit**

```bash
git add MatterEngine3/tools/embed_shaders.py MatterEngine3/include/shader_source.h \
    MatterEngine3/src/shader_source.cpp MatterEngine3/tests/shader_source_tests.cpp \
    MatterEngine3/Makefile MatterEngine3/viewer/Makefile MatterEngine3/tests/Makefile \
    MatterEngine3/.gitignore
git commit -m "feat(phase-a): embedded shader table + matter::shader_text with disk override (Stage 1a)"
```

---

### Task 3: Stage 1b — switch every shader load site to `shader_text`

**Files:**
- Modify: `MatterEngine3/viewer/raster_composer.cpp` (lines ~79, ~103–106, ~183–193)
- Modify: `MatterEngine3/viewer/gpu_culler.cpp` (lines ~105, ~890)
- Modify: `MatterEngine3/viewer/tileset_gl_ctx.cpp` (lines ~81, ~113, ~163)
- Modify: `MatterEngine3/src/tileset_bake_gpu.cpp` (lines ~143–145)
- Modify: `MatterEngine3/viewer/main.cpp` (line ~88, RT shader path)
- Modify: `MatterEngine3/viewer/tileset_gpu_tests.cpp` (lines ~186, ~349, ~446)
- Possibly modify: `MatterEngine3/viewer/renderer.cpp` (if `Renderer::init_shader` reads the file itself — check)

**Interfaces:**
- Consumes: `matter::shader_text` from Task 2.
- Produces: no shader is read from a cwd-relative `shaders/…` path anywhere in engine code; the viewer runs from any cwd.

- [ ] **Step 1: Enumerate load sites**

Run: `grep -rn '"shaders' MatterEngine3/viewer MatterEngine3/src --include='*.cpp'` and confirm the list above (line numbers may have drifted). Every hit must be converted.

- [ ] **Step 2: Convert direct reads**

Pattern — replace file reads with the lookup, preserving each site's existing error handling. Example for `raster_composer.cpp:103-106`:

```cpp
std::string vs_src, fs_raw, serr;
if (!matter::shader_text("shaders/raster.vs", vs_src, serr)) { err = serr; return false; }
if (!matter::shader_text("shaders/raster.fs", fs_raw, serr)) { err = serr; return false; }
```

For sites using raylib `LoadFileText` (raster_composer.cpp:183–193): fetch via `shader_text` into a `std::string` and use `.c_str()`; delete the `UnloadFileText` calls. For `compile_compute(path, err)`-style helpers (gpu_culler.cpp, tileset_bake_gpu.cpp `load_compute_source`, tileset_gl_ctx.cpp): change the helper's *file read* to `shader_text` and keep passing the same logical path strings. Add `#include "shader_source.h"` where needed (the include path `-I../include` / `-Iinclude` already reaches it).

- [ ] **Step 3: Convert include-flatteners**

`raster_composer.cpp:79` and `tileset_gl_ctx.cpp` resolve `#include "x.glsl"` lines by reading sibling files. Change the include resolution to request `"shaders/<name>"` (or the directory prefix of the parent logical path + name) through `shader_text`. Verify every include target is in the embedded table (Task 2 pre-step); if one is missing, add it to `SHADER_LOGICAL` and regenerate.

- [ ] **Step 4: Rebuild and run the GPU test suites**

```bash
make -C MatterEngine3/viewer -j$(nproc) gpu-tests tileset-gpu-tests
cd MatterEngine3/viewer && GALLIUM_DRIVER=d3d12 ./gpu_cull_tests && GALLIUM_DRIVER=d3d12 ./tileset_gpu_tests
```

(Read the viewer Makefile for the exact binary names/targets.) Expected: all pass.

- [ ] **Step 5: cwd-independence check**

Run the viewer from the repo root (not from `viewer/`) just long enough to pass shader init:

```bash
cd <repo-root> && GALLIUM_DRIVER=d3d12 MATTER_SCREENSHOT=/tmp/cwd_check.png ./MatterEngine3/viewer/viewer || true
```

Note: `../examples` and `cache` are still cwd-relative (fixed in Task 9), so world scan may fail from the root — the requirement here is only that **shader loading** no longer errors. If the world scan aborts first, instead verify by `MATTER_SHADER_DIR=/nonexistent` from `viewer/` still working (embedded fallback) — shader init succeeding without readable `shaders/` files proves the point: temporarily `mv shaders_gpu shaders_gpu.bak`, run one frame, restore.

- [ ] **Step 6: Screenshot gate**

```bash
GALLIUM_DRIVER=d3d12 MatterEngine3/tools/viewer_shots.sh embed /tmp/phase-a-embed
for p in aerial corner midfield far empty; do
  python3 MatterEngine3/tools/img_diff.py /home/jkern/phase-a-refs/ref_$p.png /tmp/phase-a-embed/embed_$p.png || exit 1
done
```

Expected: 5× MATCH.

- [ ] **Step 7: Commit**

```bash
git add MatterEngine3/viewer/*.cpp MatterEngine3/src/tileset_bake_gpu.cpp <plus any headers touched>
git commit -m "refactor(phase-a): all shader loads via matter::shader_text (Stage 1b)"
```

---

### Task 4: Stage 2a — public API headers `matter/*.h`

**Files:**
- Create: `MatterEngine3/include/matter/engine_context.h`
- Create: `MatterEngine3/include/matter/world_session.h`
- Create: `MatterEngine3/include/matter/events.h`
- Create: `MatterEngine3/include/matter/query.h`

**Interfaces:**
- Produces: the exact API below. Tasks 5–7 implement it; Task 6 makes the viewer consume it. Do not deviate from these signatures — if implementation reveals a genuine problem, fix here and note it in your report.

- [ ] **Step 1: Write `matter/events.h`**

```cpp
#pragma once
#include <string>

namespace matter {

enum class EventType { BakeStarted, BakePartDone, BakeFinished, BakeError };

struct Event {
    EventType type = EventType::BakeStarted;
    std::string module;        // BakePartDone: part module name
    int done = 0, total = 0;   // BakePartDone progress counters
    std::string message;       // BakeError: error detail
};

} // namespace matter
```

- [ ] **Step 2: Write `matter/query.h`**

```cpp
#pragma once
#include <cstdint>

namespace matter {

struct RayHit {
    float t = -1.0f;
    float normal[3] = {0, 0, 0};   // world-space, faces the ray origin
    uint32_t instance = 0;         // index usable with instance_info()
    uint64_t part_hash = 0;
    int material_id = -1;
};

struct InstanceInfo {
    float transform[16];           // row-major world placement
    uint64_t part_hash = 0;
    const char* module_name = nullptr;  // may be null; valid until next bake/reload
};

} // namespace matter
```

- [ ] **Step 3: Write `matter/world_session.h`**

```cpp
#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include "raylib.h"   // Camera3D — POD math types are part of the API by design

#include "matter/events.h"
#include "matter/query.h"

namespace matter {

struct WorldDesc {
    const char* schemas_dir    = nullptr;
    const char* world_data_dir = nullptr;
    const char* world_name     = nullptr;  // world subdir of world_data_dir
    const char* shared_lib_dir = nullptr;  // shared .js library dir
};

enum class RenderPath { GpuDriven, Raytrace };
enum class ResolverKind { SectorLod, PassThrough };

struct RenderOptions {
    RenderPath   path     = RenderPath::GpuDriven;
    ResolverKind resolver = ResolverKind::SectorLod;
    bool  wireframe       = false;
    bool  hiz_occlusion   = false;    // default OFF (known false-positive issue)
    float pixel_budget    = 0.0f;     // 0 = default (1.0); clamped to [0.05, 4.0]
    float active_radius   = 0.0f;     // SectorLod knob; 0 = default (64.0)
    float min_projected_size = 0.0f;  // SectorLod sub-pixel cull; 0 = off
};

struct FrameStats {
    // per-frame timings (ms)
    float resolve_ms = 0, build_ms = 0, draw_ms = 0;
    // per-frame counters
    uint32_t instances_resolved = 0;  // resolver output count
    uint32_t instances_drawn   = 0;   // clusters emitted by the GPU cull
    uint32_t clusters_culled   = 0;   // frustum-culled clusters
    uint32_t hiz_culled        = 0;   // HiZ-occlusion-culled clusters
    uint32_t triangles         = 0;   // rasterized triangle count
    // world/bake census (filled by request_bake / reload)
    uint32_t instances_total = 0;
    uint32_t parts_baked = 0;         // cache misses last bake
    uint32_t cache_hits  = 0;         // cache hits last bake
    int probe_dims[3] = {0, 0, 0};    // probe grid (all zero = unavailable)
};

class WorldSession {
public:
    ~WorldSession();   // releases session GL resources — destroy before CloseWindow

    // Phase A: synchronous — returns after the full bake + GPU upload completes.
    // Emits BakeStarted, BakePartDone xN, then BakeFinished or BakeError.
    void request_bake();

    // Poll provider deltas and apply them to world state. Call once per frame.
    void tick();

    // Resolve -> cull -> clear (kernel-derived sky color) -> draw into the
    // currently bound framebuffer. Requires a live GL context on this thread.
    void render(const Camera3D& cam, int fb_width, int fb_height,
                const RenderOptions& opts);

    bool poll_event(Event& out);       // drain one; loop until false
    const FrameStats& frame_stats() const;

    // Live-edit rebake. Fail-closed: on error the old world keeps rendering
    // and a BakeError event is emitted. Same event sequence as request_bake.
    void reload();

    // Query API (backed by a lazily built CPU BVH; first call after a bake pays
    // the build cost).
    bool raycast(const float origin[3], const float dir[3], float max_t, RayHit& out);
    uint32_t instance_count() const;
    bool instance_info(uint32_t idx, InstanceInfo& out);

    struct Impl;
    explicit WorldSession(std::unique_ptr<Impl> impl);   // internal; use open_world
    WorldSession(const WorldSession&) = delete;
    WorldSession& operator=(const WorldSession&) = delete;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace matter
```

- [ ] **Step 4: Write `matter/engine_context.h`**

```cpp
#pragma once
#include <memory>
#include <string>

#include "matter/world_session.h"

namespace matter {

struct EngineDesc {
    const char* cache_root = "cache";  // .part cache location (parts/<hash>.part)
    const char* shader_dir = nullptr;  // nullptr = embedded (MATTER_SHADER_DIR env overrides)
    bool allow_gl_lt_46 = false;       // true only for the ray-traced fallback path
};

class EngineContext {
public:
    // Requires a live GL context current on this thread (the app owns the
    // window). Fails with a GL-version error if GL < 4.6 unless
    // desc.allow_gl_lt_46. Returns nullptr + err on failure; no exceptions
    // cross the API boundary.
    static std::unique_ptr<EngineContext> create(const EngineDesc& desc,
                                                 std::string& err);
    ~EngineContext();

    std::unique_ptr<WorldSession> open_world(const WorldDesc& desc,
                                             std::string& err);

    EngineContext(const EngineContext&) = delete;
    EngineContext& operator=(const EngineContext&) = delete;

    struct Impl;
private:
    explicit EngineContext(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace matter
```

- [ ] **Step 5: Header-compile check**

```bash
cd MatterEngine3 && for h in include/matter/*.h; do \
  g++ -std=c++17 -fsyntax-only -Iinclude -I../Libraries/raylib/src $h || exit 1; done && echo headers ok
```

(Adjust the raylib include path to what `viewer/Makefile`'s `RAYLIB_PATH` uses.) Expected: `headers ok`.

- [ ] **Step 6: Commit**

```bash
git add MatterEngine3/include/matter
git commit -m "feat(phase-a): matter:: public API headers (Stage 2a)"
```

---

### Task 5: Stage 2b — facade implementation + bake progress callback

**Files:**
- Create: `MatterEngine3/viewer/matter_engine.cpp`
- Modify: `MatterEngine3/viewer/local_provider.h` (add `on_part` to `LocalProviderConfig`)
- Modify: `MatterEngine3/viewer/local_provider.cpp` (invoke `on_part` in the per-part bake loop)
- Modify: `MatterEngine3/viewer/Makefile` (add `matter_engine.cpp` to `VIEWER_SRC`)

**Interfaces:**
- Consumes: the Task 4 headers verbatim; `matter::shader_text`/`set_shader_override_dir`; all existing viewer classes (`LocalProvider`, `PartStore`, `WorldComposer`, `RasterComposer`, `GpuCuller`, `Renderer`, `SectorLodResolver`, `PassThroughResolver`, `ProbeTextures`, `viewer::gl46_available`, `viewer::make_lookat/make_perspective/mul16/extract_frustum_planes` from `raster_cull.h`).
- Produces: working `EngineContext::create`, `open_world`, and all `WorldSession` methods except the three query methods (`raycast`/`instance_count`/`instance_info` are stubbed: return `false`/`0`/`false` — implemented in Task 7).
- Produces: `LocalProviderConfig::on_part` — `std::function<void(const char* module, int done, int total)>`, called once per part actually baked or fetched during `fetch_parts`.

**The recipe is `viewer/main.cpp` (unmodified until Task 6).** Read it fully first. Mapping:

| main.cpp | facade |
|---|---|
| GL 4.6 gate (lines ~95–113) | `EngineContext::create` (err instead of FATAL; `allow_gl_lt_46` skips) |
| `connect_sequence` lambda (~170–264) | `WorldSession::request_bake` / `reload` |
| GpuCuller init (~270–279) | inside `request_bake`, first success only, per-session member |
| RT warm-up (~290–294) | lazy: first `render()` with `path == Raytrace` |
| per-frame resolve/cull (~392–431) | `render()` first half |
| `ClearBackground(sky_clear)` + draw (~438–457) | `render()` second half — `glClearColor`+`glClear` with the kernel's tone-mapped sky color (same tonemap lambda, keep as floats; don't quantize to 8-bit before clearing — pass `gamma`-mapped floats straight to `glClearColor` so output matches `ClearBackground(sky_clear)` within 1/255) |
| `poll_deltas` + `state.apply` (~507–508) | `tick()` |
| shutdown order (~549–557) | `~WorldSession` (probe_tex → raster → composer → store) |

- [ ] **Step 1: Add the progress callback to LocalProvider**

In `local_provider.h`, add to `LocalProviderConfig`:

```cpp
#include <functional>
// ...
    // Invoked during fetch_parts once per part processed (bake or cache hit):
    // module = part module name, done/total = progress through the want list.
    std::function<void(const char* module, int done, int total)> on_part;
```

In `local_provider.cpp`, find the loop in `fetch_parts` that iterates the want list (it logs per-part bake/cache activity); after each part completes add:

```cpp
if (cfg_.on_part) cfg_.on_part(module_name_c_str, (int)(i + 1), (int)want.size());
```

(using whatever the loop's module-name variable and index actually are — read the function).

- [ ] **Step 2: Write the facade skeleton**

`MatterEngine3/viewer/matter_engine.cpp` structure (fill each method by relocating the main.cpp logic per the mapping table; this is a *move* of logic, not a redesign — keep comments and constants like `near_z 0.05 / far_z 4000`, TLAS `expanded_count` depth cap 8, budget clamp identical):

```cpp
#include "matter/engine_context.h"
#include "matter/world_session.h"

#include "local_provider.h"
#include "part_store.h"
#include "world_composer.h"
#include "sector_resolver.h"
#include "renderer.h"
#include "raster_composer.h"
#include "probe_texture.h"
#include "gpu_culler.h"
#include "raster_cull.h"
#include "gl46.h"
#include "shader_source.h"

#include <deque>
#include <functional>

// GL clear: include the same GL header gpu_culler.cpp uses (check its includes).

namespace matter {

struct EngineContext::Impl {
    std::string cache_root;
    bool gl46 = false;
};

struct WorldSession::Impl {
    EngineContext::Impl* engine = nullptr;  // non-owning
    viewer::LocalProviderConfig cfg;
    std::unique_ptr<viewer::LocalProvider> provider;
    viewer::WorldManifest manifest;
    viewer::WorldState state;
    std::unique_ptr<viewer::PartStore> store;
    std::unique_ptr<viewer::WorldComposer> composer;
    std::unique_ptr<viewer::RasterComposer> raster;
    lod_select::PartLodTable lods;
    viewer::ProbeTextures probe_tex{};
    float sky_clear[3] = {96/255.f, 118/255.f, 143/255.f};
    viewer::GpuCuller gpu_culler;      // per-session (replaces reset-on-switch)
    bool culler_ready = false;
    viewer::Renderer renderer;         // RT path only; camera synced per render
    bool rt_shader_ready = false, rt_warmed = false;
    viewer::PassThroughResolver pass;
    viewer::SectorLodResolver sec{16.0f, 64.0f};
    bool connected = false;
    std::deque<Event> events;
    FrameStats stats{};
    bool bake_once(std::string& err);  // the relocated connect_sequence
};

// EngineContext::create: gl46_available check (respect allow_gl_lt_46),
// set_shader_override_dir(desc.shader_dir), stash cache_root.
// open_world: fill cfg from WorldDesc + engine cache_root, construct provider,
// install cfg.on_part -> events.push_back(BakePartDone). No bake here.
// request_bake: events BakeStarted; bake_once(); on success census stats +
// BakeFinished, on failure BakeError{message=err}. First success: gpu_culler.init
// (only when engine->gl46), culler_ready = true.
// reload: gpu_culler-safe rebake — recreate provider from cfg, then same as
// request_bake. Old world keeps rendering if bake_once fails partway ONLY in the
// sense today's viewer has (fail-closed at the script/bake level); if bake_once
// fails after tearing down store/composer, mark connected=false so render()
// no-ops — document this in the method comment.
// tick: poll_deltas -> state.apply.
// render: no-op unless connected. Apply opts (defaults: budget 0 -> 1.0 then
// clamp 0.05..4.0; active_radius 0 -> 64.0), sec/raster set_pixel_budget,
// raster->set_wireframe, resolver select. Raytrace path: lazy init_shader
// ("shaders/raytrace_tlas_blas_processed.fs") + warm_up, sync renderer.camera()
// = cam, compose, clear, renderer.draw(store->blas(), composer->tlas()).
// GpuDriven path: exact main.cpp lines ~405-431 block (resolve, lookat/persp/
// mul16 with aspect = fb_width/(float)fb_height, frustum planes,
// set_hiz_enabled(opts.hiz_occlusion), cull) then glClearColor(sky_clear...,1)+
// glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT), draw_gpu_driven,
// build_hiz(fb_width, fb_height), fill stats (resolve/build/draw ms, counters).
// poll_event: front of deque, pop, return true; false when empty.
// raycast/instance_count/instance_info: stubs returning false/0/false (Task 7).

} // namespace matter
```

Key details the implementer must get right:
- `bake_once` is main.cpp's `connect_sequence` body verbatim (including the `expanded_count` recursion, probe upload, `set_lights`, `init_gpu_driven` fail path, tonemap) with `printf`-error paths converted to the `err` out-string, and the RT-vs-raster `use_rt` branches driven by lazy state instead (raster init always happens; RT shader init is deferred to first Raytrace render).
- Today `renderer.set_lights(manifest.lights)` is called in connect for the RT shader; keep an equivalent: store `manifest.lights` and call `renderer.set_lights` during the lazy RT init (after `init_shader`), since set_lights on an unloaded shader is a no-op today anyway — read `renderer.cpp` to confirm ordering needs.
- The events deque must not grow unboundedly if the app never drains: cap at 4096 (drop oldest) with a one-line comment.
- `ClearBackground(sky_clear)` today quantizes to 8-bit; to stay pixel-identical, compute the same `(unsigned char)` values as main.cpp:219–230, then pass `c/255.0f` to `glClearColor`.

- [ ] **Step 3: Build**

Add `matter_engine.cpp` to `VIEWER_SRC` in `MatterEngine3/viewer/Makefile`. Run: `make -C MatterEngine3/viewer -j$(nproc)`. Expected: clean build (facade compiled+linked, not yet called).

- [ ] **Step 4: Sanity — existing viewer still renders**

```bash
GALLIUM_DRIVER=d3d12 MatterEngine3/tools/viewer_shots.sh facade-noop /tmp/phase-a-facade
python3 MatterEngine3/tools/img_diff.py /home/jkern/phase-a-refs/ref_aerial.png /tmp/phase-a-facade/facade-noop_aerial.png
```

Expected: MATCH (facade is dead code this task; this catches accidental behavior changes from the local_provider edit).

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/viewer/matter_engine.cpp MatterEngine3/viewer/local_provider.h \
    MatterEngine3/viewer/local_provider.cpp MatterEngine3/viewer/Makefile
git commit -m "feat(phase-a): EngineContext/WorldSession facade over in-place code (Stage 2b)"
```

---

### Task 6: Stage 3 — viewer converts to the public API (screenshot exit gate)

**Files:**
- Modify: `MatterEngine3/viewer/main.cpp` (full rewrite of engine interaction; FIFO/screenshot/env plumbing preserved)
- Modify: `MatterEngine3/viewer/ui.cpp` / `ui.h` only if compilation demands it (ViewerStats stays as the HUD's struct, fed from FrameStats)

**Interfaces:**
- Consumes: `matter/*.h` only (plus raylib, imgui via ui, and `ui.h`). After this task main.cpp must not include any other engine header (`local_provider.h`, `part_store.h`, `renderer.h`, `gpu_culler.h`, `raster_cull.h`, `gl46.h`, … all gone). This is the future grep-gate condition — self-check it now.
- Produces: a viewer with byte-equivalent behavior: same env vars (`MATTER_RT`, `MATTER_CAM`, `MATTER_WORLD`, `MATTER_HIZ`, `MATTER_SCREENSHOT`, `MATTER_CMD_FIFO`), same FIFO commands, same STATS line, same HUD.

Conversion map (rewrite main.cpp around this; everything not listed stays as-is):

- Window init, `scan_worlds("../examples")`, world pick via `MATTER_WORLD` — unchanged.
- App now owns a `Camera3D camera` directly. Copy the initial camera values from `Renderer::init_camera()` (read `renderer.cpp`) into main. Copy the body of `Renderer::update_camera_free()` into a static `update_camera_free(Camera3D&)` in main.cpp (free-cam is app policy now). `MATTER_CAM` writes `camera.position/.target`. `ui.draw_camera_panel(camera)` unchanged.
- Engine setup replaces the Renderer/gate block:

```cpp
matter::EngineDesc edesc;
edesc.cache_root = "cache";
edesc.allow_gl_lt_46 = use_rt;
std::string err;
auto engine = matter::EngineContext::create(edesc, err);
if (!engine) { fprintf(stderr, "FATAL: %s\n", err.c_str()); CloseWindow(); return 1; }
```

  Keep the `MATTER_GPU_CULL=0`-requires-`MATTER_RT=1` FATAL: `viewer::gpu_cull_requested()` is two lines of env reading — inline the env read into main.cpp (drop the `gl46.h` include).
- Opening/baking a world (initial, reload, world switch all use this):

```cpp
auto open_and_bake = [&](const viewer::WorldEntry& w) -> std::unique_ptr<matter::WorldSession> {
    matter::WorldDesc wd;
    wd.schemas_dir = w.schemas_dir.c_str();
    wd.world_data_dir = w.world_data_dir.c_str();
    wd.world_name = w.world_name.c_str();
    wd.shared_lib_dir = "../shared-lib";
    std::string werr;
    auto s = engine->open_world(wd, werr);
    if (!s) { printf("open_world: %s\n", werr.c_str()); return nullptr; }
    s->request_bake();
    matter::Event ev; bool ok = true;
    while (s->poll_event(ev)) {
        if (ev.type == matter::EventType::BakePartDone)
            printf("bake %d/%d %s\n", ev.done, ev.total, ev.module.c_str());
        if (ev.type == matter::EventType::BakeError) {
            printf("bake error: %s\n", ev.message.c_str()); ok = false;
        }
    }
    return ok ? std::move(s) : nullptr;
};
```

- `apply_world_resolver_defaults` stays in main.cpp but now writes app-side floats `float active_radius; float min_projected_size;` alongside `stats.resolver_choice` (Meadow: 400 / 0.0015 / SectorLod; else 64 / 0 / PassThrough).
- Per-frame block becomes:

```cpp
session->tick();
matter::RenderOptions opts;
opts.path = use_rt ? matter::RenderPath::Raytrace : matter::RenderPath::GpuDriven;
opts.resolver = stats.resolver_choice == 1 ? matter::ResolverKind::SectorLod
                                           : matter::ResolverKind::PassThrough;
opts.wireframe = wireframe;                  // app-side bool, F9 / FIFO toggle
opts.hiz_occlusion = stats.hiz_enabled;
opts.pixel_budget = stats.pixel_budget;
opts.active_radius = active_radius;
opts.min_projected_size = min_projected_size;

BeginDrawing();
    session->render(camera, GetScreenWidth(), GetScreenHeight(), opts);
    const matter::FrameStats& fs = session->frame_stats();
    stats.resolve_ms = fs.resolve_ms;   stats.build_ms = fs.build_ms;
    stats.draw_ms = fs.draw_ms;
    stats.instances_active = (int)fs.instances_resolved;
    stats.gpu_emitted = (int)fs.instances_drawn;
    stats.gpu_culled = (int)fs.clusters_culled;
    stats.gpu_culled_hiz = (int)fs.hiz_culled;
    stats.culled_clusters = stats.gpu_culled;
    stats.raster_tris = (int)fs.triangles;
    stats.raster_batches = 0; stats.batch_cache_hit = false;
    stats.instances_total = (int)fs.instances_total;
    stats.parts_baked = (int)fs.parts_baked;
    stats.cache_hits = (int)fs.cache_hits;
    stats.connected = true;
    memcpy(stats.probe_dims, fs.probe_dims, sizeof stats.probe_dims);
    ui.begin_frame();
    ui.draw_debug_panel(stats);
    ui.draw_worlds_panel(worlds, stats);
    ui.draw_camera_panel(camera);
    ui.end_frame();
EndDrawing();
```

  Note `ClearBackground` is gone — the kernel clears. `stats.gpu_cull_active = !use_rt;` after engine create. `stats.fps/frame_ms/cam_pos` fills unchanged.
- FIFO: `wireframe`/F9 toggle a main.cpp `bool wireframe`; `reload` → `session->reload()` then drain events (print errors; on BakeError keep running — matches fail-closed); world switch → destroy old session (`session.reset()`), `open_and_bake(worlds[idx])`, exit if null (matches today's "world switch failed; exiting"). `budget` clamp stays in main (0.05–4.0) so the HUD shows the clamped value.
- Shutdown: `session.reset(); ui.shutdown(); CloseWindow();` — session destructor releases GL, must precede CloseWindow.
- STATS printf: identical format string, sourced from the mapped stats fields.

- [ ] **Step 1: Rewrite main.cpp per the map**
- [ ] **Step 2: Build**

Run: `make -C MatterEngine3/viewer -j$(nproc)`. Fix compile errors (unused engine headers/objects in the Makefile stay — files are removed from VIEWER usage only at move time).

- [ ] **Step 3: Include self-check**

Run: `grep -nE '#include "(local_provider|part_store|world_composer|sector_resolver|renderer|raster_composer|probe_texture|gpu_culler|raster_cull|gl46|world_source|shader_source)' MatterEngine3/viewer/main.cpp` — expected: no output.

- [ ] **Step 4: THE screenshot gate**

```bash
GALLIUM_DRIVER=d3d12 MatterEngine3/tools/viewer_shots.sh api /tmp/phase-a-api
for p in aerial corner midfield far empty; do
  python3 MatterEngine3/tools/img_diff.py /home/jkern/phase-a-refs/ref_$p.png /tmp/phase-a-api/api_$p.png || exit 1
done
diff <(grep -o '^STATS,[^,]*' /home/jkern/phase-a-refs/ref_stats.log) \
     <(grep -o '^STATS,[^,]*' /tmp/phase-a-api/api_stats.log)
```

Expected: 5× MATCH and identical STATS label sequence. **Do not proceed with a diff.** If a pose differs, debug (clear-color quantization, aspect ratio, near/far, HiZ default) until MATCH.

- [ ] **Step 5: RT-path smoke** (slow, ~60 s warm-up — one shot only)

```bash
cd MatterEngine3/viewer && GALLIUM_DRIVER=d3d12 MATTER_RT=1 MATTER_WORLD=meadow \
  MATTER_SCREENSHOT=/tmp/phase-a-rt.png ./viewer
```

Expected: exits cleanly, `/tmp/phase-a-rt.png` non-black (visual check with Read tool; RT output differs from raster — no ref diff).

- [ ] **Step 6: Commit**

```bash
git add MatterEngine3/viewer/main.cpp MatterEngine3/viewer/ui.h MatterEngine3/viewer/ui.cpp
git commit -m "refactor(phase-a): viewer runs on the matter:: public API only (Stage 3, screenshot gate green)"
```

---

### Task 7: Stage 4 — query API + api_tests

**Files:**
- Modify: `MatterEngine3/include/world_tracer.h` + `MatterEngine3/src/world_tracer.cpp` (instance identity on Hit + instance table accessors)
- Modify: `MatterEngine3/viewer/matter_engine.cpp` (implement the three query stubs)
- Create: `MatterEngine3/tests/api_tests.cpp` (built via `MatterEngine3/viewer/Makefile` as target `api-tests` for now — it needs the facade TU; moves in Task 8/9)
- Modify: `MatterEngine3/viewer/Makefile` (api-tests target)

**Interfaces:**
- Consumes: `world_tracer::WorldTracer` (see Interface Reference), `matter/query.h` structs.
- Produces: working `WorldSession::raycast/instance_count/instance_info`; `api_tests` binary exercising the full public API.

- [ ] **Step 1: Extend WorldTracer with instance identity**

In `world_tracer.h`: add `uint32_t instance = 0xffffffffu;` to `Hit`; add to `WorldTracer`:

```cpp
// Post-expansion instance table (children expanded by the compositional
// fallback get their own entries). Valid after build().
size_t expanded_instance_count() const;
bool expanded_instance(size_t idx, uint64_t& part_hash, float transform[16]) const;
```

In `world_tracer.cpp`: the instance BVH already stores per-instance records (part hash + transform, int32 instance indices per the header comment) — set `hit.instance` from the winning instance index in `trace`, and expose the table through the two accessors. Read the Impl before coding; keep the change additive.

- [ ] **Step 2: Implement the facade queries**

In `matter_engine.cpp` `WorldSession::Impl` add:

```cpp
std::unique_ptr<world_tracer::WorldTracer> tracer;
bool tracer_dirty = true;   // set true after every bake/reload/tick-applied delta
std::string module_of_instance(uint64_t part_hash);  // see module-name note
```

`ensure_tracer()`: if dirty, build `std::vector<world_tracer::TraceInstance>` from `state` root entries (part_hash + transform), `tracer->build(cfg.cache_root, v, err)`; on failure keep tracer null and return false. `raycast`: ensure_tracer, `tracer->trace`, map `Hit` → `RayHit` (`t`, `normal`, `material_id`, `instance = hit.instance`, `part_hash` via `expanded_instance(hit.instance, …)`). `instance_count`: `tracer ? expanded_instance_count() : 0` (const method — build in `request_bake` success path instead if ensure_tracer-from-const is awkward; lazy build on first query is the goal, choose `mutable` members). `instance_info`: `expanded_instance` fills transform + part_hash.

Module name: check `world_source.h` / `part_store.h` for an existing part-hash→module-name mapping (manifest entries or `LoadedPart`). If one exists, use it; if genuinely absent, add `std::string module;` to `WorldManifestEntry` and fill it in `LocalProvider::connect` where the manifest is parsed (it knows each instance's module), then map hash→module in the session. `module_name` may be nullptr for expanded child instances that have no manifest entry — that's acceptable and documented in query.h.

- [ ] **Step 3: Write api_tests**

`MatterEngine3/tests/api_tests.cpp`:

```cpp
// Full public-API integration test: hidden GL window (app-owned), EngineContext,
// bake, event sequence, offscreen render, raycast. Run with GALLIUM_DRIVER=d3d12.
#include "matter/engine_context.h"
#include "raylib.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

int main() {
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(640, 360, "api_tests");
    std::string err;
    matter::EngineDesc ed;
    ed.cache_root = "cache";   // run from MatterEngine3/viewer so the bake cache is warm
    auto engine = matter::EngineContext::create(ed, err);
    if (!engine) { printf("FAIL create: %s\n", err.c_str()); return 1; }

    matter::WorldDesc wd;
    wd.schemas_dir    = "../examples/<FIXTURE>/schemas";
    wd.world_data_dir = "../examples/<FIXTURE>/WorldData";
    wd.world_name     = "<FIXTURE_WORLD>";
    wd.shared_lib_dir = "../shared-lib";
    auto session = engine->open_world(wd, err);
    if (!session) { printf("FAIL open_world: %s\n", err.c_str()); return 1; }

    session->request_bake();
    std::vector<matter::Event> evs;
    matter::Event ev;
    while (session->poll_event(ev)) evs.push_back(ev);
    assert(!evs.empty());
    assert(evs.front().type == matter::EventType::BakeStarted);
    assert(evs.back().type == matter::EventType::BakeFinished);
    int part_done = 0;
    for (auto& e : evs) if (e.type == matter::EventType::BakePartDone) ++part_done;
    printf("events: %zu (%d PartDone)\n", evs.size(), part_done);

    assert(session->instance_count() > 0);
    matter::InstanceInfo info;
    assert(session->instance_info(0, info));

    // render into the hidden window's framebuffer; assert non-black output
    Camera3D cam{};
    cam.position = (Vector3){ info.transform[3] + 8.0f, info.transform[7] + 6.0f,
                              info.transform[11] + 8.0f };
    cam.target = (Vector3){ info.transform[3], info.transform[7], info.transform[11] };
    cam.up = (Vector3){0, 1, 0}; cam.fovy = 60.0f; cam.projection = CAMERA_PERSPECTIVE;
    matter::RenderOptions opts;   // defaults: GpuDriven + SectorLod
    opts.resolver = matter::ResolverKind::PassThrough;
    for (int i = 0; i < 3; ++i) {
        BeginDrawing();
        session->render(cam, GetScreenWidth(), GetScreenHeight(), opts);
        EndDrawing();
    }
    Image img = LoadImageFromScreen();
    Color* px = LoadImageColors(img);
    long nonblack = 0, n = (long)img.width * img.height;
    for (long i = 0; i < n; ++i)
        if (px[i].r > 8 || px[i].g > 8 || px[i].b > 8) ++nonblack;
    printf("nonblack: %ld/%ld\n", nonblack, n);
    assert(nonblack > n / 20);
    UnloadImageColors(px); UnloadImage(img);

    // raycast straight down onto instance 0
    float origin[3] = { info.transform[3], info.transform[7] + 100.0f, info.transform[11] };
    float dir[3] = { 0, -1, 0 };
    matter::RayHit hit;
    bool hit_ok = session->raycast(origin, dir, 1000.0f, hit);
    printf("raycast: hit=%d t=%.3f instance=%u\n", (int)hit_ok, hit.t, hit.instance);
    assert(hit_ok && hit.t > 0.0f);

    session.reset();   // before CloseWindow
    engine.reset();
    CloseWindow();
    printf("api_tests: all passed\n");
    return 0;
}
```

Fixture: `ls MatterEngine3/examples/` and pick the smallest world (fewest schemas / WorldData entries; **not** Meadow — too slow). Substitute `<FIXTURE>`/`<FIXTURE_WORLD>` with real names. If the down-ray from instance 0 can miss (e.g. instance is a floating object), aim at the instance origin from offset `(0, +100, 0)` toward `(tx, ty, tz)` instead — adjust dir to normalized `(target - origin)`.

- [ ] **Step 4: Makefile target + run**

Add `api-tests` to `MatterEngine3/viewer/Makefile` mirroring the `gpu-tests` target (same engine objs + `matter_engine.cpp`, sources `../tests/api_tests.cpp`).

```bash
make -C MatterEngine3/viewer api-tests -j$(nproc)
cd MatterEngine3/viewer && GALLIUM_DRIVER=d3d12 ./api_tests
```

Expected: `api_tests: all passed`.

- [ ] **Step 5: Headless suites still green**

Run: `make -C MatterEngine3/tests -j$(nproc) && (cd MatterEngine3/tests && ./run_all 2>/dev/null || make -C MatterEngine3/tests run)` — read the tests Makefile for the real run target; all suites pass.

- [ ] **Step 6: Commit**

```bash
git add MatterEngine3/include/world_tracer.h MatterEngine3/src/world_tracer.cpp \
    MatterEngine3/viewer/matter_engine.cpp MatterEngine3/tests/api_tests.cpp \
    MatterEngine3/viewer/Makefile <plus world_source/local_provider files if module-name plumbing touched them>
git commit -m "feat(phase-a): query API over WorldTracer + api_tests (Stage 4)"
```

---

### Task 8: Stage 5a — the moves (pure `git mv`, three commits, no logic edits)

**Files:** moves only. **No content edits in these commits** (builds are expected to break until Task 9 — that is by design; note it in each commit message).

- [ ] **Step 1: Quiet-tree check**

Run: `git status` — only expected phase-A work in flight. If unrelated modified files exist (other agents), leave them untouched; `git mv` only the listed files.

- [ ] **Step 2: Commit 1 — kernel material into the library tree**

```bash
cd MatterEngine3
mkdir -p src/render src/provider
# render/
git mv viewer/renderer.h viewer/renderer.cpp viewer/raster_composer.h viewer/raster_composer.cpp \
       viewer/raster_mesh.h viewer/raster_mesh.cpp viewer/part_store.h viewer/part_store.cpp \
       viewer/world_composer.h viewer/world_composer.cpp viewer/world_state.h viewer/world_state.cpp \
       viewer/gpu_culler.h viewer/gpu_culler.cpp viewer/probe_texture.h viewer/probe_texture.cpp \
       viewer/tileset_provider.h viewer/tileset_provider.cpp viewer/tileset_gl_ctx.h viewer/tileset_gl_ctx.cpp \
       viewer/tileset_bake_primary.cpp viewer/tileset_bake_ao.cpp \
       viewer/raster_cull.h viewer/gl46.h src/render/
# provider/
git mv viewer/local_provider.h viewer/local_provider.cpp viewer/world_source.h \
       viewer/sector_resolver.h viewer/sector_resolver.cpp src/provider/
# facade
git mv viewer/matter_engine.cpp src/
# flat internal headers move next to their sources (keep include/matter/ + generated)
git mv $(git ls-files 'include/*.h' | grep -v '^include/matter/') src/
# shaders: shaders_gpu becomes a real kernel dir; the MSL symlink relocates
git mv viewer/shaders_gpu shaders_gpu
git mv viewer/shaders shaders     # symlink entry; verify link target still resolves
git commit -m "refactor(phase-a): move render/provider/facade + flat headers into kernel tree (Stage 5, pure renames; builds fixed in follow-up)"
```

Adjust the exact file list to reality first: `ls MatterEngine3/viewer/*.{h,cpp}` — everything except `main.cpp`, `ui.h`, `ui.cpp`, test `*.cpp` files, and the Makefile moves. After the symlink move, `ls -la MatterEngine3/shaders` must show it resolving to `../MatterSurfaceLib/shaders`; if the relative target broke (it was `../../MatterSurfaceLib/shaders` from inside `viewer/`), fix the link in Task 9 (a symlink retarget is a content change — allowed there, not here... exception: if `git mv` itself breaks the link target, retarget it in THIS commit with `ln -sfn ../MatterSurfaceLib/shaders shaders && git add shaders`, since a dangling symlink is a broken rename, not a logic edit).

- [ ] **Step 3: Commit 2 — viewer app to top level**

```bash
cd <repo-root>
mkdir -p MatterViewer
git mv MatterEngine3/viewer/main.cpp MatterEngine3/viewer/ui.h MatterEngine3/viewer/ui.cpp MatterViewer/
git mv MatterEngine3/viewer/Makefile MatterViewer/Makefile
git commit -m "refactor(phase-a): MatterViewer becomes a top-level app project (Stage 5, pure renames)"
```

Runtime cache is untracked — carry it so no rebake: `cp -r MatterEngine3/viewer/cache MatterViewer/cache` (not committed).

- [ ] **Step 4: Commit 3 — GPU tests into the kernel test tree**

```bash
git mv MatterEngine3/viewer/gpu_cull_tests.cpp MatterEngine3/viewer/tileset_gpu_tests.cpp \
       MatterEngine3/viewer/tileset_seam_tests.cpp MatterEngine3/viewer/tileset_provider_tests.cpp \
       MatterEngine3/viewer/tileset_load_tests.cpp MatterEngine3/tests/ 2>/dev/null
git commit -m "refactor(phase-a): GPU test binaries live with the kernel tests (Stage 5, pure renames)"
```

(Adjust names to the actual `*_tests.cpp` files present; move every test source left in `viewer/`. Whatever non-source stragglers remain in `viewer/` — e.g. `.gitignore`, docs — move or delete judiciously in Task 9, and `viewer/` disappears.)

---

### Task 9: Stage 5b — Makefiles, include paths, scripts, docs (builds green again)

**Files:**
- Modify: `MatterEngine3/Makefile` (library absorbs `src/render/ src/provider/ src/matter_engine.cpp`; codegen path fix `viewer/` → `.`; `-I` fixes; windows-lib if trivial — see below)
- Modify: `MatterViewer/Makefile` (app links `../MatterEngine3/libmatter_engine3.a`; `-I../MatterEngine3/include` ONLY for engine headers)
- Modify: `MatterEngine3/tests/Makefile` (`-I../src`, new test file paths, `api-tests` target moves here)
- Modify: `MatterEngine3/tools/viewer_shots.sh` (`cd $HERE/../viewer` → `cd $HERE/../../MatterViewer`), plus `meadow_sweep.sh` / any tool with the old path: `grep -rln 'viewer' MatterEngine3/tools/`
- Modify: `build-all.sh` (project list + test wiring paths), `CLAUDE.md` (project list gains MatterViewer, MatterEngine3 described as kernel library)
- Modify: any moved source whose `#include` paths broke

**Interfaces:**
- Consumes: post-move layout from Task 8.
- Produces: `make -C MatterEngine3` → `libmatter_engine3.a` (bake + render + provider + facade); `make -C MatterViewer` → `viewer` linking the .a; all test targets build from their new homes.

Key decisions already made — implement as stated:
- **Linux enforces the library boundary:** MatterViewer compiles only `main.cpp` + `ui.cpp` + imgui, links `libmatter_engine3.a` + MatterSurfaceLib's lib + raylib. Engine include path is `-I../MatterEngine3/include` (which now contains ONLY `matter/`); the autoremesher link flags (`AR_CORE_LDFLAGS`, `--allow-multiple-definition`) move to the app link line, gated by the same `wildcard` conditional (copy the block from the old viewer Makefile section).
- **Windows target stays a direct-source build** inside `MatterViewer/Makefile` (mingw compiles kernel sources directly, as today — only paths change, `../MatterEngine3/src/render/...` etc.). Pragmatic: no mingw .a packaging this phase. The grep-gate (Task 10) checks *includes in sources*, which stays clean.
- Moved kernel sources now include flat headers relative to `src/` — the library build adds `-Isrc -Isrc/render -Isrc/provider -Iinclude` so existing `#include "part_store.h"` lines keep working without edits. Only edit an `#include` line when a path genuinely cannot resolve via `-I`.
- `main.cpp`/`ui.cpp` path constants change: `scan_worlds("../examples")` → `scan_worlds("../MatterEngine3/examples")`; `wd.shared_lib_dir = "../shared-lib"` → `"../MatterEngine3/shared-lib"` (verify: `ls MatterEngine3/shared-lib` — if it lives elsewhere, use the real path); cache stays `"cache"` (copied in Task 8).
- Codegen rule: `SHADER_FILES := $(addprefix viewer/,…)` → shaders now at `MatterEngine3/shaders{,_gpu}` — update prefixes; generated header path unchanged.
- `build-all.sh`: add MatterViewer to the project list; keep MatterEngine3; update the GPU-test invocations (lines ~211–229) to the new Makefile homes (`make -C MatterEngine3/tests …` / run from `MatterViewer` where a live window is needed — read the script and follow its structure). `api-tests` joins the GPU-gated section (needs GL window + `GALLIUM_DRIVER=d3d12`).

- [ ] **Step 1: Kernel library builds**

Run: `make -C MatterEngine3 -j$(nproc)` → `libmatter_engine3.a` including render/provider/facade objects. Iterate on `-I` fixes until green.

- [ ] **Step 2: Tests build + pass**

Run: `make -C MatterEngine3/tests -j$(nproc)` then the headless suites (existing run target) — all pass. GPU/api tests: build them; run `GALLIUM_DRIVER=d3d12` from wherever their fixtures resolve (they may need cwd `MatterViewer/` for the warm cache — check each test's relative paths; adjust paths in test *Makefile working-dir or launch dir*, not test logic).

- [ ] **Step 3: MatterViewer builds and runs**

```bash
make -C MatterViewer -j$(nproc)
GALLIUM_DRIVER=d3d12 MatterEngine3/tools/viewer_shots.sh moved /tmp/phase-a-moved
for p in aerial corner midfield far empty; do
  python3 MatterEngine3/tools/img_diff.py /home/jkern/phase-a-refs/ref_$p.png /tmp/phase-a-moved/moved_$p.png || exit 1
done
```

Expected: 5× MATCH.

- [ ] **Step 4: build-all**

Run: `./build-all.sh` — every project builds.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/Makefile MatterViewer/Makefile MatterEngine3/tests/Makefile \
    MatterEngine3/tools/ build-all.sh CLAUDE.md <plus any touched sources>
git commit -m "build(phase-a): kernel .a absorbs render+provider; MatterViewer links the library (Stage 5 wiring)"
```

---

### Task 10: Stage 6 — grep-gate + full verification

**Files:**
- Create: `MatterEngine3/tools/grep_gate.sh`
- Modify: `build-all.sh` (run grep-gate in the `test` path)

- [ ] **Step 1: Write the grep-gate**

```bash
#!/usr/bin/env bash
# Dependency rule: app projects may include only matter/*.h, raylib, imgui, and
# their own headers — never MatterEngine3 internals.
set -u
fail=0
for app in MatterViewer; do
  hits=$(grep -rnE '#include\s+"' "$app" --include='*.cpp' --include='*.h' \
    | grep -vE '"(matter/|raylib|rlgl|raymath|imgui|rlImGui|ui\.h|resource_dir)' || true)
  if [ -n "$hits" ]; then
    echo "GREP-GATE FAIL: $app includes engine internals:"; echo "$hits"; fail=1
  fi
done
[ $fail -eq 0 ] && echo "grep-gate: clean"
exit $fail
```

Adjust the allowlist to the includes actually present (`grep -rn '#include' MatterViewer/`) — every remaining include must be justifiable as app-legal; if main.cpp still includes an internal header, that is a Task 6 regression to fix, not an allowlist entry.

- [ ] **Step 2: Wire into build-all test path + run**

Add `MatterEngine3/tools/grep_gate.sh` to the `test` branch of `build-all.sh` (alongside the headless suites). Run: `./MatterEngine3/tools/grep_gate.sh` → `grep-gate: clean`.

- [ ] **Step 3: Full test sweep**

Run: `GALLIUM_DRIVER=d3d12 ./build-all.sh test` — all 30+ headless suites, shader_source_tests, GPU tests, api_tests, grep-gate all pass.

- [ ] **Step 4: Final screenshot gate**

```bash
GALLIUM_DRIVER=d3d12 MatterEngine3/tools/viewer_shots.sh final /tmp/phase-a-final
for p in aerial corner midfield far empty; do
  python3 MatterEngine3/tools/img_diff.py /home/jkern/phase-a-refs/ref_$p.png /tmp/phase-a-final/final_$p.png || exit 1
done
```

Also compare a `STATS,` numeric snapshot loosely (frame times vary; instance/triangle counters must match exactly): extract fields 6–10 of matching labels from `ref_stats.log` vs `final_stats.log` and diff.

- [ ] **Step 5: Windows clean rebuild** (headers moved ⇒ mandatory full clean)

```bash
grep -n 'build/windows\|WIN_OBJ' MatterViewer/Makefile   # find the obj dir
rm -rf MatterViewer/build/windows                        # adjust to the real dir
make -C MatterViewer windows -j$(nproc)
```

Expected: `viewer.exe` (or the Makefile's named output) links cleanly. Runtime shader note: Windows previously copied shader files next to the exe — with embedding this whole step disappears; delete any shader-copy recipe from the windows target if still present (that edit belongs to Task 9 but catch it here).

- [ ] **Step 6: FIFO harness smoke**

```bash
cd MatterViewer
mkfifo /tmp/phase_a_fifo 2>/dev/null || true
GALLIUM_DRIVER=d3d12 MATTER_WORLD=meadow MATTER_CMD_FIFO=/tmp/phase_a_fifo ./viewer &
sleep 30
echo "budget 0.5" > /tmp/phase_a_fifo
echo "hiz on" > /tmp/phase_a_fifo
echo "wireframe toggle" > /tmp/phase_a_fifo
echo "stats smoke" > /tmp/phase_a_fifo
sleep 2
echo "reload" > /tmp/phase_a_fifo
sleep 30
echo "quit" > /tmp/phase_a_fifo
wait
```

Expected: viewer exits 0; stdout shows the `STATS,smoke,…` line, `hiz on`, `wireframe on`, reload completing. **Never leave the window running** — if `wait` hangs >60 s past the quit, kill the pid and investigate.

- [ ] **Step 7: Commit**

```bash
git add MatterEngine3/tools/grep_gate.sh build-all.sh
git commit -m "test(phase-a): grep-gate dependency rule + Stage 6 verification (Phase A complete)"
```

---

## Self-Review Notes (already applied)

- Spec coverage: Stage 0→Task 1, Stage 1→Tasks 2–3, Stage 2→Tasks 4–5, Stage 3→Task 6, Stage 4→Task 7, Stage 5→Tasks 8–9, Stage 6→Task 10; error-handling section lands in Tasks 4–6 (err-string returns, BakeError events, fail-closed reload); all four spec amendments (float pixel_budget, WorldDesc shape, resolver knobs in RenderOptions, allow_gl_lt_46) are reflected in Task 4's headers.
- Type consistency: `shader_text(const char*, std::string&, std::string&)` used identically in Tasks 2, 3, 5; `FrameStats` field names in Task 4 match the Task 6 mapping block; `WorldDesc`/`RenderOptions` fields consistent across Tasks 4, 6, 7.
- Known judgment calls delegated with decision rules (not placeholders): module-name plumbing (Task 7 step 2), embedded-include discovery (Task 2 pre-step), fixture world choice (Task 7 step 3), windows direct-source build (Task 9).
