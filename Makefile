CC = gcc
CXX = g++
RAYLIB_PATH = ../Libraries/raylib
CFLAGS = -Wall -Wextra -O2 -I$(RAYLIB_PATH)/src -I./include
CXXFLAGS = -Wall -Wextra -O2 -std=c++14 -Wno-missing-field-initializers -I$(RAYLIB_PATH)/src -I./include

# Detect OS and set platform-specific variables
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    PLATFORM = macos
    PLATFORM_DEFINE = PLATFORM_DESKTOP
    LDFLAGS = -L$(RAYLIB_PATH)/build/$(PLATFORM) -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo -lm
    LDLIBS = $(RAYLIB_PATH)/build/$(PLATFORM)/libraylib.a
endif
ifeq ($(UNAME_S),Linux)
    PLATFORM = linux
    PLATFORM_DEFINE = PLATFORM_DESKTOP
    LDFLAGS = -L$(RAYLIB_PATH)/build/$(PLATFORM) -lGL -lm -lpthread -ldl -lrt -lX11
    LDLIBS = $(RAYLIB_PATH)/build/$(PLATFORM)/libraylib.a
endif
ifeq ($(OS),Windows_NT)
    PLATFORM = windows
    PLATFORM_DEFINE = PLATFORM_DESKTOP
    LDFLAGS = -L$(RAYLIB_PATH)/build/$(PLATFORM) -lopengl32 -lgdi32 -lwinmm
    LDLIBS = $(RAYLIB_PATH)/build/$(PLATFORM)/libraylib.a
endif

# Platform-specific directories
BUILD_DIR = build/$(PLATFORM)
OBJ_DIR = $(BUILD_DIR)/obj

# Create platform-specific directories
$(shell mkdir -p $(OBJ_DIR))
$(shell mkdir -p $(BUILD_DIR))
$(shell mkdir -p $(RAYLIB_PATH)/build/$(PLATFORM))

# C++ main application
SRC = main.cpp src/bvh_new.cpp src/object_allocator.c src/blas_manager.cpp src/tlas_manager.cpp src/bvh_visualizer.cpp
OBJ = $(OBJ_DIR)/main.o $(OBJ_DIR)/bvh_new.o $(OBJ_DIR)/object_allocator.o $(OBJ_DIR)/blas_manager.o $(OBJ_DIR)/tlas_manager.o $(OBJ_DIR)/bvh_visualizer.o
BIN = $(BUILD_DIR)/gpu_raytrace
PREPROCESSOR = $(BUILD_DIR)/shader_preprocessor

all: dependencies shaders $(BIN)

dependencies:
	@echo "Setting up dependencies for $(PLATFORM)..."
	@mkdir -p src
	@cp ../ObjectAllocatorLib/src/object_allocator.c src/

shaders: shaders/raytrace_tlas_blas_processed.fs

$(PREPROCESSOR): src/shader_preprocessor.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

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
	$(MAKE) -C $(RAYLIB_PATH)/src PLATFORM=$(PLATFORM_DEFINE) clean
	$(MAKE) -C $(RAYLIB_PATH)/src PLATFORM=$(PLATFORM_DEFINE)
	@cp $(RAYLIB_PATH)/src/libraylib.a $(RAYLIB_LIB)
	@echo "Raylib built and copied to $(RAYLIB_LIB)"

$(RAYLIB_FORCE_FILE):
	@echo "Creating platform marker for $(PLATFORM)..."
	@mkdir -p $(RAYLIB_PATH)/build/$(PLATFORM)
	@rm -f $(RAYLIB_PATH)/build/*/.raylib_*
	@touch $(RAYLIB_FORCE_FILE)

$(BIN): $(OBJ) raylib
	$(CXX) -o $@ $(OBJ) $(LDFLAGS) $(LDLIBS)
	@echo "Built executable for $(PLATFORM): $@"

# Build rules for main target (C++)
$(OBJ_DIR)/main.o: main.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

$(OBJ_DIR)/bvh_new.o: src/bvh_new.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

$(OBJ_DIR)/object_allocator.o: src/object_allocator.c
	$(CC) -c $< $(CFLAGS) -o $@

$(OBJ_DIR)/blas_manager.o: src/blas_manager.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

$(OBJ_DIR)/tlas_manager.o: src/tlas_manager.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

$(OBJ_DIR)/bvh_visualizer.o: src/bvh_visualizer.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

# Platform-specific clean
clean:
	rm -rf $(BUILD_DIR)
	rm -f *.o $(PREPROCESSOR) shaders/raytrace_tlas_blas_processed.fs

# Clean all platforms
clean-all:
	rm -rf build/
	rm -rf $(RAYLIB_PATH)/build/
	rm -f *.o shaders/raytrace_tlas_blas_processed.fs

# Force rebuild raylib for current platform
rebuild-raylib:
	rm -f $(RAYLIB_FORCE_FILE)
	$(MAKE) raylib

# Show current platform
platform:
	@echo "Current platform: $(PLATFORM)"
	@echo "Build directory: $(BUILD_DIR)"
	@echo "Raylib library: $(RAYLIB_LIB)"

.PHONY: all clean clean-all raylib shaders dependencies rebuild-raylib platform