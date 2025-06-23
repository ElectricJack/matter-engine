CC = gcc
RAYLIB_PATH = ../Libraries/raylib
CFLAGS = -Wall -Wextra -O2 -I$(RAYLIB_PATH)/src -Iinclude

# Detect OS
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    LDFLAGS = -L$(RAYLIB_PATH)/src -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo -lm
    LDLIBS = $(RAYLIB_PATH)/src/libraylib.a
endif
ifeq ($(UNAME_S),Linux)
    LDFLAGS = -L$(RAYLIB_PATH)/src -lGL -lm -lpthread -ldl -lrt -lX11
    LDLIBS = $(RAYLIB_PATH)/src/libraylib.a
endif
ifeq ($(OS),Windows_NT)
    LDFLAGS = -L$(RAYLIB_PATH)/src -lopengl32 -lgdi32 -lwinmm
    LDLIBS = $(RAYLIB_PATH)/src/libraylib.a
endif

# Source files
SRC_DIR = src
INCLUDE_DIR = include
OBJ_DIR = obj

# Create object directory if it doesn't exist
$(shell mkdir -p $(OBJ_DIR))
$(shell mkdir -p $(OBJ_DIR)/$(SRC_DIR))

# Source files - Include both core files and linked library files
SRCS = main.c $(SRC_DIR)/open_particle_surface.c $(SRC_DIR)/surface.c $(SRC_DIR)/object_allocator.c $(SRC_DIR)/spatial_hash.c
OBJS = $(SRCS:%.c=$(OBJ_DIR)/%.o)

# Target executable
BIN = open_particle_surface

all: $(BIN)

$(BIN): $(OBJS) raylib
	$(CC) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)

raylib:
	$(MAKE) -C $(RAYLIB_PATH)/src PLATFORM=PLATFORM_DESKTOP

# Rule for main.c (in root directory)
$(OBJ_DIR)/main.o: main.c
	$(CC) $(CFLAGS) -c $< -o $@

# Rule for source files in src directory
$(OBJ_DIR)/$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJ_DIR) $(BIN)

clean-all: clean
	$(MAKE) -C $(RAYLIB_PATH)/src clean

.PHONY: all clean clean-all raylib