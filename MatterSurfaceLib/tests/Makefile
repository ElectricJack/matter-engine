# Makefile for MatterSurfaceLib tests

# Detect platform
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

# Default to Linux settings
PLATFORM = PLATFORM_DESKTOP
LIBTYPE = STATIC
RAYLIB_RELEASE_PATH = ../../../Libraries/raylib
RAYLIB_PATH = $(RAYLIB_RELEASE_PATH)/src

# Platform-specific settings
ifeq ($(UNAME_S),Linux)
    PLATFORM = PLATFORM_DESKTOP
    GRAPHICS = GRAPHICS_API_OPENGL_33
endif

ifeq ($(UNAME_S),Darwin)
    PLATFORM = PLATFORM_DESKTOP
    GRAPHICS = GRAPHICS_API_OPENGL_33
    ifeq ($(UNAME_M),arm64)
        # Apple Silicon Mac
        MACOS_VERSION = 11.0
    else
        # Intel Mac
        MACOS_VERSION = 10.15
    endif
endif

# Compiler settings
CC = g++
CFLAGS = -std=c++17 -Wall -Wno-missing-braces -Wno-unused-variable -D$(PLATFORM) -D$(GRAPHICS)

ifeq ($(UNAME_S),Darwin)
    CFLAGS += -mmacosx-version-min=$(MACOS_VERSION)
    LDFLAGS = -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo
else
    LDFLAGS = -lGL -lm -lpthread -ldl -lrt -lX11
endif

# Include directories
INCLUDE_PATHS = -I../include -I$(RAYLIB_PATH) -I../../Libraries/raylib/src

# Library paths
LDLIBS = ../../Libraries/raylib/src/libraylib.a

# Source files
MAIN_SOURCE = minimal_cell_test.cpp
MATTER_SOURCES = 

# Target
TARGET = minimal_cell_test

# Build rules
$(TARGET): $(MAIN_SOURCE) $(MATTER_SOURCES)
	$(CC) $(MAIN_SOURCE) $(MATTER_SOURCES) -o $(TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)

.PHONY: clean run run-simp run-blas run-cell run-cont run-reg

clean:
	rm -f $(TARGET) $(SIMP_TARGET) $(BLAS_TARGET) $(CELL_TARGET) $(CONT_TARGET) $(REG_TARGET)

run: $(TARGET)
	./$(TARGET)

# Debug build
debug: CFLAGS += -g -DDEBUG
debug: $(TARGET)

# Mesh simplifier unit tests (headless, no GL window required)
SIMP_TARGET = mesh_simplifier_tests
SIMP_SOURCES = mesh_simplifier_tests.cpp ../src/mesh_simplifier.cpp

$(SIMP_TARGET): $(SIMP_SOURCES)
	$(CC) $(SIMP_SOURCES) -o $(SIMP_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)

run-simp: $(SIMP_TARGET)
	./$(SIMP_TARGET)

# BLAS reference-counting / re-mesh leak regression tests (headless, no GL window)
BLAS_TARGET = blas_refcount_tests
BLAS_SOURCES = blas_refcount_tests.cpp ../src/blas_manager.cpp ../src/bvh.cpp

$(BLAS_TARGET): $(BLAS_SOURCES)
	$(CC) $(BLAS_SOURCES) -o $(BLAS_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)

run-blas: $(BLAS_TARGET)
	./$(BLAS_TARGET)

# Cell geometric-bounds / coordinate-convention regression tests (headless).
# C sources must be compiled with the C compiler so their symbols stay
# unmangled (cell.cpp references GenerateMesh et al. via extern "C").
CELL_TARGET = cell_bounds_tests
CELL_CPP = cell_bounds_tests.cpp ../src/cell.cpp ../src/mesh_simplifier.cpp \
           ../src/blas_manager.cpp ../src/bvh.cpp ../src/bvh_analyzer.cpp
CELL_C = ../src/surface.c ../src/open_particle_surface.c \
         ../src/spatial_hash.c ../src/object_allocator.c ../src/material_registry.c

CELL_C_OBJ = surface.o open_particle_surface.o spatial_hash.o object_allocator.o material_registry.o

$(CELL_TARGET): $(CELL_CPP) $(CELL_C)
	gcc -c $(CELL_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(CELL_CPP) $(CELL_C_OBJ) -o $(CELL_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)
	rm -f $(CELL_C_OBJ)

run-cell: $(CELL_TARGET)
	./$(CELL_TARGET)

# Cross-cell continuity / edge-case reproduction harness (headless, GL-free).
# Replicates the cluster meshing pipeline without UploadMesh/BLAS, so it only
# needs the SurfaceLib C sources + the simplifier. C sources via gcc to keep
# their symbols unmangled for the extern "C" decls.
CONT_TARGET = mesh_continuity_tests
CONT_CPP = mesh_continuity_tests.cpp ../src/mesh_simplifier.cpp
CONT_C   = ../src/surface.c ../src/open_particle_surface.c \
           ../src/spatial_hash.c ../src/object_allocator.c

CONT_C_OBJ = surface.o open_particle_surface.o spatial_hash.o object_allocator.o

$(CONT_TARGET): $(CONT_CPP) $(CONT_C)
	gcc -c $(CONT_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(CONT_CPP) $(CONT_C_OBJ) -o $(CONT_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)
	rm -f $(CONT_C_OBJ)

run-cont: $(CONT_TARGET)
	./$(CONT_TARGET)

# Material registry unit tests (headless, GL-free)
REG_TARGET = material_registry_tests
REG_CPP = material_registry_tests.cpp
REG_C   = ../src/material_registry.c
REG_C_OBJ = material_registry.o

$(REG_TARGET): $(REG_CPP) $(REG_C)
	gcc -c $(REG_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(REG_CPP) $(REG_C_OBJ) -o $(REG_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)
	rm -f $(REG_C_OBJ)

run-reg: $(REG_TARGET)
	./$(REG_TARGET)