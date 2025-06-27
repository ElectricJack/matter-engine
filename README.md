# OpenParticleSurfaceLib - Cross-Platform Build Guide

This guide explains how to build and run the OpenParticleSurfaceLib project across different platforms (Windows/WSL, macOS, Linux) with automatic platform detection and isolated build directories.

## Prerequisites

### Windows (WSL)
- Windows 10/11 with WSL2 installed
- Ubuntu or similar Linux distribution in WSL
- For graphics display: VcXsrv, Xming, or Windows 11 with WSLg

### macOS
- Xcode Command Line Tools (`xcode-select --install`)
- GCC or Clang compiler

### Linux
- GCC compiler and development tools
- OpenGL development libraries (`libgl1-mesa-dev`)
- X11 development libraries (`libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev`)

## Cross-Platform Build System

This project uses a cross-platform build system that:
- **Automatically detects your platform** (linux, macos, windows)
- **Isolates builds per platform** in separate `build/[platform]/` directories
- **Manages Raylib separately** for each platform to avoid conflicts
- **Works with shared folders** between different operating systems

## Building the Project

The build system automatically detects your platform and creates isolated build directories:

### Option 1: Use the build script (recommended)
```bash
./build.sh
```

### Option 2: Manual build with Make
```bash
make clean && make
```

### Platform-Specific Commands
```bash
# Show current platform and build info
make platform

# Check build status for all platforms
./platform-status.sh

# Force rebuild Raylib for current platform
make rebuild-raylib

# Clean current platform only
make clean

# Clean all platforms
make clean-all
```

### Build Output Structure
```
build/
├── linux/
│   ├── obj/           # Linux object files
│   └── open_particle_surface
├── macos/
│   ├── obj/           # macOS object files
│   └── open_particle_surface
└── windows/
    ├── obj/           # Windows object files
    └── open_particle_surface

Libraries/raylib/build/
├── linux/libraylib.a
├── macos/libraylib.a
└── windows/libraylib.a
```

## Running the Application

You have several options to run the application from Windows:

### Option 1: Double-click the batch file
- Simply double-click `run.bat` in Windows Explorer

### Option 2: From Command Prompt
```cmd
cd "D:\Shared With Desktop\AI\MatterEngine2\OpenParticleSurfaceLib"
run.bat
```

### Option 3: From PowerShell
```powershell
cd "D:\Shared With Desktop\AI\MatterEngine2\OpenParticleSurfaceLib"
.\run.ps1
```

### Option 4: Direct WSL command
```cmd
wsl bash -c "cd '/mnt/d/Shared With Desktop/AI/MatterEngine2/OpenParticleSurfaceLib' && ./run.sh"
```

## Graphics Display Setup

Since this is a graphics application, you need X11 forwarding:

### Windows 11 (Recommended)
- WSLg is built-in, should work automatically

### Windows 10
1. Install VcXsrv or Xming
2. Configure it to allow connections
3. The run script will automatically detect and configure the display

### Troubleshooting Graphics
If you get "cannot connect to X server" errors:

1. **For VcXsrv/Xming users**: Edit `run.sh` and uncomment the lines:
   ```bash
   WINDOWS_IP=$(ip route show | grep -i default | awk '{ print $3}')
   export DISPLAY=$WINDOWS_IP:0.0
   ```

2. **For WSLg**: Make sure you're on Windows 11 with the latest WSL updates

## Shared Folder Workflow

This build system is designed to work seamlessly when your project folder is shared between different operating systems (e.g., via network drives, cloud sync, or dual-boot setups):

### Switching Between Platforms
1. **No cleanup needed!** Each platform builds to its own directory
2. Simply run `./build.sh` on the new platform
3. Raylib will be automatically rebuilt for the target platform
4. Your previous builds remain intact

### Example Workflow
```bash
# On Windows (WSL)
./build.sh                    # Creates build/linux/
./platform-status.sh         # Shows linux build

# Switch to macOS (same shared folder)
./build.sh                    # Creates build/macos/
./platform-status.sh         # Shows both linux and macos builds

# Back to Windows
./run.sh                      # Automatically uses build/linux/
```

## Files Overview

- `build.sh` - Cross-platform build script with dependency management
- `run.sh` - Cross-platform run script with X11 setup
- `platform-status.sh` - Shows build status for all platforms
- `run.bat` - Windows batch script to run from Command Prompt
- `run.ps1` - PowerShell script to run from PowerShell
- `install_dependencies.sh` - Sets up project dependencies (legacy)
- `Makefile` - Cross-platform Makefile with isolated build directories

## Quick Start

### Any Platform
1. **Build**: `./build.sh`
2. **Run**: `./run.sh`
3. **Status**: `./platform-status.sh`

### Windows Specific
1. **Build**: Double-click `run.bat` or use PowerShell `.\run.ps1`
2. **Run**: Same scripts will build and run automatically

## Troubleshooting

### "Platform not detected"
- Make sure you're using a supported shell (bash, not Windows cmd directly)
- On Windows, use WSL: `wsl bash -c "./build.sh"`

### "Raylib build failed"
- Try: `make rebuild-raylib`
- Or: `make clean-all && ./build.sh`

### Mixed platform binaries
- Use `make clean-all` to start fresh
- The new system prevents this by using separate directories

Enjoy your cross-platform particle surface simulation! 🎉 