CC = gcc
CXX = g++
RAYLIB_PATH = ../Libraries/raylib
CFLAGS = -Wall -Wextra -O2 -I$(RAYLIB_PATH)/src -I./include
CXXFLAGS = -Wall -Wextra -O2 -std=c++17 -Wno-missing-field-initializers -I$(RAYLIB_PATH)/src -I./include

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

# C++ main application
SRC = main.cpp src/bvh.c src/object_allocator.c src/blas_manager.cpp src/tlas_manager.cpp
OBJ = main.o bvh.o object_allocator.o blas_manager.o tlas_manager.o
BIN = gpu_raytrace
PREPROCESSOR = shader_preprocessor

all: shaders $(BIN)

shaders: shaders/raytrace_tlas_blas_processed.fs

$(PREPROCESSOR): src/shader_preprocessor.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

shaders/raytrace_tlas_blas_processed.fs: shaders/raytrace_tlas_blas.fs shaders/blas_tlas_common.glsl $(PREPROCESSOR)
	@echo "Processing shader with includes (C++)..."
	./$(PREPROCESSOR) shaders/raytrace_tlas_blas.fs shaders/raytrace_tlas_blas_processed.fs

$(BIN): $(OBJ) raylib
	$(CXX) -o $@ $(OBJ) $(LDFLAGS) $(LDLIBS)

# Build rules for main target (C++)
main.o: main.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

bvh.o: src/bvh.c
	$(CC) -c $< $(CFLAGS) -o $@

object_allocator.o: src/object_allocator.c
	$(CC) -c $< $(CFLAGS) -o $@

blas_manager.o: src/blas_manager.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

tlas_manager.o: src/tlas_manager.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@


raylib:
	$(MAKE) -C $(RAYLIB_PATH)/src PLATFORM=PLATFORM_DESKTOP

%.o: %.c
	$(CC) -c $< $(CFLAGS)

clean:
	rm -f $(OBJ) $(BIN) *.o $(PREPROCESSOR) shaders/raytrace_tlas_blas_processed.fs

clean-all: clean
	$(MAKE) -C $(RAYLIB_PATH)/src clean

.PHONY: all clean clean-all raylib shaders