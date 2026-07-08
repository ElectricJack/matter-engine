# Libraries/autoremesher_core/Makefile
# Builds libautoremesher_core.a -- the headless subset of huxingyi/autoremesher.
#
# See UPSTREAM.md for what's vendored. This library links together the
# vendored autoremesher pipeline sources with a subset of geogram
# (BSD-3 core + exploragram hexdom), the isotropicremesher helper tree,
# Eigen (MPL-2 core only), and legacy TBB headers. No Qt is pulled in.
# No GPL modules from geogram (tetgen, triangle, lua) are compiled or
# included on the -I path.
#
# Design notes:
# - We compile geogram's core (basic, api, mesh, delaunay, voronoi,
#   points, numerics, image, parameterization, bibliography, NL, and
#   the aux top-level .cpp) plus exploragram (hexdom + optimal_transport
#   + basic). All resolved via shell wildcards -- geogram's own CMake
#   uses aux_source_directories() which is effectively the same globbing.
# - We do NOT compile geogram's src/lib/third_party/ subtree (that's
#   ARPACK/CBLAS/CLAPACK/SUPERLU/LIBF2C -- LGPL-tainted legacy numerics).
#   The OpenNL solver reaches these through nlInitExtension_SUPERLU() etc,
#   which we stub in src/nl_ext_stubs.c to return NL_FALSE so the solver
#   degrades to its iterative fallback. Same story for CHOLMOD, MKL, CUDA.
# - We do NOT compile geogram's lua/ subdir (unused by the pipeline).
# - TBB: header-only usage from the pipeline sources; the runtime symbols
#   (task, task_group_context, arena) remain unresolved in the .a --
#   consumers of this static library must link against a TBB runtime or
#   provide their own stubs (Task 6 will address this at driver link).
# - Eigen: header-only, MPL-2 core only. We MUST NOT #include from
#   Eigen/src/SuperLUSupport/, Eigen/src/UmfPackSupport/ etc (GPL/LGPL).
#   The pipeline only pulls in <Eigen/Dense>, which is MPL-2.
# - -fPIC is required so consumers (MSL, MatterEngine3) can link the .a
#   into their own shared / static libraries without relocation errors.

CXX      ?= g++
CC       ?= gcc
AR       ?= ar

# Common warnings-off: geogram core has many -Wunused etc.; upstream also
# suppresses them in vor_reset_warning_level(). Deprecated-declarations for
# std::iterator warnings in geogram templates on modern libstdc++.
COMMON_FLAGS := -O2 -fPIC \
                -Wall -Wno-unused -Wno-unused-parameter \
                -Wno-sign-compare -Wno-strict-aliasing \
                -Wno-missing-field-initializers \
                -Wno-deprecated -Wno-deprecated-declarations \
                -Wno-attributes -Wno-parentheses \
                -Wno-narrowing

# -Wno-changes-meaning is C++-only (fires on TBB legacy task.h).
CXX_ONLY_FLAGS := -Wno-changes-meaning

# Geogram compile-time switches (match its own CMakeLists.txt for the
# lib target so headers see the same macro world):
#   GEOGRAM_WITH_PDEL -- parallel Delaunay (fine on Linux)
# NL_WITH_AMGCL is NOT set: amgcl.hpp lives in geogram/third_party/amgcl
# which is not vendored in this snapshot. Without the flag, OpenNL falls
# back to its built-in iterative solvers -- which is what the autoremesher
# parameterizer uses anyway.
GEOGRAM_DEFINES := -DGEOGRAM_WITH_PDEL

INCLUDES := -Iinclude \
            -Ithirdparty/eigen \
            -Ithirdparty/geogram \
            -Ithirdparty/geogram/src/lib \
            -Ithirdparty/geogram/src/lib/geogram/third_party/libMeshb/sources \
            -Ithirdparty/geogram/src/lib/geogram/third_party/rply \
            -Ithirdparty/geogram/src/lib/geogram/third_party/zlib \
            -Ithirdparty/isotropicremesher \
            -Ithirdparty/tbb/include

# gcc-13 tightened up header transitivity: several vendored files rely on
# older gcc's implicit fixed-width-int / thread hoisting via <QDebug>, TBB
# umbrella headers, etc. Force-include what upstream got for free. This is
# a build-flag workaround (not a source edit).
FORCE_INCLUDES := -include cstdint

CXXFLAGS := -std=c++17 $(COMMON_FLAGS) $(CXX_ONLY_FLAGS) $(GEOGRAM_DEFINES) $(INCLUDES) $(FORCE_INCLUDES)
CFLAGS   := -std=gnu11 $(COMMON_FLAGS) $(GEOGRAM_DEFINES) $(INCLUDES)

# ---------------------------------------------------------------------
# Pipeline sources (Task 3: cherry-picked from upstream + local shim)
# ---------------------------------------------------------------------
# Task 6: remesh.cpp -- the top-level headless driver composing sanitize ->
# parameterize -> quad-extract -> triangulate behind autoremesher::remesh().
# Adds AUTOREMESHER_CORE_VERSION as well.
PIPELINE_CPP := \
    src/remesh.cpp \
    src/autoremesher.cpp \
    src/mesh_sanitize.cpp \
    src/mesh_separator.cpp \
    src/param_hdc.cpp \
    src/position_key.cpp \
    src/quad_extract.cpp \
    src/quad_to_tri.cpp

PIPELINE_C := \
    src/nl_ext_stubs.c

# ---------------------------------------------------------------------
# Geogram sources (BSD-3 core). Skips lua/ and third_party/.
# aux_source_directories() in geogram's CMake is effectively:
#   file(GLOB SOURCES *.cpp *.c) per subdir.
# ---------------------------------------------------------------------
GEOGRAM_ROOT := thirdparty/geogram/src/lib/geogram
GEOGRAM_DIRS := . basic api mesh delaunay voronoi points numerics \
                image parameterization bibliography NL

GEOGRAM_CPP := $(foreach d,$(GEOGRAM_DIRS),$(wildcard $(GEOGRAM_ROOT)/$(d)/*.cpp))
GEOGRAM_C   := $(foreach d,$(GEOGRAM_DIRS),$(wildcard $(GEOGRAM_ROOT)/$(d)/*.c))

# Exclusions -- files that require optional external deps we don't have.
#   nl_amgcl.cpp -- needs geogram/third_party/amgcl (not vendored). Its
#     #else-branch stub has a typo (`nlBoolean` vs `NLboolean`) and won't
#     compile. We provide a working stub in src/nl_ext_stubs.c instead.
GEOGRAM_CPP := $(filter-out $(GEOGRAM_ROOT)/NL/nl_amgcl.cpp,$(GEOGRAM_CPP))

# ---------------------------------------------------------------------
# Exploragram sources (still BSD-3, geogram's experimental hexdom
# pipeline -- the parameterizer stage pulls in quad_cover.h etc).
# ---------------------------------------------------------------------
EXPLORAGRAM_ROOT := thirdparty/geogram/src/lib/exploragram
EXPLORAGRAM_DIRS := . hexdom optimal_transport

EXPLORAGRAM_CPP := $(foreach d,$(EXPLORAGRAM_DIRS),$(wildcard $(EXPLORAGRAM_ROOT)/$(d)/*.cpp))

# ---------------------------------------------------------------------
# Isotropicremesher helper tree (MIT). Small; just its two .cpp files.
# ---------------------------------------------------------------------
ISOTROPIC_ROOT := thirdparty/isotropicremesher
ISOTROPIC_CPP := $(wildcard $(ISOTROPIC_ROOT)/*.cpp)

# ---------------------------------------------------------------------
# Geogram third_party helpers required to satisfy mesh_io.cpp / logger /
# etc. static references. Only permissively-licensed pieces:
#   - libMeshb (MIT) -- .meshb/.mesh file format support
#   - rply     (MIT) -- .ply file format support
#   - zlib     (permissive) -- generic compression, pulled in by both above
# These are NEVER called from our headless pipeline (we operate on
# in-memory buffers), but the linker requires the symbols to resolve
# because mesh_io.cpp unconditionally references them.
# UPSTREAM.md still bans lua/, HLBFGS/, PoissonRecon/, triangle/, xatlas/,
# stb_image/ from being compiled (unused + licenses vary).
# ---------------------------------------------------------------------
GEO_TP_ROOT := thirdparty/geogram/src/lib/geogram/third_party
GEO_TP_C := \
    $(GEO_TP_ROOT)/libMeshb/sources/libmeshb7.c \
    $(GEO_TP_ROOT)/rply/rply.c \
    $(wildcard $(GEO_TP_ROOT)/zlib/*.c)

# libMeshb has one extra include quirk on Linux: no changes needed for
# our snapshot. rply is a single translation unit. zlib compiles standalone.

# ---------------------------------------------------------------------
# TBB: header-only from this library's perspective. The runtime .cpp
# files under thirdparty/tbb/src/tbb/ are NOT compiled here -- consumers
# link a TBB runtime at final link time. Left as a variable in case it
# needs to be flipped on later.
# ---------------------------------------------------------------------
TBB_CPP :=

# ---------------------------------------------------------------------
# Object aggregation
# ---------------------------------------------------------------------
ALL_CPP := $(PIPELINE_CPP) $(GEOGRAM_CPP) $(EXPLORAGRAM_CPP) $(ISOTROPIC_CPP) $(TBB_CPP)
ALL_C   := $(PIPELINE_C) $(GEOGRAM_C) $(GEO_TP_C)

CPP_OBJ := $(ALL_CPP:.cpp=.o)
C_OBJ   := $(ALL_C:.c=.o)
ALL_OBJ := $(CPP_OBJ) $(C_OBJ)

# ---------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------
libautoremesher_core.a: $(ALL_OBJ)
	$(AR) rcs $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	find src -name '*.o' -delete 2>/dev/null || true
	find thirdparty -name '*.o' -delete 2>/dev/null || true
	rm -f libautoremesher_core.a

.PHONY: clean
