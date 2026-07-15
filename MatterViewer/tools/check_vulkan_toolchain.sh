#!/usr/bin/env sh
set -eu

: "${WIN_CXX:=/ucrt64/bin/g++}"
: "${VULKAN_INCLUDE:=/ucrt64/include}"
: "${VULKAN_LIB_DIR:=/ucrt64/lib}"
: "${GLSLC:=/ucrt64/bin/glslc}"
: "${CUDA_PATH:=/c/PROGRA~1/NVIDIA~2/CUDA/v13.3}"
: "${OPTIX_PATH:=/c/PROGRA~3/NVIDIA~1/OPTIXS~1.0}"
: "${HAVE_STREAMLINE:=0}"
: "${STREAMLINE_PATH:=}"
: "${STREAMLINE_DLL_DIR:=$STREAMLINE_PATH/bin/x64/development}"

missing=0
require_command() {
    if ! command -v "$2" >/dev/null 2>&1; then
        printf 'ERROR: missing %s: %s\n' "$1" "$2" >&2
        missing=1
    fi
}
require_file() {
    if ! test -f "$2"; then
        printf 'ERROR: missing %s: %s\n' "$1" "$2" >&2
        missing=1
    fi
}

require_command 'Windows C++ compiler' "$WIN_CXX"
require_command 'Vulkan shader compiler' "$GLSLC"
require_file 'Vulkan header' "$VULKAN_INCLUDE/vulkan/vulkan.h"
require_file 'Vulkan import library' "$VULKAN_LIB_DIR/libvulkan-1.dll.a"
require_file 'CUDA header' "$CUDA_PATH/include/cuda.h"
require_file 'OptiX header' "$OPTIX_PATH/include/optix.h"
case "$HAVE_STREAMLINE" in
    0) ;;
    1)
        require_file 'Streamline header' "$STREAMLINE_PATH/include/sl.h"
        require_file 'Streamline Vulkan helper header' \
            "$STREAMLINE_PATH/include/sl_helpers_vk.h"
        require_file 'Streamline core API header' \
            "$STREAMLINE_PATH/include/sl_core_api.h"
        require_file 'Streamline constants header' \
            "$STREAMLINE_PATH/include/sl_consts.h"
        require_file 'Streamline DLSS header' \
            "$STREAMLINE_PATH/include/sl_dlss.h"
        require_file 'Streamline security header' \
            "$STREAMLINE_PATH/include/sl_security.h"
        require_file 'signed Streamline interposer DLL' \
            "$STREAMLINE_DLL_DIR/sl.interposer.dll"
        require_file 'Streamline DLSS plugin DLL' \
            "$STREAMLINE_DLL_DIR/sl.dlss.dll"
        require_file 'NVIDIA DLSS runtime DLL' \
            "$STREAMLINE_DLL_DIR/nvngx_dlss.dll"
        ;;
    *)
        printf 'ERROR: HAVE_STREAMLINE must be 0 or 1, got: %s\n' \
            "$HAVE_STREAMLINE" >&2
        missing=1
        ;;
esac
test "$missing" -eq 0 || exit 1

src=
exe=
trap 'rm -f "$src" "$exe"' EXIT HUP INT TERM
src="$(mktemp --suffix=.cpp)"
exe="$(mktemp --suffix=.exe)"
printf '#include <vulkan/vulkan.h>\nint main(){return vkEnumerateInstanceVersion(0);}\n' > "$src"
"$WIN_CXX" "$src" -I"$VULKAN_INCLUDE" -L"$VULKAN_LIB_DIR" -lvulkan-1 -o "$exe"
printf 'vulkan-preflight: OK CUDA=1 OPTIX=1\n'
