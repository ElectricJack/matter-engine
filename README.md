# GPU Ray Trace Example

A modern C++ pixel-shader based raytracing implementation using Raylib, featuring modular BLAS/TLAS acceleration structures and comprehensive performance profiling.

## Features

- **🏗️ Modular BLAS/TLAS System**: Industry-standard Bottom-Level and Top-Level Acceleration Structures
- **🚀 Modern C++17**: RAII memory management, smart pointers, and STL containers
- **📊 Performance Profiling**: Built-in RAII-based timing with detailed frame breakdowns
- **🔄 Automatic Deduplication**: Hash-based BLAS sharing for memory efficiency  
- **🎨 Matrix Stack**: OpenGL-style hierarchical transformations with automatic cleanup
- **🏭 Factory Patterns**: Clean geometry creation (`BLASFactory::create_cube()`, etc.)
- **🎯 Real-time Raytracing**: Fragment shader implementation with GPU-accelerated BVH traversal
- **🖱️ Interactive Camera**: Free-look camera controls with animated scene objects
- **🔍 Debug Visualization**: Multiple debug modes for shader development and optimization

## Project Structure

```
GPURayTraceExample/
├── main.cpp               # Modern C++ main application with performance profiling
├── include/
│   ├── profiler.hpp      # RAII-based performance profiler
│   ├── blas_manager.hpp  # Modern C++ BLAS management system
│   ├── tlas_manager.hpp  # Modern C++ TLAS with matrix stack
│   ├── bvh.h            # Core C BVH implementation (symlinked)
│   └── object_allocator.h # Memory allocator (symlinked)
├── src/
│   ├── blas_manager.cpp  # BLAS manager with hash-based deduplication
│   ├── tlas_manager.cpp  # TLAS manager with hierarchical transforms
│   ├── bvh.c            # Core BVH implementation (symlinked)
│   └── object_allocator.c # Memory allocator (symlinked)
└── shaders/
    ├── fullscreen.vs     # Vertex shader for fullscreen quad
    └── raytrace_tlas_blas.fs # Advanced fragment shader with TLAS/BLAS support
```

## Dependencies

- **SpatialQueryLib**: BVH acceleration structure implementation
- **Raylib**: Graphics framework for window management and rendering

## Building

Requires C++17 compiler (GCC 7+ or Clang 5+):

```bash
make clean
make              # Builds C++ version (gpu_raytrace)
```

## Usage

```bash
./gpu_raytrace    # Run with performance profiling and BVH raytracing
```

### Controls

- **WASD**: Move camera
- **Mouse**: Look around  
- **SPACE**: Toggle between raytracing and rasterization modes
- **P**: Print current performance statistics
- **R**: Reset performance statistics

#### Debug Controls (Raytracing Mode Only)
- **1**: Ray direction visualization (RGB = XYZ ray direction)
- **2**: UV coordinates visualization (Green = X axis, Red = Y axis)
- **3**: TLAS debug visualization
- **4**: BLAS vs TLAS comparison
- **5**: Instance ID visualization  
- **6**: Full raytracing with reflections and shadows

### Performance Monitoring

The application automatically displays performance metrics:
```
Frame: 16.67 ms (60.0 FPS)
Scene Setup: 2.1 ms (12.6%)
GPU Upload: 1.2 ms (7.2%)  
Rendering: 13.2 ms (79.2%)
```

## Technical Details

### BVH Construction
- Surface Area Heuristic (SAH) for optimal splitting
- Recursive tree building on CPU
- Flattened to node and index buffers for GPU upload
- **GPU BVH Traversal**: Iterative stack-based traversal using texture data
- **Data Packing**: BVH nodes and triangles encoded in RGBA32F textures for GPU access

### Ray Tracing Pipeline
1. **Primary Rays**: Generated per pixel from camera
2. **BVH Traversal**: Iterative stack-based traversal for performance
3. **Ray-Triangle Intersection**: Möller-Trumbore algorithm
4. **Shading**: Phong-style lighting with reflections
5. **Path Tracing**: Multi-bounce reflections for materials

### Scene Content
- 3 boxes (Red, Blue, Green ground plane) - 36 triangles
- 2 spheres (Yellow, Magenta) - 16 triangles
- Total: 52 triangles organized in 31 BVH nodes
- **GPU Data**: 31 BVH nodes (31x3 RGBA32F texture), 52 triangles (52x4 RGBA32F texture)

### Performance Features
- Branchless AABB intersection tests
- Early ray termination
- Coherent memory access patterns
- Optimized shader uniform management

## Material System

- **Material 0**: Red reflective
- **Material 1**: Blue reflective  
- **Material 2**: Green diffuse (ground)
- **Material 3**: Yellow diffuse
- **Material 4**: Magenta diffuse