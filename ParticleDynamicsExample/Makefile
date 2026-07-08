CC = gcc
CXX = g++
RAYLIB_PATH = ../Libraries/raylib
SPATIAL_QUERY_PATH = ../SpatialQueryLib
OA_LIB = ../ObjectAllocatorLib
CFLAGS = -Wall -Wextra -O2 -I$(RAYLIB_PATH)/src -I./include -I$(SPATIAL_QUERY_PATH)/include -I$(OA_LIB)/include
CXXFLAGS = -Wall -Wextra -O2 -std=c++14 -Wno-missing-field-initializers -I$(RAYLIB_PATH)/src -I./include -I$(SPATIAL_QUERY_PATH)/include -I$(OA_LIB)/include

# Build flags:
# TARGET=linux           : Build Linux executable (overrides default Windows build)
# TARGET=macos           : Build macOS executable (overrides default Windows build)
# NO_MINGW=1             : Use alternative Windows toolchain instead of MinGW (Windows only)
# 
# Examples:
#   make                   # Always builds for Windows (default)
#   make TARGET=linux      # Build Linux executable
#   make TARGET=macos      # Build macOS executable
#   make NO_MINGW=1        # Build on Windows using alternative toolchain (MSVC, etc.)

# Default to Windows build unless specifically overridden
ifeq ($(TARGET),)
    TARGET = windows-native
endif

# Cross-compilation and toolchain support
ifeq ($(TARGET),windows-native)
    # Cross-compile for Windows from Linux/WSL
    CC = x86_64-w64-mingw32-gcc
    CXX = x86_64-w64-mingw32-g++
    PLATFORM = windows-native
    PLATFORM_DEFINE = PLATFORM_DESKTOP
    LDFLAGS = -L$(RAYLIB_PATH)/build/$(PLATFORM) -static-libgcc -static-libstdc++ -lopengl32 -lgdi32 -lwinmm
    LDLIBS = $(RAYLIB_PATH)/build/$(PLATFORM)/libraylib.a
    BIN_SUFFIX = .exe
else ifeq ($(TARGET),linux)
    # Build Linux executable
    PLATFORM = linux
    PLATFORM_DEFINE = PLATFORM_DESKTOP
    LDFLAGS = -L$(RAYLIB_PATH)/build/$(PLATFORM) -lGL -lm -lpthread -ldl -lrt -lX11
    LDLIBS = $(RAYLIB_PATH)/build/$(PLATFORM)/libraylib.a
else ifeq ($(TARGET),macos)
    # Build macOS executable
    PLATFORM = macos
    PLATFORM_DEFINE = PLATFORM_DESKTOP
    LDFLAGS = -L$(RAYLIB_PATH)/build/$(PLATFORM) -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo -lm
    LDLIBS = $(RAYLIB_PATH)/build/$(PLATFORM)/libraylib.a
else
    # Fallback - any other TARGET value defaults to Windows
    CC = x86_64-w64-mingw32-gcc
    CXX = x86_64-w64-mingw32-g++
    PLATFORM = windows-native
    PLATFORM_DEFINE = PLATFORM_DESKTOP
    LDFLAGS = -L$(RAYLIB_PATH)/build/$(PLATFORM) -static-libgcc -static-libstdc++ -lopengl32 -lgdi32 -lwinmm
    LDLIBS = $(RAYLIB_PATH)/build/$(PLATFORM)/libraylib.a
    BIN_SUFFIX = .exe
endif

# Platform-specific directories
BUILD_DIR = build/$(PLATFORM)
OBJ_DIR = $(BUILD_DIR)/obj

# Create platform-specific directories
$(shell mkdir -p $(OBJ_DIR))
$(shell mkdir -p $(BUILD_DIR))
$(shell mkdir -p $(RAYLIB_PATH)/build/$(PLATFORM))

# C++ main application with spatial query library and demos
SRC = main.cpp src/particle_system.cpp src/material_manager.cpp src/gamified_physics.cpp src/cluster.cpp src/cluster_manager.cpp src/solar_system_demo.cpp src/material_sandbox_demo.cpp
OBJ = $(OBJ_DIR)/main.o $(OBJ_DIR)/particle_system.o $(OBJ_DIR)/material_manager.o $(OBJ_DIR)/gamified_physics.o $(OBJ_DIR)/cluster.o $(OBJ_DIR)/cluster_manager.o $(OBJ_DIR)/solar_system_demo.o $(OBJ_DIR)/material_sandbox_demo.o $(OBJ_DIR)/object_allocator.o $(OBJ_DIR)/spatial_hash.o
BIN = $(BUILD_DIR)/particle_dynamics$(BIN_SUFFIX)

all: $(BIN)
	@echo "=== Build Complete ==="
	@echo "Platform: $(PLATFORM)"
	@echo "Target: $(TARGET)"
	@echo "Executable: $(BIN)"
	@if [ -f "$(BIN)" ]; then echo "✓ Build successful!"; ls -la $(BIN); else echo "✗ Build failed!"; fi

# No external physics library needed - using custom SoA system

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

# Compile rules
$(OBJ_DIR)/%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(OBJ_DIR)/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(OBJ_DIR)/%.o: src/%.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Sibling library sources compiled directly from source of truth
$(OBJ_DIR)/object_allocator.o: $(OA_LIB)/src/object_allocator.c $(OA_LIB)/include/object_allocator.h
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(OBJ_DIR)/spatial_hash.o: $(SPATIAL_QUERY_PATH)/src/spatial_hash.c $(SPATIAL_QUERY_PATH)/include/spatial_hash.h
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf build/

clean-all: clean
	rm -rf $(RAYLIB_PATH)/build/

help:
	@echo "=== ParticleDynamicsExample Build System ==="
	@echo "This project ALWAYS builds for Windows by default!"
	@echo ""
	@echo "Available targets:"
	@echo "  make             - Build Windows executable (default)"
	@echo "  make TARGET=linux - Build Linux executable"
	@echo "  make TARGET=macos - Build macOS executable"
	@echo ""
	@echo "Utility targets:"
	@echo "  make clean        - Remove build files"
	@echo "  make clean-all    - Remove all build files and libraries"
	@echo ""
	@echo "Current configuration:"
	@echo "  TARGET: $(TARGET)"
	@echo "  PLATFORM: $(PLATFORM)"
	@echo "  Output: $(BIN)"

-include $(OBJ:.o=.d)

.PHONY: all raylib clean clean-all help
