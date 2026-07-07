CC = gcc
CFLAGS = -Wall -Wextra -g -I./include
LDFLAGS = -lm

SRC_DIR = src
INCLUDE_DIR = include
OBJ_DIR = obj

# Create object directory if it doesn't exist
$(shell mkdir -p $(OBJ_DIR))

# Source files
SRCS = main.c $(wildcard $(SRC_DIR)/*.c)
OBJS = $(SRCS:%.c=$(OBJ_DIR)/%.o)

# Header files
HEADERS = $(wildcard $(INCLUDE_DIR)/*.h)

# Target executable
TARGET = spatialquerylib

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Rule for main.c (in root directory)
$(OBJ_DIR)/main.o: main.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

# Rule for source files in src directory
$(OBJ_DIR)/$(SRC_DIR)/%.o: $(SRC_DIR)/%.c $(HEADERS)
	mkdir -p $(OBJ_DIR)/$(SRC_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJ_DIR) $(TARGET) spatial_hash_tests

# Test target: build and run spatial hash tests with AddressSanitizer + UBSan
TEST_SRCS = tests/spatial_hash_tests.c src/spatial_hash.c src/object_allocator.c
TEST_FLAGS = -Wall -Wextra -g -I./include -fsanitize=address,undefined -lm

test: $(TEST_SRCS)
	$(CC) $(TEST_FLAGS) -o spatial_hash_tests $(TEST_SRCS)
	./spatial_hash_tests

.PHONY: all clean test