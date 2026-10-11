# Production graphics producer move

Task `bright-torrent-52` relocates the production code from base
`58289c187ef2565c7d0532fe28a8cfb903023e44`. The implementation is mechanically
equivalent, but validation is **not all green**: VT and VT-RT smoke failures
remain unresolved. No matching prior failure receipt was found. Source and
SPIR-V equality are evidence of the move, not proof of runtime failure provenance.

## Boundary and provenance

Seven separable files account for 2,241 moved lines: `raster.vert` (444),
`gbuffer.frag` (1,390), `material_common.glsl` (53), `impostor_common.glsl` (95),
`raster_mesh.cpp` (57), `raster_mesh.h` (49), `impostor_mips.h` (153).
One include in the mesh header changes to resolve its existing geometry type.
The old common-shader and mesh/helper paths contain forwarding includes.

Four private fragments account for 1,238 more moved lines: G-buffer recording
(322), raster pipelines (415), impostor atlas (213), and impostor adoption (288).
Each is included exactly once in the existing renderer. Expanding the four
includes reproduces the original renderer bytes exactly. Total: **3,479 moved
lines**, with no new rendering algorithm. See
[host dependencies and source paths](../../libs/MatterGraphics/src/native/README.md).
Three trailing separator lines stay at the host include sites, so the final
move count excludes them. Added integration consists of 57 build/forwarding
lines and 42 test path/dependency adapter lines; documentation and captured
validation evidence are counted separately from implementation.
This boundary retains host ownership; it does not establish standalone producer
APIs or facade integration. Supervisor messages `msg-8e1a9a02b90c4d97bb59409c1be932da`
and `msg-55eae42e0b4f46c4a739e2249e378a62` explicitly accept these boundaries and
prioritize unchanged production behavior over incompatible rewrite fixtures.

New integration code maps physical shader paths to unchanged embedded names,
tracks moved include dependencies, forwards legacy includes and adapts existing
source-inspection tests to those locations. Test assertions remain unchanged.
The independent compositor's dependency audit allows the moved impostor helper;
its native tests still build independently from the engine archive.

## Verification on 2026-10-11

Windows x64 MSVC 2022 RelWithDebInfo, Vulkan SDK 1.4.357.0, NVIDIA RTX 4090,
driver 610.74 / Vulkan API 1.4.341. Commands ran from the assigned WSL worktree.
Native smoke commands set `MATTER_VK_SMOKE_MODE` and
`VK_LAYER_PATH=C:\VulkanSDK\1.4.357.0\Bin`, with working directory `MatterEditor`.

| Check | Result |
| --- | --- |
| `tools/build-windows-from-wsl.sh RelWithDebInfo preflight` | Pass |
| Same build script, targets `vulkan_smoke_tests`, `viewer_logic_tests`, `shader_source_tests` | Pass, including production and fault-injection renderer objects |
| `libs/MatterGraphics/build-windows.ps1 -Config RelWithDebInfo -Test` | All 5 pass: lifetime, scene, composition, native Vulkan, independence |
| Native CTest `-R '^(viewer_logic_tests\|shader_source_tests)$' -j 1 --output-on-failure` | Both pass, 215.46 seconds |
| Native smoke: `raster`, `raster-fault`, `rt-disabled`, `animation-skin`, `vt-surfaces` | All pass; zero validation errors |
| Native smoke: `vt` | Fails 7 fill/feedback assertions; zero validation errors |
| Native smoke: `vt-rt` | Fails 12 page/source-atlas/cone-mip assertions; zero validation errors |
| Source expansion plus moved-file equality | Pass |
| Native glslc byte comparison: raster, G-buffer, skin and water variants | All 4 outputs identical to original sources |

The seven smoke modes ran serially through `aq test -n 0
/tmp/bright-torrent-validation/test_native.py -q`: **5 passed, 2 failed** in
274.86 seconds. Source/shader checks ran through `aq test -n 0
/tmp/bright-torrent-validation/test_move.py -q`: **2 passed** in 94.19 seconds.
The [evidence directory](../agent/evidence/2026-10-11-graphics-producer-move/)
preserves exact executed drivers, full failing native logs, CPU CTest output,
shader hashes and original fixture compilation diagnostics. The drivers retain
this run's absolute paths; adjust `ROOT`/output paths for another checkout.
The glslc comparison uses `--target-env=vulkan1.3 -O`, the existing engine include
directory, and the unchanged skin/water specialization defines. It compares
bytes with the canonical CMake build outputs, including the 1,594,540-byte
G-buffer SPIR-V module.

`vt` records `fills_total=0`, one pinned slot and a nonempty queue after its
fixed seven frames; `vt-rt` cannot resolve expected VT pages. No matching
baseline for those modes was found in repository findings or by the supervisor
(`msg-11719c432f68423ba308d4902b5debd8`). The earlier
[VT-normal-frame finding](streammountain-gbuffer-cache-order-2026-09-29.md)
concerns a different mode and is not a baseline receipt for these failures.
No new runtime baseline, changed assertion, tolerance or unrelated VT fix was
introduced. These failures remain explicit limitations of acceptance.

## Other checks that could not pass

The supplemental Unix `make -C MatterEngine3/tests run-vk-scene-renderer -j2`
first needed the canonical build-tree embedded shader header. Retrying with
`-o ../shaders_gen/embedded_spirv.h -o ../shaders_gen/embedded_shaders.h`
and `CC='g++ -I../../MatterEditor/build/cmake/windows-msvc/relwithdebinfo/MatterEngine3'`
fails at `vk_scene_renderer_tests.cpp:78`: `sizeof(LocalLightGpuMeta) == 32`.
The unchanged header at `vk_scene_renderer.h:1162` asserts 48; both conflicting
assertions are already present in the reference commit. The suite cannot
compile as written; its shader path reads were updated without changing this
unrelated ABI expectation.

The unmodified `.4.5` `vulkan_tests.cpp` and six native fixture headers were
read from `aq/clear-crest-48.4.5` (`38334d91b`). Compiling with `g++ -std=c++17
-fsyntax-only` and current MatterGraphics, MatterContracts, MathLib and vendored
Vulkan header include paths fails at `raster_native.h:2`: missing
`matter/graphics/vulkan_raster.h`. That header is the rejected rewrite API.
`raster_native.h:64-75` also explicitly expects inverse-transpose normal behavior
different from the native `model*n` behavior. The rewritten implementation is
not in this task's ancestry and was not imported. These original fixtures are
**incompatible/unmet**, not passing or silently skipped. Existing composition
fixtures run through the standalone native suite above. No rendering semantic
change was made to satisfy the rejected rewrite's contract.
