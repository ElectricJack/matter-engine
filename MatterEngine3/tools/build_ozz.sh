#!/usr/bin/env bash
# Build the pinned ozz runtime used by Matter animation adapters.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ozz_source="${repo_root}/Libraries/ozz-animation"
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
