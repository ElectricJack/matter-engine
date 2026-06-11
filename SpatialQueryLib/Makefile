CC = gcc
CFLAGS = -Wall -Wextra -g -I./include
LDFLAGS =

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
	rm -rf $(OBJ_DIR) $(TARGET)

.PHONY: all clean