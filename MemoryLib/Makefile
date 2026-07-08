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
	rm -rf $(OBJ_DIR) $(TARGET) memory_tests memory_hpp_tests

# Tests: ASan + UBSan (repo convention)
TEST_FLAGS = -Wall -Wextra -g -I./include -fsanitize=address,undefined
TEST_SRCS = tests/memory_tests.c $(wildcard $(SRC_DIR)/*.c)

test: $(TEST_SRCS)
	$(CC) $(TEST_FLAGS) -o memory_tests $(TEST_SRCS)
	./memory_tests

-include $(OBJS:.o=.d)

.PHONY: all clean test
