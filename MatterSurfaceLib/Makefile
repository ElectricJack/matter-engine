CC = gcc
CXX = g++
RAYLIB_PATH = ../Libraries/raylib
IMGUI_PATH = ../Libraries/imgui
CFLAGS = -Wall -Wextra -O2 -I$(RAYLIB_PATH)/src -I$(RAYLIB_PATH)/src/external/glfw/include -I$(IMGUI_PATH) -I$(IMGUI_PATH)/backends -I./include
CXXFLAGS = -Wall -Wextra -O2 -std=c++14 -Wno-missing-field-initializers -I$(RAYLIB_PATH)/src -I$(RAYLIB_PATH)/src/external/glfw/include -I$(IMGUI_PATH) -I$(IMGUI_PATH)/backends -I./include

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
            # Default: Use MinGW toolchain for native Windows builds
            CC = x86_64-w64-mingw32-gcc
            CXX = x86_64-w64-mingw32-g++-posix
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
$(shell mkdir -p $(RAYLIB_PATH)/build/$(PLATFORM))

# C++ main application
SRC = main.cpp src/bvh.cpp src/object_allocator.c src/blas_manager.cpp src/tlas_manager.cpp src/part_asset.cpp src/bvh_visualizer.cpp src/bvh_analyzer.cpp src/open_particle_surface.c src/surface.c src/material_registry.c src/spatial_hash.c src/cluster.cpp src/cell.cpp src/mesh_worker_pool.cpp src/mesh_simplifier.cpp src/mesh_build_utils.cpp src/meshing_algorithm.cpp src/marching_cubes_algorithm.cpp src/oriented_cube_algorithm.cpp src/lattice.cpp src/occupancy.cpp src/particle_culling.cpp src/vertex_ao.cpp $(IMGUI_PATH)/imgui.cpp $(IMGUI_PATH)/imgui_demo.cpp $(IMGUI_PATH)/imgui_draw.cpp $(IMGUI_PATH)/imgui_tables.cpp $(IMGUI_PATH)/imgui_widgets.cpp $(IMGUI_PATH)/backends/imgui_impl_opengl3.cpp $(IMGUI_PATH)/backends/imgui_impl_glfw.cpp
OBJ = $(OBJ_DIR)/main.o $(OBJ_DIR)/bvh.o $(OBJ_DIR)/object_allocator.o $(OBJ_DIR)/blas_manager.o $(OBJ_DIR)/tlas_manager.o $(OBJ_DIR)/part_asset.o $(OBJ_DIR)/bvh_visualizer.o $(OBJ_DIR)/bvh_analyzer.o $(OBJ_DIR)/open_particle_surface.o $(OBJ_DIR)/surface.o $(OBJ_DIR)/material_registry.o $(OBJ_DIR)/spatial_hash.o $(OBJ_DIR)/cluster.o $(OBJ_DIR)/cell.o $(OBJ_DIR)/mesh_worker_pool.o $(OBJ_DIR)/mesh_simplifier.o $(OBJ_DIR)/mesh_build_utils.o $(OBJ_DIR)/meshing_algorithm.o $(OBJ_DIR)/marching_cubes_algorithm.o $(OBJ_DIR)/oriented_cube_algorithm.o $(OBJ_DIR)/lattice.o $(OBJ_DIR)/occupancy.o $(OBJ_DIR)/particle_culling.o $(OBJ_DIR)/vertex_ao.o $(OBJ_DIR)/imgui.o $(OBJ_DIR)/imgui_demo.o $(OBJ_DIR)/imgui_draw.o $(OBJ_DIR)/imgui_tables.o $(OBJ_DIR)/imgui_widgets.o $(OBJ_DIR)/imgui_impl_opengl3.o $(OBJ_DIR)/imgui_impl_glfw.o
BIN = $(BUILD_DIR)/matter_surface_lib$(BIN_SUFFIX)
PREPROCESSOR = $(BUILD_DIR)/shader_preprocessor

all: dependencies shaders $(BIN)

dependencies:
	@echo "Setting up dependencies for $(PLATFORM)..."
	@mkdir -p src
	@cp ../ObjectAllocatorLib/src/object_allocator.c src/

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

shaders/raytrace_tlas_blas_processed.fs: shaders/raytrace_tlas_blas.fs shaders/bvh_tlas_common.glsl $(PREPROCESSOR)
	@echo "Processing shader with includes (C++)..."
	$(PREPROCESSOR) shaders/raytrace_tlas_blas.fs shaders/raytrace_tlas_blas_processed.fs

# Platform-specific raylib build with force rebuild check
RAYLIB_LIB = $(RAYLIB_PATH)/build/$(PLATFORM)/libraylib.a
RAYLIB_FORCE_FILE = $(RAYLIB_PATH)/build/$(PLATFORM)/.raylib_$(PLATFORM)

raylib: $(RAYLIB_LIB)

$(RAYLIB_LIB): $(RAYLIB_FORCE_FILE)
	@echo "Building raylib for $(PLATFORM)..."
	@mkdir -p $(RAYLIB_PATH)/build/$(PLATFORM)
ifeq ($(TARGET),windows-native)
	$(MAKE) -C $(RAYLIB_PATH)/src PLATFORM=$(PLATFORM_DEFINE) CC=$(CC) PLATFORM_OS=WINDOWS clean
	$(MAKE) -C $(RAYLIB_PATH)/src PLATFORM=$(PLATFORM_DEFINE) CC=$(CC) PLATFORM_OS=WINDOWS
else ifeq ($(PLATFORM),windows-native)
	$(MAKE) -C $(RAYLIB_PATH)/src PLATFORM=$(PLATFORM_DEFINE) CC=$(CC) PLATFORM_OS=WINDOWS clean
	$(MAKE) -C $(RAYLIB_PATH)/src PLATFORM=$(PLATFORM_DEFINE) CC=$(CC) PLATFORM_OS=WINDOWS
else ifeq ($(PLATFORM),windows-mingw)
	$(MAKE) -C $(RAYLIB_PATH)/src PLATFORM=$(PLATFORM_DEFINE) CC=$(CC) PLATFORM_OS=WINDOWS clean
	$(MAKE) -C $(RAYLIB_PATH)/src PLATFORM=$(PLATFORM_DEFINE) CC=$(CC) PLATFORM_OS=WINDOWS
else
	$(MAKE) -C $(RAYLIB_PATH)/src PLATFORM=$(PLATFORM_DEFINE) CC=$(CC) clean
	$(MAKE) -C $(RAYLIB_PATH)/src PLATFORM=$(PLATFORM_DEFINE) CC=$(CC)
endif
	@cp $(RAYLIB_PATH)/src/libraylib.a $(RAYLIB_LIB)
	@echo "Raylib built and copied to $(RAYLIB_LIB)"

$(RAYLIB_FORCE_FILE):
	@echo "Creating platform marker for $(PLATFORM)..."
	@mkdir -p $(RAYLIB_PATH)/build/$(PLATFORM)
	@rm -f $(RAYLIB_PATH)/build/*/.raylib_*
	@touch $(RAYLIB_FORCE_FILE)

$(BIN): $(OBJ) raylib
ifeq ($(TARGET),windows-native)
	# For Windows cross-compilation, link raylib first to avoid symbol conflicts
	$(CXX) -o $@ $(OBJ) $(LDLIBS) $(LDFLAGS)
else ifeq ($(PLATFORM),windows-native)
	# For WSL→Windows cross-compilation, link raylib first to avoid symbol conflicts
	$(CXX) -o $@ $(OBJ) $(LDLIBS) $(LDFLAGS)
else ifeq ($(PLATFORM),windows-mingw)
	# For Windows MinGW builds, link raylib first to avoid symbol conflicts
	$(CXX) -o $@ $(OBJ) $(LDLIBS) $(LDFLAGS)
else
	$(CXX) -o $@ $(OBJ) $(LDFLAGS) $(LDLIBS)
endif
	@echo "Built executable for $(PLATFORM): $@"
	@echo "Copying executable to root directory for easy execution..."
ifeq ($(TARGET),windows-native)
	@cp $@ ./matter_surface_lib.exe
	@echo "✓ Copied to ./matter_surface_lib.exe"
else ifeq ($(PLATFORM),windows-native)
	@cp $@ ./gpu_raytrace.exe
	@echo "✓ Copied to ./gpu_raytrace.exe (WSL→Windows cross-compile)"
else ifeq ($(PLATFORM),windows-mingw)
	@cp $@ ./matter_surface_lib.exe
	@echo "✓ Copied to ./matter_surface_lib.exe"
else ifeq ($(PLATFORM),windows)
	@cp $@ ./matter_surface_lib.exe
	@echo "✓ Copied to ./matter_surface_lib.exe"
else
	@cp $@ ./matter_surface_lib
	@echo "✓ Copied to ./matter_surface_lib"
endif

# Build rules for main target (C++)
$(OBJ_DIR)/main.o: main.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

$(OBJ_DIR)/bvh.o: src/bvh.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

$(OBJ_DIR)/object_allocator.o: src/object_allocator.c
	$(CC) -c $< $(CFLAGS) -o $@

$(OBJ_DIR)/blas_manager.o: src/blas_manager.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

$(OBJ_DIR)/tlas_manager.o: src/tlas_manager.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

$(OBJ_DIR)/part_asset.o: src/part_asset.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

$(OBJ_DIR)/bvh_visualizer.o: src/bvh_visualizer.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

$(OBJ_DIR)/bvh_analyzer.o: src/bvh_analyzer.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

$(OBJ_DIR)/open_particle_surface.o: src/open_particle_surface.c
	$(CC) -c $< $(CFLAGS) -o $@

$(OBJ_DIR)/surface.o: src/surface.c
	$(CC) -c $< $(CFLAGS) -o $@

$(OBJ_DIR)/material_registry.o: src/material_registry.c
	$(CC) -c $< $(CFLAGS) -o $@

$(OBJ_DIR)/spatial_hash.o: src/spatial_hash.c
	$(CC) -c $< $(CFLAGS) -o $@

$(OBJ_DIR)/cluster.o: src/cluster.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

$(OBJ_DIR)/cell.o: src/cell.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

$(OBJ_DIR)/mesh_worker_pool.o: src/mesh_worker_pool.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

$(OBJ_DIR)/mesh_build_utils.o: src/mesh_build_utils.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

$(OBJ_DIR)/meshing_algorithm.o: src/meshing_algorithm.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

$(OBJ_DIR)/marching_cubes_algorithm.o: src/marching_cubes_algorithm.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

$(OBJ_DIR)/oriented_cube_algorithm.o: src/oriented_cube_algorithm.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

$(OBJ_DIR)/mesh_simplifier.o: src/mesh_simplifier.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

$(OBJ_DIR)/lattice.o: src/lattice.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

$(OBJ_DIR)/occupancy.o: src/occupancy.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

$(OBJ_DIR)/particle_culling.o: src/particle_culling.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

$(OBJ_DIR)/vertex_ao.o: src/vertex_ao.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

# ImGui build rules
$(OBJ_DIR)/imgui.o: $(IMGUI_PATH)/imgui.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

$(OBJ_DIR)/imgui_demo.o: $(IMGUI_PATH)/imgui_demo.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

$(OBJ_DIR)/imgui_draw.o: $(IMGUI_PATH)/imgui_draw.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

$(OBJ_DIR)/imgui_tables.o: $(IMGUI_PATH)/imgui_tables.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

$(OBJ_DIR)/imgui_widgets.o: $(IMGUI_PATH)/imgui_widgets.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

$(OBJ_DIR)/imgui_impl_opengl3.o: $(IMGUI_PATH)/backends/imgui_impl_opengl3.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

$(OBJ_DIR)/imgui_impl_glfw.o: $(IMGUI_PATH)/backends/imgui_impl_glfw.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

# Platform-specific clean
clean:
	rm -rf $(BUILD_DIR)
	rm -f *.o $(PREPROCESSOR) shaders/raytrace_tlas_blas_processed.fs
ifeq ($(TARGET),windows-native)
	-rm -f ./matter_surface_lib.exe
else ifeq ($(PLATFORM),windows-native)
	-rm -f ./matter_surface_lib.exe
else ifeq ($(PLATFORM),windows-mingw)
	-rm -f ./matter_surface_lib.exe
else ifeq ($(PLATFORM),windows)
	-rm -f ./matter_surface_lib.exe
else
	-rm -f ./matter_surface_lib
endif

# Clean all platforms
clean-all:
	rm -rf build/
	rm -rf $(RAYLIB_PATH)/build/
	rm -f *.o shaders/raytrace_tlas_blas_processed.fs
	-rm -f ./matter_surface_lib ./matter_surface_lib.exe

# Force rebuild raylib for current platform
rebuild-raylib:
	rm -f $(RAYLIB_FORCE_FILE)
	$(MAKE) raylib

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
	@echo "Raylib library: $(RAYLIB_LIB)"
	@echo "CC: $(CC)"
	@echo "CXX: $(CXX)"

.PHONY: all clean clean-all raylib shaders dependencies rebuild-raylib platform