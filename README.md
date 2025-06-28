# Particle Dynamics Example

A C++ particle simulation project using **Open Dynamics Engine (ODE)** for physics simulation and **raylib** for graphics rendering.

## Project Structure

```
ParticleDynamicsExample/
├── main.cpp                 # Main application entry point
├── include/
│   └── particle_system.h    # Particle system interface
├── src/
│   ├── particle_system.cpp  # Particle system implementation
│   └── object_allocator.c   # Memory allocator (from ObjectAllocatorLib)
├── Makefile                 # Cross-platform build configuration
└── README.md               # This file
```

## Features

- **Real-time particle physics** using ODE physics engine
- **3D rendering** with raylib graphics library
- **Interactive controls**: 
  - Add particles with SPACE key
  - Create particle explosions with E key
  - Reset simulation with R key
  - First-person camera controls with WASD and mouse
- **Collision detection** between particles and ground plane
- **Performance monitoring** with physics timing information

## Dependencies

### Required Libraries

1. **raylib** - Graphics and input handling
   - Automatically built from `../Libraries/raylib/`
   - Cross-platform OpenGL wrapper

2. **Open Dynamics Engine (ODE)** - Physics simulation
   - Downloaded from GitHub (thomasmarsh/ODE)
   - Built with CMake for static linking

3. **ObjectAllocatorLib** - Memory management
   - Copied from `../ObjectAllocatorLib/`
   - Efficient object allocation system

### Build Tools Required

- **CMake** (for building ODE)
- **GCC/MinGW** or **Visual Studio** compiler
- **Make** build system

## Building the Project

### Prerequisites

1. Install build tools:
   ```bash
   # On Windows (using chocolatey)
   choco install cmake mingw make
   
   # On Linux/Ubuntu
   sudo apt-get install cmake build-essential
   
   # On macOS
   brew install cmake
   ```

2. Download dependencies:
   ```bash
   make download-ode
   ```

### Compilation

**This project always builds for Windows by default!**

```bash
# Build Windows executable (default)
make

# Build for Linux (when needed)
make TARGET=linux

# Build for macOS (when needed)
make TARGET=macos

# Show build options
make help

# Clean build files
make clean
```

The build system prioritizes Windows development and automatically:
- Uses MinGW cross-compilation from WSL/Linux
- Creates native Windows .exe files
- Statically links all dependencies for portability

## Controls

- **Mouse + WASD**: First-person camera movement
- **SPACE**: Add particle at camera position
- **E**: Create particle explosion at origin
- **R**: Reset simulation
- **ESC**: Toggle mouse capture

## Physics Configuration

The particle system includes:

- **Gravity**: -9.81 m/s² (realistic Earth gravity)
- **Bounce Factor**: 0.8 (particles bounce with 80% energy retention)
- **Friction**: 0.1 (ground friction coefficient)
- **Collision Detection**: Sphere-sphere and sphere-plane collisions
- **Contact Resolution**: Stable contact joints with soft CFM

## Code Structure

### Main Application (`main.cpp`)
- Window management and main loop
- Input handling and camera controls
- Rendering coordination

### Particle System (`particle_system.h/cpp`)
- ODE physics world management
- Particle creation and simulation
- Collision detection and response
- Rendering of particles as colored spheres

### Memory Management
- Uses ObjectAllocatorLib for efficient memory allocation
- Smart pointers for automatic cleanup
- RAII pattern for physics resources

## Platform Support

**Primary Target: Windows** (always builds .exe by default)
- **Windows**: Native .exe with static linking (MinGW cross-compilation)
- **Linux**: Available when explicitly requested (`make TARGET=linux`)
- **macOS**: Available when explicitly requested (`make TARGET=macos`)
- **WSL**: Automatically cross-compiles to Windows .exe

## Performance

The system tracks physics simulation time and displays:
- Frame rate (FPS)
- Particle count
- Physics step time in milliseconds

Optimized for real-time simulation with:
- Fixed time step limiting (max 1/60 second)
- Efficient collision broad phase
- Minimal memory allocations during simulation

## Example Output

```
=== Particle Dynamics Demo with ODE Physics ===
=== Setting Up Particle Physics Scene ===
Initializing ODE physics world...
ODE physics world initialized successfully!
  World ID: 0x...
  Space ID: 0x...
  Gravity: -9.81
Ground plane created.
Scene setup complete!
Frame 60 - Particles: 20
...
```

## Extending the System

The modular design allows easy extension:

1. **Add new particle types** by extending the Particle struct
2. **Implement new forces** by modifying the physics step
3. **Add constraints** using ODE joint system
4. **Enhanced rendering** with particle trails or effects
5. **Save/load** simulation states

## Troubleshooting

### Build Issues
- Ensure CMake and compiler are in PATH
- Check ODE download completed successfully
- Verify raylib builds for your platform

### Runtime Issues
- Check ODE initialization messages
- Monitor particle count (too many particles slow simulation)
- Verify OpenGL context creation

## License

This project demonstrates integration of:
- ODE (LGPL/BSD dual license)
- raylib (zlib license)
- Custom code (project-specific license) 