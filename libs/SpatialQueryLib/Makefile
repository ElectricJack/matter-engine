# Shared build config (TMP/TEMP, GLSLC, parallelism, ccache detection).
include ../../platform.mk

CC = gcc
MEMORY_LIB = ../MemoryLib
CFLAGS = -Wall -Wextra -g -I./include -I$(MEMORY_LIB)/include
LDFLAGS = -lm

SRC_DIR = src
INCLUDE_DIR = include
OBJ_DIR = build/obj

# Create object directory if it doesn't exist
$(shell mkdir -p $(OBJ_DIR))
$(shell mkdir -p $(OBJ_DIR)/$(SRC_DIR))

# Source files — mem_pool compiled from MemoryLib (source of truth)
SRCS = main.c src/spatial_hash.c
OBJS = $(OBJ_DIR)/main.o $(OBJ_DIR)/src/spatial_hash.o $(OBJ_DIR)/src/mem_pool.o

# Header files
HEADERS = $(wildcard $(INCLUDE_DIR)/*.h)

# Target executable
TARGET = build/spatialquerylib

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Rule for main.c (in root directory)
$(OBJ_DIR)/main.o: main.c $(HEADERS)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Rule for source files in src directory
$(OBJ_DIR)/$(SRC_DIR)/%.o: $(SRC_DIR)/%.c $(HEADERS)
	mkdir -p $(OBJ_DIR)/$(SRC_DIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# mem_pool compiled from MemoryLib (source of truth)
$(OBJ_DIR)/src/mem_pool.o: $(MEMORY_LIB)/src/mem_pool.c $(MEMORY_LIB)/include/mem_pool.h
	mkdir -p $(OBJ_DIR)/src
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf build

# Test targets.
#
# The sanitizers are Linux-only here: MSYS2's mingw-w64 GCC ships no libasan /
# libubsan, so passing -fsanitize=address,undefined makes the LINK fail with
# "cannot find -lasan". platform.mk already detects Windows, so gate on that
# rather than leaving the whole target unbuildable on the repo's main platform.
ifeq ($(IS_WINDOWS),Windows_NT)
SANITIZE_FLAGS =
else
SANITIZE_FLAGS = -fsanitize=address,undefined
endif

TEST_SRCS = tests/spatial_hash_tests.c src/spatial_hash.c $(MEMORY_LIB)/src/mem_pool.c
TEST_FLAGS = -Wall -Wextra -g -I./include -I$(MEMORY_LIB)/include $(SANITIZE_FLAGS) -lm

# BVH / TLAS / analyzer regression tests (C++, headless, no GL and no files).
CXX = g++
BVH_TEST_SRCS = tests/bvh_tests.cpp src/bvh.cpp src/bvh_analyzer.cpp
BVH_TEST_FLAGS = -Wall -Wextra -g -std=c++17 -I./include $(SANITIZE_FLAGS)

test: test-hash test-bvh

test-hash: $(TEST_SRCS)
	$(CC) $(TEST_FLAGS) -o build/spatial_hash_tests $(TEST_SRCS)
	./build/spatial_hash_tests

test-bvh: $(BVH_TEST_SRCS) $(HEADERS)
	$(CXX) $(BVH_TEST_FLAGS) -o build/bvh_tests $(BVH_TEST_SRCS)
	./build/bvh_tests

-include $(OBJS:.o=.d)

.PHONY: all clean test test-hash test-bvh
