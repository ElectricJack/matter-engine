# Phase 5a (tech-debt.md §6): this Makefile used to build a standalone
# raylib/GLFW/imgui GL viewer app (main.cpp + bvh_visualizer.{cpp,hpp}).
# Both are deleted outright -- not migrated -- along with the GL renderer
# path (MatterEngine3/src/render/renderer.*, raster_composer.*, gpu_culler.*).
# What remains and still matters: shader_preprocessor, which regenerates
# the COMMITTED shaders/raytrace_tlas_blas_processed.fs consumed directly by
# MatterEngine3's embedded-shaders step (MatterEngine3/Makefile's
# MSL_SHADER_DIR points straight at this directory; the MatterEngine3/shaders
# symlink that step used to go through is gone -- see CLAUDE.md "Code Sharing
# Between Projects") (see the `shaders:`/`regen-shaders` targets below, and
# tech-debt.md §9). Everything else here (CFLAGS/CXXFLAGS, the platform
# detection block) is kept only because $(PREPROCESSOR) still needs
# BUILD_DIR/CXXFLAGS; LDFLAGS/LDLIBS/raylib-build plumbing are now unused
# by anything in this file but left in place rather than risk breaking the
# platform-detection ifs for a variable that costs nothing idle.
CC = gcc
CXX = g++
RAYLIB_PATH = ../../third_party/raylib
IMGUI_PATH = ../../third_party/imgui
SQL_DIR = ../SpatialQueryLib
MEMLIB_DIR = ../MemoryLib
MATHLIB_DIR = ../MathLib
CFLAGS = -Wall -Wextra -O2 -I$(SQL_DIR)/include -I$(MEMLIB_DIR)/include -I$(MATHLIB_DIR)/include -I$(RAYLIB_PATH)/src -I$(RAYLIB_PATH)/src/external/glfw/include -I$(IMGUI_PATH) -I$(IMGUI_PATH)/backends -I./include
CXXFLAGS = -Wall -Wextra -O2 -std=c++14 -Wno-missing-field-initializers -I$(SQL_DIR)/include -I$(MEMLIB_DIR)/include -I$(MATHLIB_DIR)/include -I$(RAYLIB_PATH)/src -I$(RAYLIB_PATH)/src/external/glfw/include -I$(IMGUI_PATH) -I$(IMGUI_PATH)/backends -I./include

# Build flags:
# TARGET=windows-native : Cross-compile for Windows from Linux using MinGW
# NO_MINGW=1           : Use alternative Windows toolchain instead of MinGW (Windows only)
# WSL_LINUX=1          : Build Linux executable from WSL (default is Windows cross-compile)
# 
# Examples:
#   make                     # Build for current platform (auto-detects WSL→Windows, Windows→MinGW)
#   make WSL_LINUX=1         # Build Linux executable from WSL
#   make NO_MINGW=1          # Build on Windows using alternative toolchain (MSVC, etc.)
#   TARGET=windows-native make # Cross-compile from Linux to Windows

# Cross-compilation and toolchain support
ifeq ($(TARGET),windows-native)
    # Cross-compile for Windows from Linux/WSL
    CC = x86_64-w64-mingw32-gcc
    CXX = x86_64-w64-mingw32-g++-posix
    PLATFORM = windows-native
    PLATFORM_DEFINE = PLATFORM_DESKTOP
    LDFLAGS = -L$(RAYLIB_PATH)/build/$(PLATFORM) -static -lopengl32 -lgdi32 -lwinmm
    LDLIBS = $(RAYLIB_PATH)/build/$(PLATFORM)/libraylib.a
    BIN_SUFFIX = .exe
else
    # Auto-detect OS and set platform-specific variables
    UNAME_S := $(shell uname -s)
    
    # WSL detection
    IS_WSL := 0
    ifneq ($(shell uname -r | grep -i microsoft),)
        IS_WSL := 1
    endif
    ifneq ($(shell uname -r | grep -i wsl),)
        IS_WSL := 1
    endif
    ifneq ($(wildcard /proc/version),)
        ifneq ($(shell grep -i microsoft /proc/version 2>/dev/null),)
            IS_WSL := 1
        endif
    endif
    
    # Windows detection (multiple methods for robustness)
    IS_WINDOWS := 0
    ifneq ($(OS),)
        ifeq ($(OS),Windows_NT)
            IS_WINDOWS := 1
        endif
    endif
    # Also check uname output for Windows patterns
    ifneq ($(findstring CYGWIN,$(UNAME_S)),)
        IS_WINDOWS := 1
    endif
    ifneq ($(findstring MINGW,$(UNAME_S)),)
        IS_WINDOWS := 1
    endif
    ifneq ($(findstring MSYS,$(UNAME_S)),)
        IS_WINDOWS := 1
    endif
    
    ifeq ($(UNAME_S),Darwin)
        PLATFORM = macos
        PLATFORM_DEFINE = PLATFORM_DESKTOP
        LDFLAGS = -L$(RAYLIB_PATH)/build/$(PLATFORM) -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo -lm
        LDLIBS = $(RAYLIB_PATH)/build/$(PLATFORM)/libraylib.a
    else ifeq ($(IS_WSL),1)
        # WSL detected - default to cross-compile to Windows unless WSL_LINUX=1
        ifeq ($(WSL_LINUX),1)
            # User wants Linux executable from WSL
            PLATFORM = linux
            PLATFORM_DEFINE = PLATFORM_DESKTOP
            LDFLAGS = -L$(RAYLIB_PATH)/build/$(PLATFORM) -lGL -lm -lpthread -ldl -lrt -lX11
            LDLIBS = $(RAYLIB_PATH)/build/$(PLATFORM)/libraylib.a
        else
            # Default: Cross-compile to Windows from WSL
            CC = x86_64-w64-mingw32-gcc
            CXX = x86_64-w64-mingw32-g++-posix
            PLATFORM = windows-native
            PLATFORM_DEFINE = PLATFORM_DESKTOP
            LDFLAGS = -L$(RAYLIB_PATH)/build/$(PLATFORM) -static -lopengl32 -lgdi32 -lwinmm
            LDLIBS = $(RAYLIB_PATH)/build/$(PLATFORM)/libraylib.a
            BIN_SUFFIX = .exe
        endif
    else ifeq ($(UNAME_S),Linux)
        PLATFORM = linux
        PLATFORM_DEFINE = PLATFORM_DESKTOP
        LDFLAGS = -L$(RAYLIB_PATH)/build/$(PLATFORM) -lGL -lm -lpthread -ldl -lrt -lX11
        LDLIBS = $(RAYLIB_PATH)/build/$(PLATFORM)/libraylib.a
    else ifeq ($(IS_WINDOWS),1)
        PLATFORM_DEFINE = PLATFORM_DESKTOP
        # On Windows, use MinGW by default for native .exe builds
        # Set NO_MINGW=1 to use default Windows toolchain instead
        ifeq ($(NO_MINGW),1)
            # Alternative Windows toolchain (MSVC, regular gcc, etc.)
            PLATFORM = windows
            LDFLAGS = -L$(RAYLIB_PATH)/build/$(PLATFORM) -lopengl32 -lgdi32 -lwinmm
        else
            # Default: Use MinGW toolchain for native Windows builds.
            # MSYS2 UCRT64 (the toolchain actually installed here) ships plain
            # x86_64-w64-mingw32-g++, not the "-posix" threading variant that
            # Debian/Ubuntu's mingw-w64 packages provide (and that the
            # Linux/WSL cross-compile branches above still use, since that is
            # what is actually installed there) -- verified with `which`.
            CC = x86_64-w64-mingw32-gcc
            CXX = x86_64-w64-mingw32-g++
            PLATFORM = windows-mingw
            LDFLAGS = -L$(RAYLIB_PATH)/build/$(PLATFORM) -static -lopengl32 -lgdi32 -lwinmm
        endif
        LDLIBS = $(RAYLIB_PATH)/build/$(PLATFORM)/libraylib.a
        BIN_SUFFIX = .exe
    else
        # Fallback for unknown platforms - assume Unix-like
        PLATFORM = unknown
        PLATFORM_DEFINE = PLATFORM_DESKTOP
        LDFLAGS = -L$(RAYLIB_PATH)/build/$(PLATFORM) -lGL -lm -lpthread -ldl -lrt -lX11
        LDLIBS = $(RAYLIB_PATH)/build/$(PLATFORM)/libraylib.a
    endif
endif

# Platform-specific directories
BUILD_DIR = build/$(PLATFORM)
OBJ_DIR = $(BUILD_DIR)/obj

# Create platform-specific directories
$(shell mkdir -p $(OBJ_DIR))
$(shell mkdir -p $(BUILD_DIR))

PREPROCESSOR = $(BUILD_DIR)/shader_preprocessor

all: shaders

shaders: shaders/raytrace_tlas_blas_processed.fs

$(PREPROCESSOR): src/shader_preprocessor.cpp
ifeq ($(TARGET),windows-native)
	# For cross-compilation, build preprocessor for host (Linux) platform
	g++ -Wall -Wextra -O2 -std=c++14 -o $@ $<
else ifeq ($(PLATFORM),windows-native)
	# For WSL cross-compilation, build preprocessor for host (Linux) platform
	g++ -Wall -Wextra -O2 -std=c++14 -o $@ $<
else
	$(CXX) $(CXXFLAGS) -o $@ $<
endif

shaders/raytrace_tlas_blas_processed.fs: shaders/raytrace_tlas_blas.fs shaders/bvh_tlas_common.glsl shaders/materials.glsl shaders/lighting.glsl $(PREPROCESSOR)
	@echo "Processing shader with includes (C++)..."
	$(PREPROCESSOR) shaders/raytrace_tlas_blas.fs shaders/raytrace_tlas_blas_processed.fs

# Platform-specific clean: everything for this platform lives under
# build/$(PLATFORM)/ (objects, preprocessor, binary), so this is just that.
#
# NOTE: shaders/raytrace_tlas_blas_processed.fs is deliberately NOT removed.
# It is a generated file (shader_preprocessor expands the #includes in
# raytrace_tlas_blas.fs) but it is also COMMITTED, and MatterEngine3's
# embedded-shaders step consumes it directly from this directory (see the
# module header comment above).
# Deleting it breaks every downstream build until MatterSurfaceLib is rebuilt
# to regenerate it -- which is impossible on a toolchain missing
# x86_64-w64-mingw32-g++-posix, i.e. the default MSYS2 UCRT64 setup. Use
# `make regen-shaders` to rebuild it deliberately.
clean:
	rm -rf $(BUILD_DIR)

# Clean all platforms
clean-all:
	rm -rf build/
	rm -rf $(RAYLIB_PATH)/build/

# Explicitly regenerate the committed preprocessed shader. Requires a working
# toolchain for $(PREPROCESSOR); review the diff before committing the result.
.PHONY: regen-shaders
regen-shaders:
	rm -f shaders/raytrace_tlas_blas_processed.fs
	$(MAKE) shaders/raytrace_tlas_blas_processed.fs

# Show current platform
platform:
	@echo "=== Platform Detection Debug ==="
	@echo "UNAME_S: $(UNAME_S)"
	@echo "OS: $(OS)"
	@echo "IS_WSL: $(IS_WSL)"
	@echo "IS_WINDOWS: $(IS_WINDOWS)"
	@echo "WSL_LINUX: $(WSL_LINUX)"
	@echo "Current platform: $(PLATFORM)"
	@echo "Platform define: $(PLATFORM_DEFINE)"
	@echo "Build directory: $(BUILD_DIR)"
	@echo "CC: $(CC)"
	@echo "CXX: $(CXX)"

.PHONY: all clean clean-all shaders platform