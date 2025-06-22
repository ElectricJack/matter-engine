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
ln -sf ../SurfaceLib/src/mc_tables.h src/mc_tables.h

# Create symlinks for ObjectAllocatorLib
echo "Creating symlinks for ObjectAllocatorLib..."

# ObjectAllocatorLib includes
ln -sf ../ObjectAllocatorLib/include/object_allocator.h include/object_allocator.h

# ObjectAllocatorLib sources
ln -sf ../ObjectAllocatorLib/src/object_allocator.c src/object_allocator.c

echo "Symlinks created successfully."
echo "Added dependencies:"
echo "  - SurfaceLib: surface.h, particle.h, surface.c, mc_tables.h"
echo "  - ObjectAllocatorLib: object_allocator.h, object_allocator.c"