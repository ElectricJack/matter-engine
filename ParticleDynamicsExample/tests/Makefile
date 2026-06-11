CXX = g++
CXXFLAGS = -Wall -Wextra -O0 -g -std=c++17 -I../include -I../../Libraries/raylib/src -I../../SpatialQueryLib/include

# Source files
SOURCES = basic_test.cpp \
          ../src/particle_system.cpp \
          ../src/material_manager.cpp \
          ../src/gamified_physics.cpp \
          ../../SpatialQueryLib/src/spatial_hash.c \
          ../../SpatialQueryLib/src/object_allocator.c

# Object files
OBJECTS = $(SOURCES:.cpp=.o)
OBJECTS := $(OBJECTS:.c=.o)

# Target executable
TARGET = basic_test

# Build target
$(TARGET): $(OBJECTS)
	$(CXX) $(OBJECTS) -o $(TARGET) -lm

# Compile rules
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

%.o: %.c
	gcc -Wall -Wextra -O0 -g -I../../SpatialQueryLib/include -c $< -o $@

# Clean
clean:
	rm -f $(OBJECTS) $(TARGET)

# Run tests
test: $(TARGET)
	./$(TARGET)

.PHONY: clean test 