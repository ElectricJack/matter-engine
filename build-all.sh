#!/bin/bash
# Build every project in the repo.
#
# Usage:
#   ./build-all.sh          # build everything
#   ./build-all.sh clean    # clean every project, then build
#   ./build-all.sh test     # build, then run headless test suites
#
# Per project, a successful build leaves a runnable binary under build/ (or
# build/<platform>/ for the projects with real platform variants). Failures
# don't abort the run -- the script prints a per-project summary at the end
# and exits non-zero if anything failed.

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
    libs/MemoryLib
    libs/ParticleFlowLib
    libs/SpatialQueryLib
    MatterEngine3
    MatterEditor
)

# Projects whose Makefile defaults to Windows cross-compile and need
# WSL_LINUX=1 to build the native Linux binary.
WSL_LINUX_PROJECTS=(
    libs/MatterSurfaceLib
)

# Projects that use TARGET=linux instead.
#
# MatterEditor is deliberately NOT listed here even though
# MatterEditor/Makefile grew a `linux` target in Phase 5b
# (docs/superpowers/plans/2026-07-25-mathlib-and-raylib-removal.md). That
# target is unverified -- written with no Linux toolchain available to
# compile or run it (see MatterEditor/README.md) -- and MatterEditor's
# Makefile does not accept a TARGET=linux flag anyway; this loop would need
# `build_one MatterEditor "linux"` (a literal make target name, not
# TARGET=linux) once someone has confirmed the `linux` target actually
# builds on a real machine. MatterEditor is a SIMPLE_PROJECTS entry above,
# so a bare `make` there still resolves to `.DEFAULT_GOAL := windows`
# regardless of $PLATFORM -- unaffected by this script running on Linux/WSL.
TARGET_LINUX_PROJECTS=(
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
    ( cd third_party/raylib/src && make clean PLATFORM=PLATFORM_DESKTOP >/dev/null 2>&1 || true
      make PLATFORM=PLATFORM_DESKTOP CUSTOM_CFLAGS="$RAYLIB_CUSTOM_CFLAGS" >/dev/null 2>&1 )
    # Some projects look in build/<platform>/libraylib.a -- mirror it there too.
    mkdir -p "third_party/raylib/build/$PLATFORM"
    cp third_party/raylib/src/libraylib.a "third_party/raylib/build/$PLATFORM/libraylib.a"
}

# autoremesher_core: static library (geogram core + hexdom + isotropicremesher
# + autoremesher pipeline). Built once here so MSL / MatterEngine3 can link it.
# Builds on every platform and needs no companion runtime library -- its TBB
# replacement is header-only (see third_party/autoremesher_core/thirdparty/tbb_shim/).
prep_autoremesher_core() {
    echo "Building autoremesher_core static lib..."
    ( cd third_party/autoremesher_core && make >/dev/null 2>&1 )
    if [ -f third_party/autoremesher_core/libautoremesher_core.a ]; then
        echo "  autoremesher_core: OK"
    else
        echo "  autoremesher_core: FAIL"
    fi
}

if [ "$MODE" = "clean" ]; then
    for p in "${ALL_PROJECTS[@]}"; do clean_one "$p"; done
    ( cd third_party/raylib/src && make clean PLATFORM=PLATFORM_DESKTOP >/dev/null 2>&1 || true )
    ( cd third_party/autoremesher_core && make clean >/dev/null 2>&1 || true )
    echo "All projects cleaned."
fi

prep_raylib
prep_autoremesher_core

for p in "${SIMPLE_PROJECTS[@]}";        do build_one "$p" ""; done
for p in "${WSL_LINUX_PROJECTS[@]}";     do build_one "$p" "WSL_LINUX=1"; done
for p in "${TARGET_LINUX_PROJECTS[@]}";  do build_one "$p" "TARGET=linux"; done

if [ "$MODE" = "test" ]; then
    echo
    echo "============================================================"
    echo "  Running headless test suites"
    echo "============================================================"

    # Phase A: grep-gate — app projects must not include engine internals.
    echo
    echo "--- grep-gate (MatterEditor dependency rule) ---"
    bash MatterEngine3/tools/grep_gate.sh || RESULT[MatterEditor]="FAIL (grep-gate)"

    for proj in libs/MemoryLib libs/SpatialQueryLib; do
        base="$(basename "$proj")"
        bin="$proj/build/$(echo "$base" | tr '[:upper:]' '[:lower:]')"
        if [ -x "$bin" ]; then
            echo
            echo "--- $proj ---"
            "$bin" || RESULT[$proj]="FAIL (tests)"
        fi
    done

    echo
    echo "--- MemoryLib (memory_tests + memory_hpp_tests) ---"
    make -C libs/MemoryLib test || RESULT[libs/MemoryLib]="FAIL (tests)"

    echo
    echo "--- ParticleFlowLib (pf_tests, ASan+UBSan) ---"
    make -C libs/ParticleFlowLib test || RESULT[libs/ParticleFlowLib]="FAIL (tests)"

    # MatterSurfaceLib headless suites (no GL window). Target name == binary name.
    # mesh_indexed_tests, mesh_transform_tests are Phase 5 additions (Task 2/3).
    for suite in mesh_simplifier_tests material_registry_tests cell_bounds_tests \
                 blas_refcount_tests mesh_continuity_tests blas_tint_tests \
                 particle_culling_tests voxel_imposter_tests \
                 mesh_indexed_tests mesh_transform_tests mesh_smooth_tests; do
        if make -C libs/MatterSurfaceLib/tests "build/$suite" >/dev/null 2>&1; then
            echo
            echo "--- MatterSurfaceLib ($suite) ---"
            "libs/MatterSurfaceLib/tests/build/$suite" || RESULT[libs/MatterSurfaceLib]="FAIL (tests)"
        else
            RESULT[libs/MatterSurfaceLib]="FAIL (test build)"
        fi
    done

    # MSL mesh_retopo_tests (Phase 5 Task 6) — links libautoremesher_core.a.
    # Only runs if the vendored lib built; otherwise skip cleanly (autoremesher_core
    # is optional, prep_autoremesher_core above just warns "FAIL" and moves on).
    if [ -f third_party/autoremesher_core/libautoremesher_core.a ]; then
        if make -C libs/MatterSurfaceLib/tests build/mesh_retopo_tests >/dev/null 2>&1; then
            echo
            echo "--- MatterSurfaceLib (mesh_retopo_tests) ---"
            libs/MatterSurfaceLib/tests/build/mesh_retopo_tests || RESULT[libs/MatterSurfaceLib]="FAIL (tests)"
        else
            RESULT[libs/MatterSurfaceLib]="FAIL (test build)"
        fi
    else
        echo
        echo "--- MatterSurfaceLib (mesh_retopo_tests) SKIPPED (no libautoremesher_core.a) ---"
    fi

    if make -C libs/MeshChartingLib/tests build/mesh_charting_tests >/dev/null 2>&1; then
        echo; echo "--- MeshChartingLib (mesh_charting_tests) ---"
        ./libs/MeshChartingLib/tests/build/mesh_charting_tests || RESULT[libs/MeshChartingLib]="FAIL (tests)"
    else
        RESULT[libs/MeshChartingLib]="FAIL (test build)"
    fi

    if make -C libs/MathLib/tests build/mathlib_tests >/dev/null 2>&1; then
        echo; echo "--- MathLib (mathlib_tests) ---"
        ./libs/MathLib/tests/build/mathlib_tests || RESULT[libs/MathLib]="FAIL (tests)"
    else
        RESULT[libs/MathLib]="FAIL (test build)"
    fi

    # MatterEngine3 headless suites (script host + voxel-CSG bake; GL-free host,
    # raylib-linked BLAS path). Each run-* target builds then runs its binary, so
    # a non-zero status covers both build and test failures. run-graph-integration
    # exercises the full SP-3 install -> SP-2 ScriptHost bake path end-to-end.
    for tgt in run-ecs run-physics run-partv2 run-script run-iso run-graph run-graph-integration run-trivar run-polytri run-shlib run-comp run-flatten run-dev run-example run-gallery run-treebake run-viewer-logic run-grasslod run-stressforest run-tilesetphysics run-tilesetcore run-tilesetplacement run-tilesetdsl run-tilesetbake run-tilesetgtex run-tilesetvkrepack run-tilesetvkao run-tilesetvkhorizon run-tilesettorusbvh run-tilesetmeadowmanifest run-shader-source run-asyncq run-liveprod run-modapply run-modbake; do
        echo
        echo "--- MatterEngine3 ($tgt) ---"
        make -C MatterEngine3/tests "$tgt" || RESULT[MatterEngine3]="FAIL ($tgt)"
    done

    # Phase 5 retopo end-to-end integration (Task 14) — links autoremesher_core.
    # Same gate as the MSL mesh_retopo suite above: skip cleanly if the vendored
    # static lib wasn't built (e.g. clean checkout with no autoremesher_core).
    if [ -f third_party/autoremesher_core/libautoremesher_core.a ]; then
        echo
        echo "--- MatterEngine3 (run-retopo-integration) ---"
        make -C MatterEngine3/tests run-retopo-integration \
            || RESULT[MatterEngine3]="FAIL (run-retopo-integration)"
    else
        echo
        echo "--- MatterEngine3 (run-retopo-integration) SKIPPED (no libautoremesher_core.a) ---"
    fi

    # Viewer GPU tests — GL 4.6 required. On WSLg this needs GALLIUM_DRIVER=d3d12.
    # We infer availability by (a) the env var being set OR (b) glxinfo reporting >=4.6.
    can_gpu=0
    if [ "${GALLIUM_DRIVER:-}" = "d3d12" ]; then can_gpu=1
    elif command -v glxinfo >/dev/null 2>&1; then
        if glxinfo 2>/dev/null | grep -q "OpenGL core profile version string:.* 4\.[6-9]\|OpenGL core profile version string:.* [5-9]\."; then
            can_gpu=1
        fi
    fi

    if [ "$can_gpu" -eq 1 ]; then
        # run-tilesetgpu / run-tilesetseam / run-tilesetload retired in V5 with
        # the GL ray-tracing .gtex bake (docs/vulkan-rt-gtex-bake.md §I.8). The
        # bake is Vulkan-only now; its CPU-side gates run headless above
        # (run-tilesetvkrepack / run-tilesetvkao / run-tilesetvkhorizon) and the
        # device-side gate is run-vk-scene-renderer.
        echo
        echo "--- MatterEngine3/tests (tileset-provider-tests) ---"
        make -C MatterEngine3/tests run-tilesetprovider || RESULT[MatterEngine3]="FAIL (run-tilesetprovider)"

        echo
        echo "--- MatterEngine3/tests (api-tests) ---"
        make -C MatterEngine3/tests api-tests || RESULT[MatterEngine3]="FAIL (api-tests build)"
        ( cd MatterEditor && GALLIUM_DRIVER=d3d12 ../MatterEngine3/tests/build/api_tests ) \
            || RESULT[MatterEngine3]="FAIL (api-tests run)"

        echo
        echo "--- MatterEngine3/tests (run-asyncbake) ---"
        # Phase B: async-bake-tests links GPU objects (needs embedded_shaders.h) but
        # runs headless (allow_gl_lt_46=true; no window). GALLIUM_DRIVER not needed at
        # runtime but the target must build in the GPU-capable environment.
        make -C MatterEngine3/tests run-asyncbake || RESULT[MatterEngine3]="FAIL (run-asyncbake)"

        echo
        echo "--- MatterEngine3/tools (meadow_forestfloor_shots) ---"
        bash MatterEngine3/tools/meadow_forestfloor_shots.sh /tmp/build_all_ff_shots \
            || RESULT[MatterEngine3]="FAIL (meadow_forestfloor_shots)"

    else
        echo
        echo "--- MatterEngine3/tests GPU tests SKIPPED (needs GL 4.6 + GALLIUM_DRIVER=d3d12) ---"
    fi
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
