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

# Library paths. The src/ archive is the Windows (MinGW COFF) cross-compile
# output; Linux/macOS test builds must link the native ELF/Mach-O archive under
# build/<platform>/ (matching the main Makefile's per-platform LDLIBS).
ifeq ($(UNAME_S),Linux)
    LDLIBS = ../../Libraries/raylib/build/linux/libraylib.a
else ifeq ($(UNAME_S),Darwin)
    LDLIBS = ../../Libraries/raylib/build/macos/libraylib.a
else
    LDLIBS = ../../Libraries/raylib/src/libraylib.a
endif

# Source files
MAIN_SOURCE = minimal_cell_test.cpp
MATTER_SOURCES = 

# Target
TARGET = minimal_cell_test

# Build rules
$(TARGET): $(MAIN_SOURCE) $(MATTER_SOURCES)
	$(CC) $(MAIN_SOURCE) $(MATTER_SOURCES) -o $(TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)

.PHONY: clean run run-simp run-blas run-cell run-cont run-reg run-tint run-cull run-par run-cube run-ao run-part

clean:
	rm -f $(TARGET) $(SIMP_TARGET) $(BLAS_TARGET) $(CELL_TARGET) $(CONT_TARGET) $(REG_TARGET) $(TINT_TARGET) $(CULL_TARGET) $(PAR_TARGET) $(CUBE_TARGET) $(AO_TARGET) $(PART_TARGET)

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
BLAS_SOURCES = blas_refcount_tests.cpp ../src/blas_manager.cpp ../src/bvh.cpp ../src/vertex_ao.cpp ../src/occupancy.cpp

$(BLAS_TARGET): $(BLAS_SOURCES)
	$(CC) $(BLAS_SOURCES) -o $(BLAS_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)

run-blas: $(BLAS_TARGET)
	./$(BLAS_TARGET)

# Cell geometric-bounds / coordinate-convention regression tests (headless).
# C sources must be compiled with the C compiler so their symbols stay
# unmangled (cell.cpp references GenerateMesh et al. via extern "C").
CELL_TARGET = cell_bounds_tests
CELL_CPP = cell_bounds_tests.cpp ../src/cell.cpp ../src/mesh_simplifier.cpp \
           ../src/blas_manager.cpp ../src/bvh.cpp ../src/bvh_analyzer.cpp \
           ../src/mesh_build_utils.cpp ../src/meshing_algorithm.cpp \
           ../src/marching_cubes_algorithm.cpp ../src/oriented_cube_algorithm.cpp \
           ../src/vertex_ao.cpp ../src/occupancy.cpp
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

# Per-triangle tint: GPU pack helper + dedup-identity regression (headless, no GL)
TINT_TARGET = blas_tint_tests
TINT_SOURCES = blas_tint_tests.cpp ../src/blas_manager.cpp ../src/bvh.cpp ../src/vertex_ao.cpp ../src/occupancy.cpp

$(TINT_TARGET): $(TINT_SOURCES)
	$(CC) $(TINT_SOURCES) -o $(TINT_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)

run-tint: $(TINT_TARGET)
	./$(TINT_TARGET)

# Occupancy-based interior culling unit tests (headless, no GL window)
CULL_TARGET = particle_culling_tests
CULL_SOURCES = particle_culling_tests.cpp ../src/lattice.cpp ../src/occupancy.cpp ../src/particle_culling.cpp

$(CULL_TARGET): $(CULL_SOURCES)
	$(CC) $(CULL_SOURCES) -o $(CULL_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)

run-cull: $(CULL_TARGET)
	./$(CULL_TARGET)

# Baked per-vertex AO unit tests (headless, no GL window)
AO_TARGET = vertex_ao_tests
AO_SOURCES = vertex_ao_tests.cpp ../src/vertex_ao.cpp ../src/lattice.cpp ../src/occupancy.cpp

$(AO_TARGET): $(AO_SOURCES)
	$(CC) $(AO_SOURCES) -o $(AO_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)

run-ao: $(AO_TARGET)
	./$(AO_TARGET)

# Per-cell parallel-meshing determinism harness (headless, GL-free at runtime).
# Mirrors the CELL target's C-source handling (gcc for unmangled extern "C")
# plus the worker pool. Never calls UploadMesh, so no GL context is needed.
PAR_TARGET = parallel_mesh_tests
PAR_CPP = parallel_mesh_tests.cpp ../src/cell.cpp ../src/mesh_simplifier.cpp \
          ../src/blas_manager.cpp ../src/bvh.cpp ../src/bvh_analyzer.cpp \
          ../src/mesh_worker_pool.cpp ../src/mesh_build_utils.cpp \
          ../src/meshing_algorithm.cpp ../src/marching_cubes_algorithm.cpp \
          ../src/oriented_cube_algorithm.cpp ../src/vertex_ao.cpp ../src/occupancy.cpp
PAR_C = ../src/surface.c ../src/open_particle_surface.c \
        ../src/spatial_hash.c ../src/object_allocator.c ../src/material_registry.c

PAR_C_OBJ = surface.o open_particle_surface.o spatial_hash.o object_allocator.o material_registry.o

$(PAR_TARGET): $(PAR_CPP) $(PAR_C)
	gcc -c $(PAR_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(PAR_CPP) $(PAR_C_OBJ) -o $(PAR_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)
	rm -f $(PAR_C_OBJ)

run-par: $(PAR_TARGET)
	./$(PAR_TARGET)

# Oriented-cube meshing algorithm unit tests (headless, GL-free; needs only the
# material registry for albedo and raylib for MemAlloc/MemFree).
CUBE_TARGET = oriented_cube_tests
CUBE_CPP = oriented_cube_tests.cpp ../src/oriented_cube_algorithm.cpp
CUBE_C   = ../src/material_registry.c
CUBE_C_OBJ = material_registry.o

$(CUBE_TARGET): $(CUBE_CPP) $(CUBE_C)
	gcc -c $(CUBE_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(CUBE_CPP) $(CUBE_C_OBJ) -o $(CUBE_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)
	rm -f $(CUBE_C_OBJ)

run-cube: $(CUBE_TARGET)
	./$(CUBE_TARGET)

# Part serialization round-trip + format-guard tests (headless, GL-free).
# material_registry.c via gcc to keep its extern "C" symbols unmangled.
PART_TARGET = part_asset_tests
PART_CPP = part_asset_tests.cpp ../src/part_asset.cpp ../src/blas_manager.cpp \
           ../src/bvh.cpp ../src/tlas_manager.cpp ../src/vertex_ao.cpp \
           ../src/occupancy.cpp
PART_C   = ../src/material_registry.c
PART_C_OBJ = material_registry.o

$(PART_TARGET): $(PART_CPP) $(PART_C)
	gcc -c $(PART_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(PART_CPP) $(PART_C_OBJ) -o $(PART_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)
	rm -f $(PART_C_OBJ)

run-part: $(PART_TARGET)
	./$(PART_TARGET)