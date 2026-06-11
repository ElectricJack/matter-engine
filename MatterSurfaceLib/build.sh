#!/bin/bash
# Build script for GPURayTraceExample with cross-platform support

set -e  # Exit on any error

echo "Building GPURayTraceExample..."

# Show usage information if help is requested
if [[ "$1" == "--help" || "$1" == "-h" ]]; then
    echo ""
    echo "Usage: $0 [options]"
    echo ""
    echo "Build options:"
    echo "  $0                    # Build for current platform (auto-detects WSL→Windows)"
    echo "  $0 --no-mingw        # Build on Windows using alternative toolchain"
    echo "  $0 --wsl-linux       # Build Linux executable from WSL (instead of Windows)"
    echo "  $0 --cross-compile   # Cross-compile from Linux to Windows"
    echo "  $0 --help           # Show this help message"
    echo ""
    echo "Manual make commands:"
    echo "  make                 # Default build (auto-detects platform)"
    echo "  make WSL_LINUX=1     # Build Linux executable from WSL"
    echo "  make NO_MINGW=1      # Use alternative toolchain on Windows"
    echo "  TARGET=windows-native make # Cross-compile to Windows"
    echo ""
    exit 0
fi

# Detect current platform
UNAME_S=$(uname -s)
UNAME_R=$(uname -r)

# Check for WSL
IS_WSL=0
if echo "$UNAME_R" | grep -qi microsoft; then
    IS_WSL=1
elif echo "$UNAME_R" | grep -qi wsl; then
    IS_WSL=1
elif [ -f /proc/version ] && grep -qi microsoft /proc/version; then
    IS_WSL=1
fi

case "${UNAME_S}" in
    Linux*)     
        if [ $IS_WSL -eq 1 ]; then
            PLATFORM="wsl"
        else
            PLATFORM="linux"
        fi;;
    Darwin*)    PLATFORM=macos;;
    CYGWIN*)    PLATFORM=windows;;
    MINGW*)     PLATFORM=windows;;
    *)          PLATFORM="unknown:${UNAME_S}"
esac

echo "Detected platform: $PLATFORM"
if [ $IS_WSL -eq 1 ]; then
    echo "WSL detected - will cross-compile to Windows by default"
fi

# Handle build options
MAKE_ARGS=""
BUILD_TYPE="default"

# Check for required tools on Windows
if [[ "$PLATFORM" == "windows" ]]; then
    echo "Checking for required build tools on Windows..."
    
    # Check for make
    if ! command -v make &> /dev/null; then
        echo ""
        echo "❌ ERROR: 'make' not found in PATH"
        echo ""
        echo "To build on Windows, you need to install MinGW-w64:"
        echo ""
        echo "Option 1: Install MSYS2 (Recommended)"
        echo "  1. Download and install MSYS2 from: https://www.msys2.org/"
        echo "  2. Open MSYS2 terminal and run:"
        echo "     pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-make"
        echo "  3. Add to Windows PATH: C:\\msys64\\mingw64\\bin"
        echo "  4. Restart your terminal and try again"
        echo ""
        echo "Option 2: Install MinGW-w64 directly"
        echo "  1. Download from: https://www.mingw-w64.org/downloads/"
        echo "  2. Add to Windows PATH"
        echo ""
        echo "Option 3: Use WSL (Windows Subsystem for Linux)"
        echo "  1. Install WSL and Ubuntu"
        echo "  2. Run: sudo apt install build-essential mingw-w64"
        echo "  3. Build from WSL terminal"
        echo ""
        exit 1
    fi
    
    # Check for MinGW
    if ! command -v x86_64-w64-mingw32-gcc &> /dev/null; then
        echo ""
        echo "⚠️  WARNING: MinGW cross-compiler not found"
        echo "   Will attempt to use system default compiler"
        echo "   For native .exe builds, install MinGW-w64 (see instructions above)"
        echo ""
        MAKE_ARGS="NO_MINGW=1"
        BUILD_TYPE="system default (not MinGW)"
    else
        echo "✓ MinGW detected - will build native .exe"
        BUILD_TYPE="MinGW (native .exe)"
         fi
fi

# Handle command line arguments
if [[ "$1" == "--no-mingw" ]]; then
    if [[ "$PLATFORM" == "windows" ]]; then
        MAKE_ARGS="NO_MINGW=1"
        BUILD_TYPE="alternative toolchain"
        echo "Using alternative Windows toolchain (not MinGW)"
    else
        echo "ERROR: --no-mingw flag can only be used on Windows"
        echo "For cross-compilation from Linux, use: $0 --cross-compile"
        exit 1
    fi
elif [[ "$1" == "--wsl-linux" ]]; then
    if [[ "$PLATFORM" == "wsl" ]]; then
        MAKE_ARGS="WSL_LINUX=1"
        BUILD_TYPE="WSL Linux"
        echo "Building Linux executable from WSL"
    else
        echo "ERROR: --wsl-linux flag can only be used in WSL"
        echo "Current platform: $PLATFORM"
        exit 1
    fi
elif [[ "$1" == "--cross-compile" ]]; then
    MAKE_ARGS="TARGET=windows-native"
    BUILD_TYPE="cross-compile"
    echo "Cross-compiling for Windows from $PLATFORM"
    
    # Check if MinGW is available for cross-compilation
    if ! command -v x86_64-w64-mingw32-gcc &> /dev/null; then
        echo "ERROR: MinGW cross-compiler not found!"
        echo "Install with: sudo apt update && sudo apt install -y mingw-w64 mingw-w64-tools"
        exit 1
    fi
fi

# WSL default cross-compilation check (only if no explicit args given)
if [[ "$PLATFORM" == "wsl" && -z "$1" ]]; then
    echo "WSL detected: Checking for MinGW cross-compiler..."
    if ! command -v x86_64-w64-mingw32-gcc &> /dev/null; then
        echo ""
        echo "⚠️  MinGW cross-compiler not found in WSL"
        echo "   Installing MinGW for Windows cross-compilation..."
        echo ""
        echo "   Run: sudo apt update && sudo apt install -y mingw-w64 mingw-w64-tools"
        echo "   Or use: $0 --wsl-linux  (to build Linux executable instead)"
        echo ""
        exit 1
    else
        echo "✓ MinGW detected - will cross-compile to Windows .exe"
        BUILD_TYPE="WSL→Windows cross-compile"
    fi
fi

# Build the project
echo "Building project for $PLATFORM ($BUILD_TYPE build)..."
make clean
make $MAKE_ARGS

echo "Build completed successfully!"
echo "Platform: $PLATFORM"
echo "Build type: $BUILD_TYPE"

# Show the appropriate executable path
if [[ "$BUILD_TYPE" == "cross-compile" || "$BUILD_TYPE" == "WSL→Windows cross-compile" || "$PLATFORM" == "windows" ]]; then
    echo "Executable: ./gpu_raytrace.exe"
    if [ -f "./gpu_raytrace.exe" ]; then
        echo "✓ Ready to run: ./gpu_raytrace.exe"
        if [[ "$PLATFORM" == "wsl" ]]; then
            echo "  Note: This Windows .exe can be run directly from WSL or copied to Windows"
        fi
    fi
else
    echo "Executable: ./gpu_raytrace"
    if [ -f "./gpu_raytrace" ]; then
        echo "✓ Ready to run: ./gpu_raytrace"
    fi
fi

# Note: Executable is automatically copied to root directory by Makefile 