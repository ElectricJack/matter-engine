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

CXX = g++
ENGINE_INCLUDE = ../../../MatterEngine3/include
CXXFLAGS = -std=c++17 -Wall -Wextra -g -I../include -I$(ENGINE_INCLUDE)

OBJ_DIR = build

TARGET = $(OBJ_DIR)/mathlib_tests

all: test

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(TARGET): mathlib_tests.cpp ../include/matter_math.h $(ENGINE_INCLUDE)/matter/math_types.h | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -o $(TARGET) mathlib_tests.cpp

test: $(TARGET)
	./$(TARGET)

clean:
	rm -rf $(OBJ_DIR)

.PHONY: all test clean
