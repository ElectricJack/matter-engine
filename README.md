# SurfaceLib - Isosurface Geometry Library

A C library for generating and visualizing isosurfaces using techniques like marching cubes. This project demonstrates creating various types of 3D surfaces with a raylib-based visualization.

## Features

- Generate various types of isosurfaces:
  - Spheres
  - Tori (donuts)
  - Metaballs/blobs
  - Custom user-defined functions
- Simple API for creating and managing surfaces
- Visualization with raylib graphics library
- Extensible design for easy integration with other projects

## Prerequisites

- [raylib](https://www.raylib.com/) library installed in `../Libraries/raylib`
- C compiler (gcc/clang)
- Make

## Building

```bash
make
```

## Running

```bash
./surface_app
```

## Controls

- Use mouse to orbit the camera
- Escape key to exit

## Library Usage

The library consists of two main components:

1. **API Headers**: Located in the `include/` directory
2. **Implementation**: Located in the `src/` directory

### Basic Usage Example

```c
#include "surface.h"

// Create a simple sphere surface
Surface sphere = InitSurface(SURFACE_SPHERE, (Vector3){0, 0, 0}, 1.0f);
sphere.color = RED;

// Generate a mesh for the surface
SurfaceMesh mesh = GenerateSurfaceMesh(sphere, 0.0f, 
                                     (Vector3){-2, -2, -2}, 
                                     (Vector3){2, 2, 2});

// Convert to raylib mesh for rendering
Mesh raylibMesh = ConvertToRaylibMesh(mesh);

// Render the mesh (in your drawing code)
DrawMesh(raylibMesh, material, transform);

// Clean up resources when done
UnloadSurfaceMesh(&mesh);
UnloadMesh(raylibMesh);
```

### Creating Custom Surfaces

You can define your own surface types by providing a custom evaluation function:

```c
float MyCustomFunction(Vector3 position, void* userData) {
    // Your signed distance function implementation
    // Return negative values for inside, positive for outside
    return /* your calculation */;
}

// Create and configure surface
Surface custom = InitSurface(SURFACE_CUSTOM, (Vector3){0, 0, 0}, 1.0f);
SetSurfaceCustomFunction(&custom, MyCustomFunction, myUserData);
```

## Integration with Other Projects

To use this library in your own projects:

1. Create symlinks to the `include/surface.h` and implementation files from your project
2. Include the library header: `#include "surface.h"`
3. Compile with the appropriate include path: `-I/path/to/surface/include`

See the project README in the root directory for more details on the shared code approach.