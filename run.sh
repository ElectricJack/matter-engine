#!/bin/bash
# Run script for GPURayTraceExample

set -e  # Exit on any error

echo "Running GPURayTraceExample..."

# Detect current platform
UNAME_S=$(uname -s)
case "${UNAME_S}" in
    Linux*)     PLATFORM=linux;;
    Darwin*)    PLATFORM=macos;;
    CYGWIN*)    PLATFORM=windows;;
    MINGW*)     PLATFORM=windows;;
    *)          PLATFORM="unknown:${UNAME_S}"
esac

EXECUTABLE="./build/$PLATFORM/gpu_raytrace"

# Check if platform-specific executable exists, fallback to symlink
if [ -f "$EXECUTABLE" ]; then
    echo "Using platform-specific executable: $EXECUTABLE"
elif [ -f "./gpu_raytrace" ]; then
    EXECUTABLE="./gpu_raytrace"
    echo "Using symlinked executable: $EXECUTABLE"
else
    echo "Error: Executable not found for platform $PLATFORM"
    echo "Please build the project first using ./build.sh"
    echo "Expected: $EXECUTABLE"
    exit 1
fi

# Try to set up display for WSL
if [ -n "$WSL_DISTRO_NAME" ]; then
    echo "Detected WSL environment"
    
    # Try to use WSLg if available (Windows 11)
    if [ -z "$DISPLAY" ]; then
        export DISPLAY=:0.0
        echo "Set DISPLAY to :0.0 for WSLg"
    fi
    
    # Alternative: Try Windows host IP for VcXsrv/Xming
    # Uncomment the lines below if you're using VcXsrv or Xming on Windows
    # WINDOWS_IP=$(ip route show | grep -i default | awk '{ print $3}')
    # export DISPLAY=$WINDOWS_IP:0.0
    # echo "Set DISPLAY to $WINDOWS_IP:0.0 for VcXsrv/Xming"
fi

echo "Starting application..."
echo "If you see 'cannot connect to X server' error, you need to:"
echo "1. Install VcXsrv or Xming on Windows, OR"
echo "2. Use Windows 11 with WSLg support"
echo ""

# Run the application
$EXECUTABLE 