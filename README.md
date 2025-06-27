# GPU Ray Tracing Example

A cross-platform C++ ray tracing application using Raylib with BVH (Bounding Volume Hierarchy) acceleration structures. Features modular BLAS/TLAS system with GPU-accelerated ray tracing.

## Features

- **Cross-Platform Build System**: Supports Linux, macOS, and Windows (via WSL)
- **GPU Ray Tracing**: Hardware-accelerated ray tracing using OpenGL compute shaders
- **BVH Acceleration**: Optimized Bottom-Level (BLAS) and Top-Level (TLAS) acceleration structures
- **Modular Architecture**: Separate managers for BLAS, TLAS, and visualization
- **Performance Profiling**: Built-in timing and statistics
- **Platform Isolation**: Build artifacts separated by platform to prevent conflicts

## Requirements

### Windows (WSL)
- Windows Subsystem for Linux (WSL2) with Ubuntu
- GCC/G++ compiler: `sudo apt install build-essential g++`
- Development libraries: `sudo apt install libgl1-mesa-dev libx11-dev`

### Linux
- GCC/G++ compiler
- OpenGL development libraries
- X11 development libraries

### macOS
- Xcode command line tools
- OpenGL framework (included with macOS)

## Quick Start

### Windows (Recommended)

1. **Open PowerShell or Command Prompt**:
   ```cmd
   # Run with PowerShell
   .\run.ps1
   
   # Or run with Command Prompt
   .\run.bat
   ```

2. **Or use WSL directly**:
   ```bash
   # Build the project
   ./build.sh
   
   # Run the application
   ./run.sh
   ```

### Linux/macOS

```bash
# Build the project
./build.sh

# Run the application
./run.sh
```

## Cross-Platform Build System

The build system automatically detects your platform and creates isolated build directories:

```
build/
├── linux/          # Linux builds
├── macos/          # macOS builds  
└── windows/        # Windows builds
```

### Platform Status

Check the build status for all platforms:

```bash
./platform-status.sh
```

Example output:
```
=== GPURayTraceExample Platform Status ===

Current platform: linux

Build Status:
linux     : ✓ Built (1.4M, 2025-06-27 13:42) [Raylib: ✓] [Preprocessor: ✓] ← Current
macos     : ✗ Not built [Raylib: ✗] [Preprocessor: ✗]
windows   : ✗ Not built [Raylib: ✗] [Preprocessor: ✗]

Symlink: ./gpu_raytrace -> ./build/linux/gpu_raytrace
Shaders: ✓ Processed
```

### Makefile Targets

```bash
make                 # Build for current platform
make platform        # Show platform information
make clean           # Clean current platform
make clean-all       # Clean all platforms
make rebuild-raylib  # Force rebuild raylib for current platform
make shaders         # Process shaders only
```

## Architecture

### BLAS Manager
- Manages Bottom-Level Acceleration Structures
- Handles triangle data and BVH construction
- Provides GPU texture generation for ray tracing
- Implements mesh deduplication and caching

### TLAS Manager  
- Manages Top-Level Acceleration Structures
- Handles instance transforms and materials
- Provides scene building utilities
- Matrix stack for hierarchical transforms

### BVH Visualizer
- Debug visualization of acceleration structures
- Wireframe rendering of bounding boxes
- Color-coded depth visualization
- Triangle and node inspection tools

## GPU Ray Tracing Pipeline

1. **BLAS Construction**: Build BVH for each unique mesh
2. **TLAS Construction**: Build top-level BVH for scene instances
3. **GPU Upload**: Transfer acceleration structures to GPU textures
4. **Shader Binding**: Bind textures and uniforms to ray tracing shader
5. **Ray Tracing**: GPU compute shader traverses BVH structures

## Dependencies

The project automatically manages dependencies:

- **Raylib**: Graphics library (built from source)
- **ObjectAllocator**: Memory management (copied from ObjectAllocatorLib)
- **Shader Preprocessor**: GLSL include processing

## Troubleshooting

### Windows/WSL Issues

**"cannot connect to X server" error:**
- Install VcXsrv or Xming on Windows
- Or use Windows 11 with WSLg support
- Or run with `DISPLAY=:0.0` environment variable

**Build failures:**
- Ensure WSL2 is installed and updated
- Install required packages: `sudo apt update && sudo apt install build-essential g++`

### Performance

**Low frame rates:**
- Ensure GPU drivers are up to date
- Check that hardware acceleration is enabled
- Monitor GPU usage with system tools

**Memory issues:**
- Large scenes may require more RAM
- Consider reducing triangle counts for testing
- Monitor memory usage with built-in profiling

## Development

### Adding New Shapes

1. Implement triangle generation in `BLASFactory` namespace
2. Register with BLAS manager using `register_*` functions
3. Add to scene using TLAS manager's `draw()` method

### Custom Materials

Materials are identified by 32-bit IDs passed to the `draw()` method. Implement material handling in your ray tracing shaders.

### Shader Modification

Ray tracing shaders are in `shaders/` directory:
- `raytrace_tlas_blas.fs`: Main ray tracing fragment shader
- `bvh_tlas_common.glsl`: Common BVH traversal functions

After modifying shaders, run `make shaders` to reprocess includes.

## Performance Statistics

The application provides detailed performance metrics:

```
=== BLAS Manager Statistics ===
Unique BLAS count: 3
Total triangles: 974
Total nodes: 1934
Hash buckets: 3 used, max chain length: 1

=== TLAS Manager Statistics ===
Draw records: 2/50
Matrix stack depth: 1
Built TLAS: 2 instances, 3 nodes
```

## License

This project is part of the MatterEngine2 framework. See the main project LICENSE for details.