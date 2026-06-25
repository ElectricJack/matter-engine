#!/bin/bash
# Build every project in the repo.
#
# Usage:
#   ./build-all.sh          # build everything
#   ./build-all.sh clean    # clean every project, then build
#   ./build-all.sh test     # build, then run headless test suites
#
# Per project, a successful build leaves a runnable binary either in the
# project root or under build/<platform>/. Failures don't abort the run --
# the script prints a per-project summary at the end and exits non-zero
# if anything failed.

set -u

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

MODE="${1:-build}"

# Detect platform
UNAME_S="$(uname -s)"
case "$UNAME_S" in
    Linux*)  PLATFORM=linux ;;
    Darwin*) PLATFORM=macos ;;
    *)       PLATFORM=linux ;;  # WSL/MinGW fall through to Linux build
esac

# Projects that use a flat Makefile (no TARGET= flag).
# MatterEngine3 is a library sub-project: `make` builds libmatter_engine3.a
# (no app binary); its headless test targets run in the test section below.
SIMPLE_PROJECTS=(
    BasicWindowApp
    SurfaceLib
    ObjectAllocatorLib
    SpatialQueryLib
    MatterEngine3
)

# Projects whose Makefile defaults to Windows cross-compile and need
# WSL_LINUX=1 to build the native Linux binary.
WSL_LINUX_PROJECTS=(
    OpenParticleSurfaceLib
    GPURayTraceExample
    MatterSurfaceLib
)

# Projects that use TARGET=linux instead.
TARGET_LINUX_PROJECTS=(
    ParticleDynamicsExample
)

ALL_PROJECTS=( "${SIMPLE_PROJECTS[@]}" "${WSL_LINUX_PROJECTS[@]}" "${TARGET_LINUX_PROJECTS[@]}" )

declare -A RESULT

build_one() {
    local proj="$1"
    local args="$2"
    echo
    echo "============================================================"
    echo "  Building $proj  (make $args)"
    echo "============================================================"
    if ( cd "$proj" && make $args ); then
        RESULT[$proj]="OK"
    else
        RESULT[$proj]="FAIL"
    fi
}

clean_one() {
    local proj="$1"
    echo "  cleaning $proj..."
    ( cd "$proj" && make clean >/dev/null 2>&1 || true )
}

# raylib's intermediate .o files can be stale from a different OS; make
# sure the static lib gets rebuilt for the current platform.
# The raytrace shader samples several textures at once (4 core BVH + 2 voxel
# imposter volumes), which can exceed raylib's default
# RL_DEFAULT_BATCH_MAX_TEXTURE_UNITS of 8. Bump
# it so every sampler gets a slot; otherwise the last texture silently fails to
# bind. Must be defined when raylib itself is compiled (it lives in rlgl).
RAYLIB_CUSTOM_CFLAGS="-DRL_DEFAULT_BATCH_MAX_TEXTURE_UNITS=16"
prep_raylib() {
    echo "Rebuilding raylib for $PLATFORM..."
    ( cd Libraries/raylib/src && make clean PLATFORM=PLATFORM_DESKTOP >/dev/null 2>&1 || true
      make PLATFORM=PLATFORM_DESKTOP CUSTOM_CFLAGS="$RAYLIB_CUSTOM_CFLAGS" >/dev/null 2>&1 )
    # Some projects look in build/<platform>/libraylib.a -- mirror it there too.
    mkdir -p "Libraries/raylib/build/$PLATFORM"
    cp Libraries/raylib/src/libraylib.a "Libraries/raylib/build/$PLATFORM/libraylib.a"
}

if [ "$MODE" = "clean" ]; then
    for p in "${ALL_PROJECTS[@]}"; do clean_one "$p"; done
    ( cd Libraries/raylib/src && make clean PLATFORM=PLATFORM_DESKTOP >/dev/null 2>&1 || true )
    echo "All projects cleaned."
fi

prep_raylib

for p in "${SIMPLE_PROJECTS[@]}";        do build_one "$p" ""; done
for p in "${WSL_LINUX_PROJECTS[@]}";     do build_one "$p" "WSL_LINUX=1"; done
for p in "${TARGET_LINUX_PROJECTS[@]}";  do build_one "$p" "TARGET=linux"; done

if [ "$MODE" = "test" ]; then
    echo
    echo "============================================================"
    echo "  Running headless test suites"
    echo "============================================================"
    for proj in ObjectAllocatorLib SpatialQueryLib; do
        bin="$proj/$(echo "$proj" | tr '[:upper:]' '[:lower:]')"
        # ObjectAllocatorLib's binary is named "objectallocator" (no Lib suffix).
        [ "$proj" = "ObjectAllocatorLib" ] && bin="$proj/objectallocator"
        if [ -x "$bin" ]; then
            echo
            echo "--- $proj ---"
            "$bin" || RESULT[$proj]="FAIL (tests)"
        fi
    done

    # MatterSurfaceLib headless suites (no GL window). Target name == binary name.
    for suite in mesh_simplifier_tests material_registry_tests cell_bounds_tests \
                 blas_refcount_tests mesh_continuity_tests blas_tint_tests \
                 particle_culling_tests voxel_imposter_tests; do
        if make -C MatterSurfaceLib/tests "$suite" >/dev/null 2>&1; then
            echo
            echo "--- MatterSurfaceLib ($suite) ---"
            "MatterSurfaceLib/tests/$suite" || RESULT[MatterSurfaceLib]="FAIL (tests)"
        else
            RESULT[MatterSurfaceLib]="FAIL (test build)"
        fi
    done

    if make -C MeshChartingLib/tests mesh_charting_tests >/dev/null 2>&1; then
        echo; echo "--- MeshChartingLib (mesh_charting_tests) ---"
        ./MeshChartingLib/tests/mesh_charting_tests || RESULT[MeshChartingLib]="FAIL (tests)"
    else
        RESULT[MeshChartingLib]="FAIL (test build)"
    fi

    # MatterEngine3 headless suites (script host + voxel-CSG bake; GL-free host,
    # raylib-linked BLAS path). Each run-* target builds then runs its binary, so
    # a non-zero status covers both build and test failures. run-graph-integration
    # exercises the full SP-3 install -> SP-2 ScriptHost bake path end-to-end.
    for tgt in run-partv2 run-script run-graph run-graph-integration run-trivar run-shlib run-comp run-dev; do
        echo
        echo "--- MatterEngine3 ($tgt) ---"
        make -C MatterEngine3/tests "$tgt" || RESULT[MatterEngine3]="FAIL ($tgt)"
    done
fi

echo
echo "============================================================"
echo "  Summary"
echo "============================================================"
fail=0
for p in "${ALL_PROJECTS[@]}"; do
    r="${RESULT[$p]:-SKIP}"
    printf "  %-25s %s\n" "$p" "$r"
    [ "$r" != "OK" ] && fail=1
done

exit $fail
