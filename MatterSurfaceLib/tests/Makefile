# Makefile for MatterSurfaceLib tests

# Detect platform
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

# Default to Linux settings
PLATFORM = PLATFORM_DESKTOP
LIBTYPE = STATIC
RAYLIB_RELEASE_PATH = ../../../Libraries/raylib
RAYLIB_PATH = $(RAYLIB_RELEASE_PATH)/src

# Platform-specific settings
ifeq ($(UNAME_S),Linux)
    PLATFORM = PLATFORM_DESKTOP
    GRAPHICS = GRAPHICS_API_OPENGL_33
endif

ifeq ($(UNAME_S),Darwin)
    PLATFORM = PLATFORM_DESKTOP
    GRAPHICS = GRAPHICS_API_OPENGL_33
    ifeq ($(UNAME_M),arm64)
        # Apple Silicon Mac
        MACOS_VERSION = 11.0
    else
        # Intel Mac
        MACOS_VERSION = 10.15
    endif
endif

# Compiler settings
CC = g++
CFLAGS = -std=c++17 -Wall -Wno-missing-braces -Wno-unused-variable -D$(PLATFORM) -D$(GRAPHICS)

ifeq ($(UNAME_S),Darwin)
    CFLAGS += -mmacosx-version-min=$(MACOS_VERSION)
    LDFLAGS = -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo
else
    LDFLAGS = -lGL -lm -lpthread -ldl -lrt -lX11
endif

# Include directories
INCLUDE_PATHS = -I../include -I$(RAYLIB_PATH) -I../../Libraries/raylib/src

# Library paths
LDLIBS = ../../Libraries/raylib/src/libraylib.a

# Source files
MAIN_SOURCE = minimal_cell_test.cpp
MATTER_SOURCES = 

# Target
TARGET = minimal_cell_test

# Build rules
$(TARGET): $(MAIN_SOURCE) $(MATTER_SOURCES)
	$(CC) $(MAIN_SOURCE) $(MATTER_SOURCES) -o $(TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)

.PHONY: clean run

clean:
	rm -f $(TARGET)

run: $(TARGET)
	./$(TARGET)

# Debug build
debug: CFLAGS += -g -DDEBUG
debug: $(TARGET)