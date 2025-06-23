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
SRC = main.c src/surface.c src/spatial_hash.c src/object_allocator.c tests.c
OBJ = $(SRC:.c=.o)
BIN = surface_app

all: $(BIN)

$(BIN): $(OBJ) raylib
	$(CC) -o $@ $(OBJ) $(LDFLAGS) $(LDLIBS)

raylib:
	$(MAKE) -C $(RAYLIB_PATH)/src PLATFORM=PLATFORM_DESKTOP

%.o: %.c
	$(CC) -c $< $(CFLAGS) -o $@

clean:
	rm -f $(OBJ) $(BIN)

clean-all: clean
	$(MAKE) -C $(RAYLIB_PATH)/src clean

.PHONY: all clean clean-all raylib