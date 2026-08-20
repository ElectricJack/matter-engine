# libs/MathLib/tests/Makefile
#
# Standalone test binary for libs/MathLib/include/matter_math.h. MathLib
# itself is header-only and has zero dependencies; this Makefile pulls in
# exactly one additional include path (MatterEngine3/include) so the TRS
# test can compare against matter::Mat4f/Float3/Quaternion, which are
# themselves a standalone POD header with no further includes (see the
# comment in mathlib_tests.cpp for why the actual transform_math.h function
# isn't linked directly -- it drags in flecs).
#
# Deliberately does NOT touch MatterEngine3/tests/Makefile.

# Shared build config (TMP/TEMP, GLSLC, parallelism, ccache detection).
include ../../../platform.mk

CXX = g++
ENGINE_INCLUDE = ../../../MatterEngine3/include
CXXFLAGS = -std=c++17 -Wall -Wextra -g -I../include -I$(ENGINE_INCLUDE)

OBJ_DIR = build

TARGET = $(OBJ_DIR)/mathlib_tests

# C smoke-compile target (Phase 4 Step 2): proves matter_math_c.h is valid C,
# compiled with the C compiler rather than g++ -- see matter_math_c_smoke.c's
# header comment for why this needs to be a separate gcc invocation instead
# of just relying on mathlib_tests.cpp (which is built with g++ and would
# silently accept C++-only syntax leaking into the header).
CC = gcc
CSMOKE_TARGET = $(OBJ_DIR)/matter_math_c_smoke

all: test c-smoke

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(TARGET): mathlib_tests.cpp ../include/matter_math.h ../include/matter_math_c.h $(ENGINE_INCLUDE)/matter/math_types.h | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -o $(TARGET) mathlib_tests.cpp

$(CSMOKE_TARGET): matter_math_c_smoke.c ../include/matter_math_c.h | $(OBJ_DIR)
	$(CC) -std=c99 -Wall -Wextra -g -I../include -o $(CSMOKE_TARGET) matter_math_c_smoke.c

test: $(TARGET)
	./$(TARGET)

c-smoke: $(CSMOKE_TARGET)
	./$(CSMOKE_TARGET)
	@echo "matter_math_c.h compiles and runs as plain C99"

clean:
	rm -rf $(OBJ_DIR)

.PHONY: all test c-smoke clean
