CC = gcc
OA_LIB = ../ObjectAllocatorLib
CFLAGS = -Wall -Wextra -g -I./include -I$(OA_LIB)/include
LDFLAGS = -lm

SRC_DIR = src
INCLUDE_DIR = include
OBJ_DIR = obj

# Create object directory if it doesn't exist
$(shell mkdir -p $(OBJ_DIR))
$(shell mkdir -p $(OBJ_DIR)/$(SRC_DIR))

# Source files — object_allocator compiled from ObjectAllocatorLib (source of truth)
SRCS = main.c src/spatial_hash.c src/bvh.c
OBJS = $(OBJ_DIR)/main.o $(OBJ_DIR)/src/spatial_hash.o $(OBJ_DIR)/src/bvh.o $(OBJ_DIR)/src/object_allocator.o

# Header files
HEADERS = $(wildcard $(INCLUDE_DIR)/*.h)

# Target executable
TARGET = spatialquerylib

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

# object_allocator compiled from ObjectAllocatorLib (source of truth)
$(OBJ_DIR)/src/object_allocator.o: $(OA_LIB)/src/object_allocator.c $(OA_LIB)/include/object_allocator.h
	mkdir -p $(OBJ_DIR)/src
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf $(OBJ_DIR) $(TARGET) spatial_hash_tests bvh_tests

# Test target: build and run spatial hash tests with AddressSanitizer + UBSan
TEST_SRCS = tests/spatial_hash_tests.c src/spatial_hash.c $(OA_LIB)/src/object_allocator.c
TEST_FLAGS = -Wall -Wextra -g -I./include -I$(OA_LIB)/include -fsanitize=address,undefined -lm

# BVH / matrix_inverse tests
BVH_TEST_SRCS = tests/bvh_tests.c src/bvh.c $(OA_LIB)/src/object_allocator.c
BVH_TEST_FLAGS = -Wall -Wextra -g -I./include -I$(OA_LIB)/include -fsanitize=address,undefined -lm

test: $(TEST_SRCS) $(BVH_TEST_SRCS)
	$(CC) $(TEST_FLAGS) -o spatial_hash_tests $(TEST_SRCS)
	./spatial_hash_tests
	$(CC) $(BVH_TEST_FLAGS) -o bvh_tests $(BVH_TEST_SRCS)
	./bvh_tests

-include $(OBJS:.o=.d)

.PHONY: all clean test
