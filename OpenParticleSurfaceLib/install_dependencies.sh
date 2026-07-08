#!/usr/bin/env bash
# install_dependencies.sh - Create symlinks to SurfaceLib and MemoryLib dependencies

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

# Create symlinks for MemoryLib
echo "Creating symlinks for MemoryLib..."

# MemoryLib includes
ln -sf ../MemoryLib/include/mem_pool.h include/mem_pool.h

# MemoryLib sources
ln -sf ../MemoryLib/src/mem_pool.c src/mem_pool.c

# Create symlinks for SpatialQueryLib
echo "Creating symlinks for SpatialQueryLib..."

# SpatialQueryLib includes
ln -sf ../SpatialQueryLib/include/spatial_hash.h include/spatial_hash.h

# SpatialQueryLib sources
ln -sf ../SpatialQueryLib/src/spatial_hash.c src/spatial_hash.c

echo "Symlinks created successfully."
echo "Added dependencies:"
echo "  - SurfaceLib: surface.h, particle.h, surface.c, mc_tables.h"
echo "  - MemoryLib: mem_pool.h, mem_pool.c"
echo "  - SpatialQueryLib: spatial_hash.h, spatial_hash.c"