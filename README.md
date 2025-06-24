# GPU Ray Trace Example

A pixel-shader based raytracing implementation using Raylib, demonstrating real-time GPU raytracing with BVH acceleration structures.

## Features

- **BVH Acceleration Structure**: Implements a CPU-built BVH tree with GPU-friendly flattening for fast ray-triangle intersection
- **Test Scene**: Procedurally generated scene with boxes and spheres converted to triangular meshes
- **Material System**: Multiple material IDs for different shading properties
- **Real-time Raytracing**: Fragment shader implementation with primary ray shooting per pixel
- **Path Tracing**: Simple reflections and multi-bounce lighting
- **Interactive Camera**: Free-look camera controls with WASD movement
- **Debug Visualization**: Multiple debug modes to verify shader operation, UV coordinates, ray generation, and hit testing

## Project Structure

```
GPURayTraceExample/
├── main.c                 # Main application and rendering loop
├── include/
│   ├── scene.h           # Scene generation and management
│   ├── bvh.h            # BVH acceleration structure (symlinked)
│   └── object_allocator.h # Memory allocator (symlinked)
├── src/
│   ├── scene.c          # Scene generation implementation
│   ├── bvh.c           # BVH implementation (symlinked)
│   └── object_allocator.c # Memory allocator (symlinked)
└── shaders/
    ├── fullscreen.vs    # Vertex shader for fullscreen quad
    └── raytrace.fs     # Fragment shader with raytracing implementation
```

## Dependencies

- **SpatialQueryLib**: BVH acceleration structure implementation
- **Raylib**: Graphics framework for window management and rendering

## Building

```bash
make clean
make
```

## Usage

```bash
./gpu_raytrace
```

### Controls

- **WASD**: Move camera
- **Mouse**: Look around
- **SPACE**: Toggle between raytracing and rasterization modes

#### Debug Controls (Raytracing Mode Only)
- **1**: Ray direction visualization (RGB = XYZ ray direction)
- **2**: UV coordinates visualization (Green = X axis, Red = Y axis)
- **3**: Ray direction visualization (alternative view) 
- **4**: **Triangle intersection testing** (Red/Blue boxes, Green ground, Sky gradient)
- **5**: Distance visualization (White = close, Black = far, Black = no hit)
- **6**: Full raytracing with reflections and shadows
- **7**: **BVH traversal** (Uses GPU-accelerated BVH for ray-triangle intersection)

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