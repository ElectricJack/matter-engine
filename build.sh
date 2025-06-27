#!/bin/bash
# Build script for GPURayTraceExample with cross-platform support

set -e  # Exit on any error

echo "Building GPURayTraceExample..."

# Detect current platform
UNAME_S=$(uname -s)
case "${UNAME_S}" in
    Linux*)     PLATFORM=linux;;
    Darwin*)    PLATFORM=macos;;
    CYGWIN*)    PLATFORM=windows;;
    MINGW*)     PLATFORM=windows;;
    *)          PLATFORM="unknown:${UNAME_S}"
esac

echo "Detected platform: $PLATFORM"

# Build the project
echo "Building project for $PLATFORM..."
make clean
make

echo "Build completed successfully!"
echo "Platform: $PLATFORM"
echo "Executable: ./build/$PLATFORM/gpu_raytrace"

# Create a symlink for easy access (platform-agnostic)
if [ -f "./build/$PLATFORM/gpu_raytrace" ]; then
    rm -f ./gpu_raytrace
    ln -sf "./build/$PLATFORM/gpu_raytrace" ./gpu_raytrace
    echo "Symlink created: ./gpu_raytrace -> ./build/$PLATFORM/gpu_raytrace"
fi 