# ParticleFlowLib: generic agent-particle kernel + path recording.
# Standalone build for dev/tests; MatterEngine3 compiles src/ directly
# into libmatter_engine3.a (MatterSurfaceLib direct-source pattern).

# Shared build config (TMP/TEMP, GLSLC, parallelism, ccache detection).
include ../../platform.mk

CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -g -O2 -I./include
OBJ_DIR = build/obj
SRCS = $(wildcard src/*.cpp)
OBJS = $(SRCS:src/%.cpp=$(OBJ_DIR)/%.o)
HEADERS = $(wildcard include/*.h)

all: build/libparticleflow.a

build/libparticleflow.a: $(OBJS)
	ar rcs $@ $^

$(OBJ_DIR)/%.o: src/%.cpp $(HEADERS)
	@mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# -MMD -MP above already writes a .d file per object; nothing ever included
# them, so this Makefile relied solely on the coarse $(HEADERS) prerequisite
# above (any header change recompiles every .o, not just objects that use
# it). Pick up the generated .d files too for precise tracking.
-include $(wildcard $(OBJ_DIR)/*.d)

# The sanitizers are Linux-only here: MSYS2's mingw-w64 GCC ships no libasan /
# libubsan, so passing -fsanitize=address,undefined makes the LINK fail with
# "cannot find -lasan". platform.mk already detects Windows, so gate on that
# rather than leaving the whole target unbuildable on the repo's main platform.
ifeq ($(IS_WINDOWS),Windows_NT)
SANITIZE_FLAGS =
else
SANITIZE_FLAGS = -fsanitize=address,undefined
endif

TEST_FLAGS = -std=c++17 -Wall -Wextra -g -I./include $(SANITIZE_FLAGS)

test: tests/pf_tests.cpp $(SRCS) $(HEADERS) | build
	$(CXX) $(TEST_FLAGS) -o build/pf_tests tests/pf_tests.cpp $(SRCS)
	./build/pf_tests

build:
	@mkdir -p $@

clean:
	rm -rf build

.PHONY: all test clean
