#!/usr/bin/env bash
# Build the pinned ozz runtime used by Matter animation adapters.
#
# WINDOWS: run this from PowerShell/cmd, not an MSYS2 bash shell:
#   $env:PATH = "C:\msys64\ucrt64\bin;" + $env:PATH
#   cmake -S third_party/ozz-animation -B third_party/ozz-animation/build/matter -G Ninja ...<flags below>
#   cmake --build third_party/ozz-animation/build/matter --target ozz_base ozz_animation ozz_animation_offline --parallel
# Under MSYS2 bash, CMake's compiler probe fails with "Cannot create temporary
# file in C:\WINDOWS\": ninja does not inherit a usable TMP/TEMP through the
# MSYS->native environment conversion, and exporting TMP/TEMP does not fix it
# (unlike the plain `make` builds, which take TMP=/TEMP= as make variables).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ozz_source="${repo_root}/third_party/ozz-animation"
ozz_build="${ozz_source}/build/matter"

cmake -S "${ozz_source}" -B "${ozz_build}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF \
    -Dozz_build_tests=OFF \
    -Dozz_build_samples=OFF \
    -Dozz_build_howtos=OFF \
    -Dozz_build_tools=OFF \
    -Dozz_build_gltf=OFF \
    -Dozz_build_fbx=OFF \
    -Dozz_build_postfix=OFF
cmake --build "${ozz_build}" --target \
    ozz_base ozz_animation ozz_animation_offline --parallel
