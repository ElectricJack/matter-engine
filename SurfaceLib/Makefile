CC = gcc
RAYLIB_PATH = ../Libraries/raylib
SPATIAL_QUERY_PATH = ../SpatialQueryLib
OA_LIB = ../ObjectAllocatorLib
CFLAGS = -Wall -Wextra -O2 -I$(RAYLIB_PATH)/src -Iinclude -I$(SPATIAL_QUERY_PATH)/include -I$(OA_LIB)/include

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

# Source files — sibling library sources compiled directly from source of truth
SRC = main.c src/surface.c tests.c
OBJ = main.o src/surface.o tests.o object_allocator.o spatial_hash.o
BIN = surface_app

all: $(BIN)

$(BIN): $(OBJ) raylib
	$(CC) -o $@ $(OBJ) $(LDFLAGS) $(LDLIBS)

raylib:
	$(MAKE) -C $(RAYLIB_PATH)/src PLATFORM=PLATFORM_DESKTOP

%.o: %.c
	$(CC) -c $< $(CFLAGS) -o $@

# Sibling library sources compiled directly from source of truth
object_allocator.o: $(OA_LIB)/src/object_allocator.c $(OA_LIB)/include/object_allocator.h
	$(CC) -c $< $(CFLAGS) -o $@

spatial_hash.o: $(SPATIAL_QUERY_PATH)/src/spatial_hash.c $(SPATIAL_QUERY_PATH)/include/spatial_hash.h
	$(CC) -c $< $(CFLAGS) -o $@

clean:
	rm -f $(OBJ) $(BIN)

clean-all: clean
	$(MAKE) -C $(RAYLIB_PATH)/src clean

.PHONY: all clean clean-all raylib