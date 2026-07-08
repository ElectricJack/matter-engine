CC = gcc
RAYLIB_PATH = ../Libraries/raylib
IMGUI_PATH = ../Libraries/imgui
CFLAGS = -Wall -Wextra -O2 -MMD -MP -I$(RAYLIB_PATH)/src -I$(RAYLIB_PATH)/src/external/glfw/include -I$(IMGUI_PATH) -I$(IMGUI_PATH)/backends

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
SRC = main.cpp $(IMGUI_PATH)/imgui.cpp $(IMGUI_PATH)/imgui_demo.cpp $(IMGUI_PATH)/imgui_draw.cpp $(IMGUI_PATH)/imgui_tables.cpp $(IMGUI_PATH)/imgui_widgets.cpp $(IMGUI_PATH)/backends/imgui_impl_opengl3.cpp $(IMGUI_PATH)/backends/imgui_impl_glfw.cpp
# Object files (local names)
OBJ = main.o imgui.o imgui_demo.o imgui_draw.o imgui_tables.o imgui_widgets.o imgui_impl_opengl3.o imgui_impl_glfw.o
BIN = cube_app

all: $(BIN)

$(BIN): $(OBJ) raylib
	g++ -o $@ $(OBJ) $(LDFLAGS) $(LDLIBS)

raylib:
	$(MAKE) -C $(RAYLIB_PATH)/src PLATFORM=PLATFORM_DESKTOP

%.o: %.c
	$(CC) -c $< $(CFLAGS)

main.o: main.cpp
	g++ -c $< $(CFLAGS) -std=c++11

imgui.o: $(IMGUI_PATH)/imgui.cpp
	g++ -c $< $(CFLAGS) -std=c++11 -o $@

imgui_demo.o: $(IMGUI_PATH)/imgui_demo.cpp
	g++ -c $< $(CFLAGS) -std=c++11 -o $@

imgui_draw.o: $(IMGUI_PATH)/imgui_draw.cpp
	g++ -c $< $(CFLAGS) -std=c++11 -o $@

imgui_tables.o: $(IMGUI_PATH)/imgui_tables.cpp
	g++ -c $< $(CFLAGS) -std=c++11 -o $@

imgui_widgets.o: $(IMGUI_PATH)/imgui_widgets.cpp
	g++ -c $< $(CFLAGS) -std=c++11 -o $@

imgui_impl_opengl3.o: $(IMGUI_PATH)/backends/imgui_impl_opengl3.cpp
	g++ -c $< $(CFLAGS) -std=c++11 -o $@

imgui_impl_glfw.o: $(IMGUI_PATH)/backends/imgui_impl_glfw.cpp
	g++ -c $< $(CFLAGS) -std=c++11 -o $@

clean:
	rm -f $(OBJ) $(OBJ:.o=.d) $(BIN)

clean-all: clean
	$(MAKE) -C $(RAYLIB_PATH)/src clean

-include $(OBJ:.o=.d)

.PHONY: all clean clean-all raylib