"""Usage: python3 -I gen_linux_smoke_cmake.py <repo> <out-dir>; then
cmake -S <out-dir> -B <build> -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C <build>.
Needs MatterEditor/build/linux/libglfw3.a (make -C MatterEditor build/linux/libglfw3.a).

Generate a Linux CMake project for vulkan_smoke_tests from the repo's
canonical MSVC source lists (cmake/MatterViewer.cmake, MatterTargets.cmake)."""
import re, sys, pathlib
root = pathlib.Path(sys.argv[1]); out = pathlib.Path(sys.argv[2])
viewer = (root / 'cmake/MatterViewer.cmake').read_text()
def block(name):
    m = re.search(r'set\(%s\s*(.*?)\)' % re.escape(name), viewer, re.S)
    return m.group(1).split()
srcs = []
for tok in block('matter_vulkan_cpu_contract_sources') + block('matter_vulkan_smoke_sources'):
    if tok.startswith('${'):
        continue
    srcs.append(tok)
srcs += ['MatterEngine3/tests/vulkan_smoke_tests.cpp', 'MatterEditor/src/ui_lighting_controls.cpp']
# Link libraries' sources (MatterTargets.cmake / MatterThirdParty.cmake equivalents).
srcs += ['libs/MemoryLib/src/mem_bank.c', 'libs/MemoryLib/src/mem_arena.c',
         'libs/MemoryLib/src/mem_array.c',
         'libs/AssetStoreLib/src/store_hash.cpp', 'libs/AssetStoreLib/src/store_os.cpp',
         'libs/AssetStoreLib/src/blob_store.cpp', 'libs/AssetStoreLib/src/ref_table.cpp',
         'libs/AssetStoreLib/src/asset_pages.cpp',
         'third_party/flecs/flecs.c', 'third_party/bc7enc/bc7enc.cpp', 'third_party/bc7enc/rgbcx.cpp']
srcs += [str(p.relative_to(root)) for p in sorted((root/'libs/ProfileLib/src').glob('*.cpp'))]
srcs += [str(p.relative_to(root)) for p in sorted((root/'libs/MeshChartingLib/src').glob('*.cpp'))]
srcs += [str(p.relative_to(root)) for p in sorted((root/'libs/SpatialQueryLib/src').glob('*.c*'))]
seen = []
for s in srcs:
    if s not in seen:
        seen.append(s)
missing = [s for s in seen if not (root / s).exists()]
assert not missing, missing
incs = '''MatterEngine3 MatterEngine3/include MatterEngine3/src MatterEngine3/src/render
MatterEngine3/src/provider libs/MatterSurfaceLib/include libs/MatterSurfaceLib/src
libs/SpatialQueryLib/include libs/MemoryLib/include libs/MathLib/include libs/ProfileLib/include
libs/ParticleFlowLib/include libs/MeshChartingLib/include third_party/quickjs-ng third_party/raylib/src
third_party/box3d/include third_party/bc7enc third_party/flecs third_party/ozz-animation/include
third_party/Vulkan-Headers/include MatterEngine3/tests third_party/raylib/src/external/glfw/include
libs/AssetStoreLib/include libs/AssetStoreLib/src'''.split()
lines = ['cmake_minimum_required(VERSION 3.20)', 'project(rt_counter_smoke C CXX)',
         'set(CMAKE_CXX_STANDARD 17)', 'set(CMAKE_C_STANDARD 17)', 'set(CMAKE_C_EXTENSIONS ON)',
         f'set(R "{root}")',
         'add_executable(vulkan_smoke_tests', *[f'  "${{R}}/{s}"' for s in seen], ')',
         'target_include_directories(vulkan_smoke_tests PRIVATE', *[f'  "${{R}}/{i}"' for i in incs], ')',
         'target_compile_definitions(vulkan_smoke_tests PRIVATE PLATFORM_DESKTOP '
         'MATTER_VK_TEST_FAULT_INJECTION MATTER_VULKAN_ONLY MATTER_HAVE_STREAMLINE=0)',
         'target_compile_options(vulkan_smoke_tests PRIVATE -O2 -Wall -Wextra -Wno-missing-field-initializers)',
         f'target_link_libraries(vulkan_smoke_tests PRIVATE "${{R}}/MatterEditor/build/linux/libglfw3.a" '
         '/usr/lib/x86_64-linux-gnu/libvulkan.so.1 X11 dl pthread m rt)']
# gpu_visual_mesher_vk_tests.cpp calls the Windows-only _putenv_s.
shim = out / 'putenv_shim.h'
shim.write_text('#pragma once\n#include <stdlib.h>\n'
                'static inline int _putenv_s(const char* n, const char* v) '
                '{ return setenv(n, v, 1); }\n')
lines += ['set_source_files_properties("${R}/MatterEngine3/tests/gpu_visual_mesher_vk_tests.cpp"',
          f'  PROPERTIES COMPILE_OPTIONS "-include;{shim}")']
(out / 'CMakeLists.txt').write_text('\n'.join(lines) + '\n')
print(len(seen), 'sources')
