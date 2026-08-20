#!/usr/bin/env bash
# Build MatterEditor for Windows with NVIDIA DLSS (Streamline) enabled.
#
#   ./build-dlss.sh              build (incremental)
#   ./build-dlss.sh clean        wipe build/ trees first, then build
#   ./build-dlss.sh dist         build, then stage a shareable package
#
# Override the SDK location if it is not the default:
#   STREAMLINE_PATH=/d/SDKs/streamline-sdk-v2.13.0 ./build-dlss.sh
#
# Run from an MSYS2 UCRT64 shell, from the repository root.
set -euo pipefail

STREAMLINE_PATH="${STREAMLINE_PATH:-/d/SDKs/streamline-sdk-v2.12.0}"
# NOTE: the SDK ships two DLL sets. `development` (18 dlls) adds sl.imgui and
# sl.nvperf and is what this project has always shipped; `bin/x64` (16) is the
# release set. MatterEditor/Makefile still defaults to the release path, so this
# is passed explicitly rather than relied upon.
STREAMLINE_DLL_DIR="${STREAMLINE_DLL_DIR:-$STREAMLINE_PATH/bin/x64/development}"

# MSYS2's make clobbers the Windows TEMP variable, which makes GCC fail with
# "Cannot create temporary file in C:\WINDOWS". Exporting is not enough -- these
# have to be passed as make VARIABLES on every invocation.
# GCC needs a WINDOWS-form path here, not an MSYS one, hence cygpath -m. Try the
# variables MSYS2 already sets, and VERIFY the directory exists -- deriving one
# from $HOME is wrong under `env bash`, where HOME can be /home/<user> rather
# than the Windows profile, yielding C:\msys64\home\... which does not exist.
if [ -z "${TMPDIR_WIN:-}" ]; then
    for _cand in "${TEMP:-}" "${TMP:-}" "${LOCALAPPDATA:+$LOCALAPPDATA/Temp}" /tmp; do
        [ -n "$_cand" ] || continue
        _u="$(cygpath -u "$_cand" 2>/dev/null)" || continue
        [ -d "$_u" ] || continue
        TMPDIR_WIN="$(cygpath -m "$_u")"
        break
    done
fi
[ -n "${TMPDIR_WIN:-}" ] && [ -d "$(cygpath -u "$TMPDIR_WIN" 2>/dev/null)" ] \
    || { echo "ERROR: no usable temp dir; set TMPDIR_WIN to a Windows path" >&2; exit 1; }
MAKE_TMP=(TMP="$TMPDIR_WIN" TEMP="$TMPDIR_WIN")

export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"

MODE="${1:-build}"

say() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
die() { printf '\nERROR: %s\n' "$*" >&2; exit 1; }

# --- preflight -------------------------------------------------------------
[ -f MatterEditor/Makefile ] || die "run this from the repository root"

[ -d "$STREAMLINE_PATH" ] || die "Streamline SDK not found: $STREAMLINE_PATH
  Set STREAMLINE_PATH to your local, licensed SDK install."

for h in sl.h sl_helpers_vk.h sl_core_api.h sl_consts.h sl_dlss.h sl_security.h; do
    [ -f "$STREAMLINE_PATH/include/$h" ] || die "missing SDK header: include/$h"
done

[ -d "$STREAMLINE_DLL_DIR" ] || die "Streamline DLL dir not found: $STREAMLINE_DLL_DIR"
for d in sl.interposer.dll sl.dlss.dll; do
    [ -f "$STREAMLINE_DLL_DIR/$d" ] || die "missing runtime DLL: $d in $STREAMLINE_DLL_DIR"
done

# The MatterEngine3 embedded-shaders step consumes this generated-but-committed
# file directly (MatterEngine3/Makefile's MSL_SHADER_DIR points straight at
# this directory). If it has gone missing the build fails late with a
# confusing "No rule to make target" -- catch it here.
PROCESSED=libs/MatterSurfaceLib/shaders/raytrace_tlas_blas_processed.fs
[ -f "$PROCESSED" ] || die "$PROCESSED is missing.
  It is committed but generated. Restore it with:
      git checkout -- $PROCESSED
  (Regenerating needs MatterSurfaceLib's shader_preprocessor, which does not
  build on stock MSYS2 UCRT64.)"

say "Streamline SDK : $STREAMLINE_PATH"
echo "  DLLs         : $STREAMLINE_DLL_DIR ($(ls "$STREAMLINE_DLL_DIR"/*.dll | wc -l) files)"

SL_FLAGS=(HAVE_STREAMLINE=1
          STREAMLINE_PATH="$STREAMLINE_PATH"
          STREAMLINE_DLL_DIR="$STREAMLINE_DLL_DIR")

# --- clean -----------------------------------------------------------------
# Deliberately does NOT clean libs/MatterSurfaceLib: its build needs
# x86_64-w64-mingw32-g++-posix, which stock MSYS2 UCRT64 does not have, so a
# clean there cannot be undone by a rebuild.
if [ "$MODE" = "clean" ]; then
    say "Cleaning build trees"
    for p in MatterEngine3 MatterEngine3/tests MatterEditor \
             libs/MemoryLib libs/SpatialQueryLib libs/ParticleFlowLib libs/MeshChartingLib; do
        make -C "$p" clean "${MAKE_TMP[@]}" >/dev/null 2>&1 || true
        echo "  cleaned $p"
    done
fi

# --- build -----------------------------------------------------------------
say "Building kernel (libmatter_engine3.a)"
make -C MatterEngine3 "${MAKE_TMP[@]}" -j"$(nproc)"

say "Building MatterEditor (Vulkan + DLSS)"
make -C MatterEditor windows "${SL_FLAGS[@]}" "${MAKE_TMP[@]}"

BIN=MatterEditor/build/windows/editor.exe
[ -f "$BIN" ] || die "build reported success but $BIN is missing"

# --- verify ----------------------------------------------------------------
say "Result"
echo "  binary : $BIN ($(stat -c%s "$BIN") bytes)"
echo "  dlls   : $(ls MatterEditor/build/windows/*.dll 2>/dev/null | wc -l) staged alongside"
if strings "$BIN" 2>/dev/null | grep -q 'active_dlss_mode'; then
    echo "  dlss   : compiled in (found DLSS symbols)"
else
    echo "  dlss   : *** NOT FOUND in binary -- built without MATTER_HAVE_STREAMLINE? ***"
fi

if [ "$MODE" = "dist" ]; then
    say "Staging distributable"
    make -C MatterEditor dist "${SL_FLAGS[@]}" "${MAKE_TMP[@]}"
fi

say "Done"
echo "  Run it from the MatterEditor directory (asset lookup is cwd-relative):"
echo "      cd MatterEditor && ./build/windows/editor.exe"
