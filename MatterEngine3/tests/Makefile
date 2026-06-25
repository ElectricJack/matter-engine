# Makefile for MatterEngine3 tests.
# MatterEngine3 consumes the MatterSurfaceLib prototype READ-ONLY:
#   new headers via -I../include, consumed prototype headers via
#   -I../../MatterSurfaceLib/include, consumed prototype sources as
#   ../../MatterSurfaceLib/src/<dep>. raylib paths match MatterSurfaceLib/tests
#   (same directory depth).

# Detect platform
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

# Default to Linux settings
PLATFORM = PLATFORM_DESKTOP
LIBTYPE = STATIC
RAYLIB_RELEASE_PATH = ../../../Libraries/raylib
RAYLIB_PATH = $(RAYLIB_RELEASE_PATH)/src

ifeq ($(UNAME_S),Linux)
    PLATFORM = PLATFORM_DESKTOP
    GRAPHICS = GRAPHICS_API_OPENGL_33
endif

ifeq ($(UNAME_S),Darwin)
    PLATFORM = PLATFORM_DESKTOP
    GRAPHICS = GRAPHICS_API_OPENGL_33
    ifeq ($(UNAME_M),arm64)
        MACOS_VERSION = 11.0
    else
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

# Include directories: NEW MatterEngine3 headers, then consumed MatterSurfaceLib
# headers, then raylib.
INCLUDE_PATHS = -I../include -I../../MatterSurfaceLib/include -I$(RAYLIB_PATH) -I../../Libraries/raylib/src

# Library paths (match MatterSurfaceLib/tests; same depth).
ifeq ($(UNAME_S),Linux)
    LDLIBS = ../../Libraries/raylib/build/linux/libraylib.a
else ifeq ($(UNAME_S),Darwin)
    LDLIBS = ../../Libraries/raylib/build/macos/libraylib.a
else
    LDLIBS = ../../Libraries/raylib/src/libraylib.a
endif

.PHONY: clean run-partv2 run-script

# Part Artifact v2 round-trip + resolved-hash + format-guard tests (headless, GL-free).
# NEW impl ../src/part_asset_v2.cpp + consumed MatterSurfaceLib backend sources.
# material_registry.c via gcc to keep its extern "C" symbols unmangled.
PARTV2_TARGET = part_asset_v2_tests
PARTV2_CPP = part_asset_v2_tests.cpp ../src/part_asset_v2.cpp \
             ../../MatterSurfaceLib/src/part_asset.cpp \
             ../../MatterSurfaceLib/src/blas_manager.cpp \
             ../../MatterSurfaceLib/src/bvh.cpp \
             ../../MatterSurfaceLib/src/tlas_manager.cpp \
             ../../MatterSurfaceLib/src/vertex_ao.cpp \
             ../../MatterSurfaceLib/src/occupancy.cpp
PARTV2_C   = ../../MatterSurfaceLib/src/material_registry.c
PARTV2_C_OBJ = material_registry.o

$(PARTV2_TARGET): $(PARTV2_CPP) $(PARTV2_C)
	gcc -c $(PARTV2_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(PARTV2_CPP) $(PARTV2_C_OBJ) -o $(PARTV2_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)
	rm -f $(PARTV2_C_OBJ)

run-partv2: $(PARTV2_TARGET)
	./$(PARTV2_TARGET)

clean:
	rm -f $(PARTV2_TARGET) $(SCRIPT_TARGET)

# Script host (QuickJS-ng) + DSL/CSG bake tests (headless, GL-free for the host;
# the BLAS surface path links raylib like the part-asset tests). QuickJS-ng C
# sources are compiled via gcc so their symbols stay unmangled for extern "C".
# v0.10.0 requires xsum.c (xsum_small_* symbols) in addition to the plan's core set.
QJS_DIR  = ../../Libraries/quickjs-ng
QJS_INC  = -I$(QJS_DIR)
QJS_C    = $(QJS_DIR)/quickjs.c $(QJS_DIR)/libregexp.c \
           $(QJS_DIR)/libunicode.c $(QJS_DIR)/cutils.c $(QJS_DIR)/xsum.c
QJS_OBJ  = quickjs.o libregexp.o libunicode.o cutils.o xsum.o

SCRIPT_TARGET = script_host_tests
SCRIPT_CPP = script_host_tests.cpp ../src/script_host.cpp ../src/dsl_state.cpp \
             ../src/part_asset_v2.cpp \
             ../../MatterSurfaceLib/src/part_asset.cpp \
             ../../MatterSurfaceLib/src/blas_manager.cpp \
             ../../MatterSurfaceLib/src/bvh.cpp \
             ../../MatterSurfaceLib/src/tlas_manager.cpp \
             ../../MatterSurfaceLib/src/vertex_ao.cpp \
             ../../MatterSurfaceLib/src/occupancy.cpp
SCRIPT_C   = ../../MatterSurfaceLib/src/material_registry.c

$(SCRIPT_TARGET): $(SCRIPT_CPP) $(SCRIPT_C) $(QJS_C)
	gcc -c $(QJS_C) -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC)
	gcc -c $(SCRIPT_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(SCRIPT_CPP) $(QJS_OBJ) material_registry.o -o $(SCRIPT_TARGET) \
	      $(CFLAGS) $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS)
	rm -f $(QJS_OBJ) material_registry.o

run-script: $(SCRIPT_TARGET)
	./$(SCRIPT_TARGET)
