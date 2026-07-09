# ExplorerDemo — standalone demo app that consumes only the MatterEngine3 public API.
#
# Compile: main.cpp + camera_rig.cpp
# Link:    libmatter_engine3.a + raylib + system GL libs
# Engine include path: -I../MatterEngine3/include ONLY (no engine internals)
#
# Run from ExplorerDemo/ directory so that cache/ resolves correctly.

CC = g++
CFLAGS = -std=c++17 -O2 -Wall -Wno-missing-braces -Wno-unused-variable \
         -DPLATFORM_DESKTOP -DGRAPHICS_API_OPENGL_43

RAYLIB_PATH = ../Libraries/raylib/src
ME3_DIR     = ../MatterEngine3

INCLUDE_PATHS = -I$(ME3_DIR)/include \
                -I$(RAYLIB_PATH)

BOX3D_DIR = ../Libraries/box3d

LDFLAGS = -lGL -lm -lpthread -ldl -lrt -lX11
LDLIBS  = ../Libraries/raylib/build/linux/libraylib.a \
          $(BOX3D_DIR)/libbox3d.a

BUILD_DIR = build/linux

APP_SRC = main.cpp camera_rig.cpp
CPP_NAMES = $(notdir $(APP_SRC))

$(BUILD_DIR):
	mkdir -p $@

CPP_OBJ = $(addprefix $(BUILD_DIR)/,$(CPP_NAMES:.cpp=.o))

$(CPP_OBJ): $(BUILD_DIR)/%.o: %.cpp | $(BUILD_DIR)
	$(CC) -c $< -o $@ $(CFLAGS) $(INCLUDE_PATHS) -MMD -MP

.PHONY: build-engine-lib
build-engine-lib:
	$(MAKE) -C $(ME3_DIR)

.PHONY: shaders shaders_gpu_link
shaders:
	@[ -e shaders ] || ln -s ../MatterSurfaceLib/shaders shaders

shaders_gpu_link:
	@[ -e shaders_gpu ] || ln -s $(ME3_DIR)/shaders_gpu shaders_gpu

.DEFAULT_GOAL := explorer

explorer: build-engine-lib shaders shaders_gpu_link $(CPP_OBJ)
	$(CC) $(CPP_OBJ) $(ME3_DIR)/libmatter_engine3.a -o explorer \
	      $(CFLAGS) $(LDFLAGS) $(LDLIBS)

-include $(CPP_OBJ:.o=.d)

clean:
	rm -rf explorer build shaders shaders_gpu

.PHONY: explorer clean
