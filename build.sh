#!/bin/bash
# Build script for OpenParticleSurfaceLib with cross-platform support

set -e  # Exit on any error

echo "Building OpenParticleSurfaceLib..."

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

# Copy dependencies (in case they get out of sync)
echo "Copying dependencies..."
cp '../SurfaceLib/include/surface.h' include/
cp '../SurfaceLib/include/particle.h' include/
cp '../ObjectAllocatorLib/include/object_allocator.h' include/
cp '../SpatialQueryLib/include/spatial_hash.h' include/

cp '../SurfaceLib/src/surface.c' src/
cp '../ObjectAllocatorLib/src/object_allocator.c' src/
cp '../SpatialQueryLib/src/spatial_hash.c' src/
cp '../SurfaceLib/include/mc_tables.h' src/

# Build the project
echo "Building project for $PLATFORM..."
make clean
make

echo "Build completed successfully!"
echo "Platform: $PLATFORM"
echo "Executable: ./build/$PLATFORM/open_particle_surface"

# Create a symlink for easy access (platform-agnostic)
if [ -f "./build/$PLATFORM/open_particle_surface" ]; then
    rm -f ./open_particle_surface
    ln -sf "./build/$PLATFORM/open_particle_surface" ./open_particle_surface
    echo "Symlink created: ./open_particle_surface -> ./build/$PLATFORM/open_particle_surface"
fi 