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
for platform in linux macos windows windows-native; do
    BUILD_DIR="./build/$platform"
    RAYLIB_LIB="../Libraries/raylib/build/$platform/libraylib.a"
    PREPROCESSOR="$BUILD_DIR/shader_preprocessor"
    
    if [ "$platform" = "windows-native" ]; then
        EXECUTABLE="$BUILD_DIR/gpu_raytrace.exe"
        DISPLAY_NAME="win-native"
    else
        EXECUTABLE="$BUILD_DIR/gpu_raytrace"
        DISPLAY_NAME="$platform"
    fi
    
    printf "%-10s: " "$DISPLAY_NAME"
    
    if [ -f "$EXECUTABLE" ]; then
        SIZE=$(du -h "$EXECUTABLE" 2>/dev/null | cut -f1)
        TIMESTAMP=$(stat -c "%y" "$EXECUTABLE" 2>/dev/null | cut -d' ' -f1-2 | cut -d'.' -f1 2>/dev/null || echo "unknown")
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

# Check root executables
echo "Root Directory Executables:"
if [ -f "./gpu_raytrace" ]; then
    SIZE=$(du -h "./gpu_raytrace" 2>/dev/null | cut -f1)
    echo "  ./gpu_raytrace: ✓ ($SIZE)"
else
    echo "  ./gpu_raytrace: ✗"
fi

if [ -f "./gpu_raytrace.exe" ]; then
    SIZE=$(du -h "./gpu_raytrace.exe" 2>/dev/null | cut -f1)
    echo "  ./gpu_raytrace.exe: ✓ ($SIZE)"
else
    echo "  ./gpu_raytrace.exe: ✗"
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
echo "  ./build.sh                    - Build for current platform ($CURRENT_PLATFORM)"
echo "  ./run.sh                      - Run for current platform"
echo "  ./build-cross-compile.sh      - Cross-compile Windows .exe from Linux/WSL"
echo "  TARGET=windows-native make    - Cross-compile using Makefile directly"
echo "  make platform                 - Show Makefile platform info"
echo "  make clean                    - Clean current platform"
echo "  make clean-all                - Clean all platforms"
echo "  make rebuild-raylib           - Force rebuild raylib for current platform"
echo "  make shaders                  - Process shaders" 