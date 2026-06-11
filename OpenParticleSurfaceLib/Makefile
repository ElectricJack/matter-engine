CC = gcc
RAYLIB_PATH = ../Libraries/raylib
CFLAGS = -Wall -Wextra -O2 -I$(RAYLIB_PATH)/src -Iinclude

# Detect OS and set platform-specific variables
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    PLATFORM = macos
    PLATFORM_DEFINE = PLATFORM_DESKTOP
    LDFLAGS = -L$(RAYLIB_PATH)/build/$(PLATFORM) -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo -lm
    LDLIBS = $(RAYLIB_PATH)/build/$(PLATFORM)/libraylib.a
endif
ifeq ($(UNAME_S),Linux)
    PLATFORM = linux
    PLATFORM_DEFINE = PLATFORM_DESKTOP
    LDFLAGS = -L$(RAYLIB_PATH)/build/$(PLATFORM) -lGL -lm -lpthread -ldl -lrt -lX11
    LDLIBS = $(RAYLIB_PATH)/build/$(PLATFORM)/libraylib.a
endif
ifeq ($(OS),Windows_NT)
    PLATFORM = windows
    PLATFORM_DEFINE = PLATFORM_DESKTOP
    LDFLAGS = -L$(RAYLIB_PATH)/build/$(PLATFORM) -lopengl32 -lgdi32 -lwinmm
    LDLIBS = $(RAYLIB_PATH)/build/$(PLATFORM)/libraylib.a
endif

# Platform-specific directories
SRC_DIR = src
INCLUDE_DIR = include
OBJ_DIR = build/$(PLATFORM)/obj
BUILD_DIR = build/$(PLATFORM)

# Create platform-specific directories
$(shell mkdir -p $(OBJ_DIR))
$(shell mkdir -p $(OBJ_DIR)/$(SRC_DIR))
$(shell mkdir -p $(BUILD_DIR))
$(shell mkdir -p $(RAYLIB_PATH)/build/$(PLATFORM))

# Source files - Include both core files and linked library files
SRCS = main.c $(SRC_DIR)/open_particle_surface.c $(SRC_DIR)/surface.c $(SRC_DIR)/object_allocator.c $(SRC_DIR)/spatial_hash.c
OBJS = $(SRCS:%.c=$(OBJ_DIR)/%.o)

# Target executable (platform-specific)
BIN = $(BUILD_DIR)/open_particle_surface

all: $(BIN)

$(BIN): $(OBJS) raylib
	$(CC) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)
	@echo "Built executable for $(PLATFORM): $@"
	@echo "Copying executable to root directory for easy execution..."
ifeq ($(PLATFORM),windows)
	@rm -f ./open_particle_surface.exe
	@cp $@ ./open_particle_surface.exe
	@echo "✓ Copied to ./open_particle_surface.exe"
else
	@rm -f ./open_particle_surface
	@cp $@ ./open_particle_surface
	@echo "✓ Copied to ./open_particle_surface"
endif

# Platform-specific raylib build with force rebuild check
RAYLIB_LIB = $(RAYLIB_PATH)/build/$(PLATFORM)/libraylib.a
RAYLIB_FORCE_FILE = $(RAYLIB_PATH)/build/$(PLATFORM)/.raylib_$(PLATFORM)

raylib: $(RAYLIB_LIB)

$(RAYLIB_LIB): $(RAYLIB_FORCE_FILE)
	@echo "Building raylib for $(PLATFORM)..."
	@mkdir -p $(RAYLIB_PATH)/build/$(PLATFORM)
	$(MAKE) -C $(RAYLIB_PATH)/src PLATFORM=$(PLATFORM_DEFINE) clean
	$(MAKE) -C $(RAYLIB_PATH)/src PLATFORM=$(PLATFORM_DEFINE)
	@cp $(RAYLIB_PATH)/src/libraylib.a $(RAYLIB_LIB)
	@echo "Raylib built and copied to $(RAYLIB_LIB)"

$(RAYLIB_FORCE_FILE):
	@echo "Creating platform marker for $(PLATFORM)..."
	@mkdir -p $(RAYLIB_PATH)/build/$(PLATFORM)
	@rm -f $(RAYLIB_PATH)/build/*/.raylib_*
	@touch $(RAYLIB_FORCE_FILE)

# Rule for main.c (in root directory)
$(OBJ_DIR)/main.o: main.c
	$(CC) $(CFLAGS) -c $< -o $@

# Rule for source files in src directory
$(OBJ_DIR)/$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# Platform-specific clean
clean:
	rm -rf $(BUILD_DIR)
ifeq ($(PLATFORM),windows)
	-rm -f ./open_particle_surface.exe
else
	-rm -f ./open_particle_surface
endif

# Clean all platforms
clean-all:
	rm -rf build/
	rm -rf $(RAYLIB_PATH)/build/
	-rm -f ./open_particle_surface ./open_particle_surface.exe

# Force rebuild raylib for current platform
rebuild-raylib:
	rm -f $(RAYLIB_FORCE_FILE)
	$(MAKE) raylib

# Show current platform
platform:
	@echo "Current platform: $(PLATFORM)"
	@echo "Build directory: $(BUILD_DIR)"
	@echo "Raylib library: $(RAYLIB_LIB)"

.PHONY: all clean clean-all raylib rebuild-raylib platform