# Standalone graphics acceptance on Linux

Task `clear-crest-48.4.4`, claim epoch 34. Tested source:
`e2bf6305115ff05101b1d8b4e193a25cc881a18e`, after guarded `aq git pull --branch dev`.
The latest supervisor task directive removes Windows CI as a completion
requirement and requests applicable Linux verification. Prior Windows receipts
remain historical evidence at their recorded sources.

A previously nonexistent `/tmp/matter-graphics-e34` build completed all 72
steps. All 12 unchanged CTests passed in 45.24 seconds, with Vulkan validation
required for device fixtures and sample teardown. Device: llvmpipe (LLVM 20.1.2,
256 bits), Mesa 25.2.8, Vulkan 1.4.318. No implementation or assertions changed.

The Linux host lacked glslc and validation layers. Ubuntu noble packages
`glslc`, `libshaderc1` and `vulkan-validationlayers` were downloaded with
`apt-get download` and extracted with `dpkg-deb -x` into
`/tmp/graphics-e34-toolchain/root`; no system installation was needed.
Package versions and SHA-256 hashes are in `toolchain.txt`.

Reproduce from the repository root after staging those packages at that path:

```bash
export LD_LIBRARY_PATH=/tmp/graphics-e34-toolchain/root/usr/lib/x86_64-linux-gnu
export VK_LAYER_PATH=/tmp/graphics-e34-toolchain/root/usr/share/vulkan/explicit_layer.d
export MATTER_GRAPHICS_REQUIRE_VALIDATION=1
cmake -S libs/MatterGraphics -B /tmp/matter-graphics-e34 -G Ninja \
  -DBUILD_TESTING=ON -DMATTER_GRAPHICS_VULKAN=ON -DCMAKE_BUILD_TYPE=Debug \
  -DVulkan_INCLUDE_DIR="$PWD/third_party/Vulkan-Headers/include" \
  -DVulkan_LIBRARY=/usr/lib/x86_64-linux-gnu/libvulkan.so.1 \
  -DMATTER_GRAPHICS_GLSLC=/tmp/graphics-e34-toolchain/root/usr/bin/glslc
cmake --build /tmp/matter-graphics-e34 --parallel 2
ctest --test-dir /tmp/matter-graphics-e34 --output-on-failure -j 1 \
  --output-junit /tmp/graphics-e34-ctest.xml
```

`build.log`, `ctest.log`, `ctest.xml` and `fixtures.log` preserve original
results. `GFX-IMPORTED-RASTER` covers repeated/mirrored placements, normals,
materials, depth and failure behavior. `GFX-SAMPLE` covers imported and
synthetic assets, shared instances, camera/light state, updates and release.
Four views each have actual, repeat and albedo PNGs, converted losslessly from
the CTest sample's PPM captures. `capture-hashes.json` records both formats;
all four actual/repeat pairs match exactly. Repeat agreement establishes
reproducibility; the existing fixture assertions supply independent pixel
and lifetime checks. No Windows/Linux image parity claim is made.

`linkage.json` exports the actual CMake File API's 22 compiled targets,
sources, dependencies and link fragments. `GFX-INDEPENDENT-GRAPH` passed the
source/link/compiler-header/shader closure audit with procedural, terrain and
script-host targets absent. `ldd.txt` shows Vulkan and standard Linux/C++
runtime dependencies. `vulkaninfo.log` identifies the device and staged layer.

This epoch adds evidence only: zero moved or newly written implementation
lines. The delivered sample extraction retains its original accounting of
246 moved lines, 152 adapter/build lines added and five other lines removed.
Its sample file over 50 lines contains facade/pass/camera/light/capture/lifetime
wiring; the native device and imported acceptance headers retain relocated
implementations. The new JSON/log files over 50 lines are captured evidence.

`aq prime` failed with `context.required_over_budget`; the full own-task
description/completion, latest comments and both required component vault
specifications were read directly before work. This infrastructure error does
not change the recorded test outcomes.
