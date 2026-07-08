# Task 2 Report: Stage 1a — Shader Embedding Codegen + `matter::shader_text`

## What Was Built

1. **`MatterEngine3/tools/embed_shaders.py`** — codegen script verbatim from the brief. Reads shader files relative to a `<base_dir>`, emits a C++ header with inline string literals keyed by logical path.

2. **`MatterEngine3/include/shader_source.h`** — public API header defining `matter::shader_text()` and `matter::set_shader_override_dir()`, verbatim from the brief.

3. **`MatterEngine3/src/shader_source.cpp`** — implementation verbatim from the brief. Lookup order: `MATTER_SHADER_DIR` env → `g_override_dir` → embedded table.

4. **`MatterEngine3/tests/shader_source_tests.cpp`** — three-assertion test verbatim from the brief: embedded lookup works, unknown path fails with useful error, override dir wins.

5. **`MatterEngine3/.gitignore`** — single entry `shaders_gen/` to ignore the generated directory.

## Pre-Step: Extra `.glsl` Include Files

The brief instructs: grep the `.fs`/`.comp` sources for `#include`, add any extra glsl files to the table. Findings:
- `shaders/raster.fs` includes `tileset_sampling.glsl` (already in brief's table)
- `shaders_gpu/tileset_bake_ao.comp` and `tileset_bake_primary.comp` include `materials.glsl` and `bvh_tlas_common.glsl`
- `shaders/raytrace_tlas_blas.fs` includes `materials.glsl`, `bvh_tlas_common.glsl`, `tileset_sampling.glsl`, `lighting.glsl` (note: this is the non-processed `.fs`, not in the embed table)

Added to `SHADER_LOGICAL`: `shaders/materials.glsl`, `shaders/bvh_tlas_common.glsl`, `shaders/lighting.glsl`. Final embedded count: 12 shaders.

## Makefile Changes

### `MatterEngine3/Makefile`
- Added `SHADER_LOGICAL` / `SHADER_FILES` variables and `shaders_gen/embedded_shaders.h` codegen target.
- Added `src/shader_source.cpp` to `ME3_CPP` and `shader_source.o` to `ME3_OBJ`.
- Made `$(ME3_OBJ)` depend on `shaders_gen/embedded_shaders.h` (so compiling the batch always has the header ready).

### `MatterEngine3/viewer/Makefile`
- Added `../src/shader_source.cpp` to `PIPELINE_CPP`.
- Added `.PHONY: embedded-shaders` target that calls `$(MAKE) -C .. shaders_gen/embedded_shaders.h`.
- Made `viewer` target depend on `embedded-shaders` (before the object builds).
- Added `embedded-shaders` to `.PHONY` list.

### `MatterEngine3/tests/Makefile`
- Added `SHADER_SOURCE_TARGET`, `SHADER_SOURCE_CPP`, `$(SHADER_SOURCE_TARGET)` build rule, `run-shader-source` target.
- Added `$(SHADER_SOURCE_TARGET)` to `clean`.
- Added `run-shader-source` to `.PHONY`.
- Include paths: `-I../include -I..` (so `shader_source.h` is found via `-I../include`, `../shaders_gen/embedded_shaders.h` is found via `-I..`).

## Commands Run and Key Output

### Step 5: Expect failure first
```
$ cd MatterEngine3/tests && make shader_source_tests
make: *** No rule to make target '../shaders_gen/embedded_shaders.h', needed by 'shader_source_tests'.  Stop.
```
Expected failure confirmed.

### Step 5: Generate header
```
$ cd MatterEngine3 && make shaders_gen/embedded_shaders.h
mkdir -p shaders_gen
python3 tools/embed_shaders.py shaders_gen/embedded_shaders.h viewer shaders/raster.vs shaders/raster.fs shaders/tileset_sampling.glsl shaders/raytrace_tlas_blas_processed.fs shaders/materials.glsl shaders/bvh_tlas_common.glsl shaders/lighting.glsl shaders_gpu/cull.comp shaders_gpu/hiz_downsample.comp shaders_gpu/raster_gpu_driven.vs shaders_gpu/tileset_bake_primary.comp shaders_gpu/tileset_bake_ao.comp
```
12 entries embedded.

### Step 5: Test run
```
$ cd MatterEngine3/tests && make run-shader-source
g++ shader_source_tests.cpp ../src/shader_source.cpp -o shader_source_tests \
      -std=c++17 -Wall -Wno-missing-braces -Wno-unused-variable -DPLATFORM_DESKTOP -DGRAPHICS_API_OPENGL_33 -I../include -I..
./shader_source_tests
shader_source_tests: all passed
```

### Step 6: Full library build
```
$ cd MatterEngine3 && make -j$(nproc)
Exit: 0 (no errors/warnings)
```

### Step 6: Full viewer build
```
$ cd MatterEngine3/viewer && make -j$(nproc) viewer
Exit: 0 (no errors/warnings)
```

## Deviations from Brief

- `SHADER_LOGICAL` extended with `shaders/materials.glsl`, `shaders/bvh_tlas_common.glsl`, `shaders/lighting.glsl` per the brief's pre-step instruction. This is required behavior, not a deviation.
- The viewer Makefile uses a `.PHONY: embedded-shaders` approach (runs `$(MAKE) -C ..`) rather than a per-object order-only dependency. This is functionally equivalent to the brief's "add an order-only rule" instruction — it ensures the header is generated before any `.o` is compiled.

## Concerns

None. All tests pass, both full builds succeed, no debug prints left in, `shaders_gen/` is gitignored.
