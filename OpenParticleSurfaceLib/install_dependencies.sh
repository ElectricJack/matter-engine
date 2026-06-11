#!/usr/bin/env bash
# install_dependencies.sh - Create symlinks to SurfaceLib and ObjectAllocatorLib dependencies

set -e  # exit on any error

# Create symlinks for SurfaceLib
echo "Creating symlinks for SurfaceLib..."
mkdir -p include
mkdir -p src

# SurfaceLib includes
ln -sf ../SurfaceLib/include/surface.h include/surface.h
ln -sf ../SurfaceLib/include/particle.h include/particle.h

# SurfaceLib sources
ln -sf ../SurfaceLib/src/surface.c src/surface.c
ln -sf ../SurfaceLib/include/mc_tables.h src/mc_tables.h

# Create symlinks for ObjectAllocatorLib
echo "Creating symlinks for ObjectAllocatorLib..."

# ObjectAllocatorLib includes
ln -sf ../ObjectAllocatorLib/include/object_allocator.h include/object_allocator.h

# ObjectAllocatorLib sources
ln -sf ../ObjectAllocatorLib/src/object_allocator.c src/object_allocator.c

# Create symlinks for SpatialQueryLib
echo "Creating symlinks for SpatialQueryLib..."

# SpatialQueryLib includes
ln -sf ../SpatialQueryLib/include/spatial_hash.h include/spatial_hash.h

# SpatialQueryLib sources
ln -sf ../SpatialQueryLib/src/spatial_hash.c src/spatial_hash.c

echo "Symlinks created successfully."
echo "Added dependencies:"
echo "  - SurfaceLib: surface.h, particle.h, surface.c, mc_tables.h"
echo "  - ObjectAllocatorLib: object_allocator.h, object_allocator.c"
echo "  - SpatialQueryLib: spatial_hash.h, spatial_hash.c"