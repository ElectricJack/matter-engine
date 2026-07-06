# Makefile for MatterEngine3 (script host + voxel-CSG bake library).
#
# MatterEngine3 is a library sub-project: it has no application `main` of its own
# yet, so `make` builds a static archive (libmatter_engine3.a) that bundles the
# script host, the DSL/CSG bake pipeline, SP-1's part-asset v2 serializer, and the
# vendored QuickJS-ng engine. A future consumer app links this archive.
#
# MatterEngine3 consumes the MatterSurfaceLib prototype READ-ONLY:
#   new headers via -Iinclude, consumed prototype headers via
#   -I../MatterSurfaceLib/include. QuickJS-ng is vendored under
#   ../Libraries/quickjs-ng and compiled with gcc so its extern "C" symbols stay
#   unmangled. Per CLAUDE.md memory: after struct/header changes, prefer
#   `make clean && make` to avoid stale objects.

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

PLATFORM = PLATFORM_DESKTOP
GRAPHICS = GRAPHICS_API_OPENGL_33

ifeq ($(UNAME_S),Darwin)
    ifeq ($(UNAME_M),arm64)
        MACOS_VERSION = 11.0
    else
        MACOS_VERSION = 10.15
    endif
endif

CC      = g++
CFLAGS  = -std=c++17 -Wall -Wno-missing-braces -Wno-unused-variable -O2 -D$(PLATFORM) -D$(GRAPHICS)
ifeq ($(UNAME_S),Darwin)
    CFLAGS += -mmacosx-version-min=$(MACOS_VERSION)
endif

RAYLIB_PATH = ../Libraries/raylib/src

# Include directories: NEW MatterEngine3 headers, consumed MatterSurfaceLib
# headers, vendored QuickJS-ng, then raylib (for the BLAS/Tri/TriEx types).
QJS_DIR       = ../Libraries/quickjs-ng
BOX3D_DIR     = ../Libraries/box3d
INCLUDE_PATHS = -Iinclude -I../MatterSurfaceLib/include -I$(QJS_DIR) -I$(RAYLIB_PATH) -I$(BOX3D_DIR)/include

# Vendored QuickJS-ng engine. v0.10.0 needs xsum.c (xsum_small_* symbols) beyond
# the core four. Compiled via gcc (C) so the embedding symbols stay unmangled.
QJS_C   = $(QJS_DIR)/quickjs.c $(QJS_DIR)/libregexp.c \
          $(QJS_DIR)/libunicode.c $(QJS_DIR)/cutils.c $(QJS_DIR)/xsum.c
QJS_OBJ = quickjs.o libregexp.o libunicode.o cutils.o xsum.o

# NEW MatterEngine3 host/DSL/CSG sources + SP-1's part-asset v2 serializer.
ME3_CPP = src/script_host.cpp src/dsl_state.cpp src/dsl_bindings.cpp \
          src/csg_lowering.cpp src/part_asset_v2.cpp src/world_lights.cpp \
          src/probe_volume.cpp src/world_tracer.cpp src/part_cluster.cpp \
          src/tileset_layout.cpp src/tileset_collider.cpp src/tileset_settle.cpp \
          src/tileset_placement.cpp src/tileset_part_collider.cpp src/tileset_bake.cpp \
          src/tileset_phase.cpp src/tileset_gtex.cpp src/tileset_torus_bvh.cpp
ME3_OBJ = script_host.o dsl_state.o dsl_bindings.o csg_lowering.o part_asset_v2.o world_lights.o \
          probe_volume.o world_tracer.o part_cluster.o tileset_layout.o tileset_collider.o \
          tileset_settle.o tileset_placement.o tileset_part_collider.o tileset_bake.o \
          tileset_phase.o tileset_gtex.o tileset_torus_bvh.o

LIB = libmatter_engine3.a

.PHONY: all clean test

all: $(LIB)

$(QJS_OBJ): $(QJS_C)
	gcc -c $(QJS_C) -O2 -DCONFIG_VERSION='"0.10.0"' -I$(QJS_DIR)

$(ME3_OBJ): $(ME3_CPP)
	$(CC) -c $(ME3_CPP) $(CFLAGS) $(INCLUDE_PATHS)

$(LIB): $(QJS_OBJ) $(ME3_OBJ)
	ar rcs $(LIB) $(QJS_OBJ) $(ME3_OBJ)

# Delegate to the test suite (headless, GL-free host + raylib-linked BLAS path).
test:
	$(MAKE) -C tests run-partv2
	$(MAKE) -C tests run-script

clean:
	rm -f $(LIB) $(QJS_OBJ) $(ME3_OBJ)
	$(MAKE) -C tests clean
