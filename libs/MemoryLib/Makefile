CC = gcc
CFLAGS = -Wall -Wextra -g -I./include
LDFLAGS =

SRC_DIR = src
INCLUDE_DIR = include
OBJ_DIR = obj

$(shell mkdir -p $(OBJ_DIR))

SRCS = main.c $(wildcard $(SRC_DIR)/*.c)
OBJS = $(SRCS:%.c=$(OBJ_DIR)/%.o)
HEADERS = $(wildcard $(INCLUDE_DIR)/*.h)

TARGET = memorylib

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJ_DIR)/main.o: main.c $(HEADERS)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(OBJ_DIR)/$(SRC_DIR)/%.o: $(SRC_DIR)/%.c $(HEADERS)
	mkdir -p $(OBJ_DIR)/$(SRC_DIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf $(OBJ_DIR) $(TARGET) objectallocator memory_tests memory_hpp_tests

# Tests: ASan + UBSan (repo convention)
TEST_FLAGS = -Wall -Wextra -g -I./include -fsanitize=address,undefined
TEST_CC_OBJS = $(OBJ_DIR)/t_mem_pool.o $(OBJ_DIR)/t_mem_arena.o $(OBJ_DIR)/t_mem_array.o

$(OBJ_DIR)/t_%.o: $(SRC_DIR)/%.c $(HEADERS)
	$(CC) $(TEST_FLAGS) -c $< -o $@

test: $(TEST_CC_OBJS) tests/memory_tests.c tests/memory_hpp_tests.cpp
	$(CC) $(TEST_FLAGS) -o memory_tests tests/memory_tests.c $(TEST_CC_OBJS)
	./memory_tests
	g++ -Wall -Wextra -g -std=c++14 -I./include -fsanitize=address,undefined -o memory_hpp_tests tests/memory_hpp_tests.cpp $(TEST_CC_OBJS)
	./memory_hpp_tests

-include $(OBJS:.o=.d)

.PHONY: all clean test
