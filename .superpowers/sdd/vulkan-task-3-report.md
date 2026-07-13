# Vulkan Matrix Phase 1 - Task 3 Report

## Scope

Migrated CPU camera/frustum and transform consumers to the Task 2 canonical
`matter::Mat4f` contract without changing persisted `float[16]` bytes. Public
`WorldSession::render(const Camera3D&, ...)` remains unchanged. GPU matrices are
packed only at the shader boundary through `pack_glsl_mat4`/`pack_instance`.

Required call-site/build integration additionally touched `gpu_culler.cpp`,
`gpu_culler.h`, `MatterEngine3/Makefile`, `MatterEngine3/tests/Makefile`,
`release_part_tests.cpp`, and `shaders_gpu/cull.comp`. The shader depth compare
now consumes Vulkan NDC z directly instead of applying the OpenGL `[-1,1]`
remap.

## TDD RED

Added regressions first in `viewer_logic_tests.cpp` and migrated the GPU camera
fixture in `gpu_cull_tests.cpp`. The first UCRT64 compile failed at the intended
old API boundary:

- `viewer::pack_instance` was not a member of `viewer`.
- `GpuInstanceRec` had no `object_to_world` member.
- The same compile also exposed pre-existing Windows-only `realpath` failures at
  `viewer_logic_tests.cpp:282-283`; these are unrelated to Task 3.

No production code was changed before this RED was captured.

## GREEN / Verification

- `make -C MatterEngine3/tests run-matrix CC='g++ -pipe -save-temps=obj' GRAPHICS=GRAPHICS_API_OPENGL_43`
  - PASS: `ALL PASS`.
- Focused Task 3 executable covering persisted bytes, translation `[3,7,11]`,
  `pack_instance`, and Vulkan near/far NDC `[0,1]`:
  - PASS, exit 0.
- Focused production compilation of `matrix_math.o`, `frame_matrices.o`,
  `part_store.o`, `world_composer.o`, `resolvers.o`, `gpu_culler.o`, and
  `matter_engine.o` under UCRT64:
  - PASS, exit 0.
- Focused compilation of `gpu_cull_tests.cpp` and `release_part_tests.cpp`:
  - PASS, exit 0.
- Search over compiled render/provider/session and migrated tests:
  - no `transpose_to_gl`, `make_lookat`, `make_perspective`,
    `camera_frustum_planes_raw`, or old frustum extractor references.

The complete requested legacy command could not run in this native environment:

- `run-viewer-logic` is blocked during compilation by the existing use of POSIX
  `realpath` on UCRT64.
- `run-partstore`/`run-comp` use Linux link flags on non-Linux/non-macOS builds
  (`-lGL -ldl -lrt -lX11`), which are unavailable to MinGW. Their C compilation
  also requires `-save-temps=obj` because the sandbox denies `C:\msys64\tmp`.
- `run-gpucull` additionally requires a live GL 4.6 context, unavailable here;
  its changed test translation unit compiled successfully instead.

## Self-review and staging

- Persisted arrays are copied into/out of `Mat4f`; no serialization writer or
  placement layout changed.
- Camera construction uses one `build_frame_matrices` result and passes its
  `world_to_clip` and `frustum_planes` together.
- Expanded GPU-culler transforms remain canonical CPU matrices; packing occurs
  only in `GpuInstanceRec` and the GLSL uniform upload.
- No Vulkan SDK headers or libraries were introduced.
- Pre-existing dirty hunks in `matter_engine.cpp`, `part_store.cpp`, and
  `MatterEngine3/Makefile` were excluded with patch staging. All other unrelated
  dirty files remain unstaged.

## Review fix: Vulkan-ZO RT unprojection

Review identified that both active OptiX ray-generation kernels still converted
the sampled `[0,1]` depth to OpenGL NDC `[-1,1]`, even though Task 3 now supplies
the inverse Vulkan-ZO `world_to_clip`. Data-flow inspection confirmed
`depth_linearize.comp` copies the window-depth sample unchanged.

TDD evidence:

- Added a `run-matrix` source gate requiring both active kernels to contain
  `float ndc_z = z_ndc;`.
- RED: `run-matrix` reported exactly two failures, one for each kernel.
- Minimal fix: changed only the two `ndc_z` assignments; x/y reconstruction and
  the projection contract were left unchanged.
- GREEN: fresh `make -B -C MatterEngine3/tests run-matrix ...` returned
  `ALL PASS`.

Additional verification:

- Fresh UCRT64 compilation of `matrix_math.o`, `frame_matrices.o`, and
  `matter_engine.o`: exit 0.
- CUDA 13.3 + OptiX 8.1 direct PTX compilation of both `shadow_raygen.cu` and
  `lighting_raygen.cu`: exit 0. Both emitted only the existing nvcc warning that
  the extern constant `params` declaration is treated as a static definition.
- `lighting_raygen.cu` already contained extensive unrelated dirty work. Only
  the one-line Vulkan unprojection hunk was patch-staged; the unrelated CUDA
  edits remain unstaged. `shadow_raygen.cu` contributed only its one-line fix.
