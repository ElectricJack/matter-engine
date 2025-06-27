#!/bin/bash
# Platform status script for GPURayTraceExample

echo "=== GPURayTraceExample Platform Status ==="
echo ""

# Detect current platform
UNAME_S=$(uname -s)
case "${UNAME_S}" in
    Linux*)     CURRENT_PLATFORM=linux;;
    Darwin*)    CURRENT_PLATFORM=macos;;
    CYGWIN*)    CURRENT_PLATFORM=windows;;
    MINGW*)     CURRENT_PLATFORM=windows;;
    *)          CURRENT_PLATFORM="unknown:${UNAME_S}"
esac

echo "Current platform: $CURRENT_PLATFORM"
echo ""

# Check build directories
echo "Build Status:"
for platform in linux macos windows; do
    BUILD_DIR="./build/$platform"
    EXECUTABLE="$BUILD_DIR/gpu_raytrace"
    RAYLIB_LIB="../Libraries/raylib/build/$platform/libraylib.a"
    PREPROCESSOR="$BUILD_DIR/shader_preprocessor"
    
    printf "%-10s: " "$platform"
    
    if [ -f "$EXECUTABLE" ]; then
        SIZE=$(du -h "$EXECUTABLE" 2>/dev/null | cut -f1)
        TIMESTAMP=$(stat -f "%Sm" -t "%Y-%m-%d %H:%M" "$EXECUTABLE" 2>/dev/null || stat -c "%y" "$EXECUTABLE" 2>/dev/null | cut -d' ' -f1-2)
        printf "✓ Built ($SIZE, $TIMESTAMP)"
    else
        printf "✗ Not built"
    fi
    
    if [ -f "$RAYLIB_LIB" ]; then
        printf " [Raylib: ✓]"
    else
        printf " [Raylib: ✗]"
    fi
    
    if [ -f "$PREPROCESSOR" ]; then
        printf " [Preprocessor: ✓]"
    else
        printf " [Preprocessor: ✗]"
    fi
    
    if [ "$platform" = "$CURRENT_PLATFORM" ]; then
        printf " ← Current"
    fi
    
    echo ""
done

echo ""

# Check symlink
if [ -L "./gpu_raytrace" ]; then
    TARGET=$(readlink "./gpu_raytrace")
    echo "Symlink: ./gpu_raytrace -> $TARGET"
elif [ -f "./gpu_raytrace" ]; then
    echo "Symlink: ./gpu_raytrace (regular file - not a symlink)"
else
    echo "Symlink: ./gpu_raytrace (not found)"
fi

echo ""

# Check shaders
if [ -f "shaders/raytrace_tlas_blas_processed.fs" ]; then
    echo "Shaders: ✓ Processed"
else
    echo "Shaders: ✗ Not processed"
fi

echo ""
echo "Usage:"
echo "  ./build.sh           - Build for current platform ($CURRENT_PLATFORM)"
echo "  ./run.sh             - Run for current platform"
echo "  make platform        - Show Makefile platform info"
echo "  make clean           - Clean current platform"
echo "  make clean-all       - Clean all platforms"
echo "  make rebuild-raylib  - Force rebuild raylib for current platform"
echo "  make shaders         - Process shaders" 