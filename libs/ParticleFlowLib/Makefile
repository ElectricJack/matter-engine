# ParticleFlowLib: generic agent-particle kernel + path recording.
# Standalone build for dev/tests; MatterEngine3 compiles src/ directly
# into libmatter_engine3.a (MatterSurfaceLib direct-source pattern).
CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -g -O2 -I./include
OBJ_DIR = build/obj
SRCS = $(wildcard src/*.cpp)
OBJS = $(SRCS:src/%.cpp=$(OBJ_DIR)/%.o)
HEADERS = $(wildcard include/*.h)

all: build/libparticleflow.a

build/libparticleflow.a: $(OBJS)
	ar rcs $@ $^

$(OBJ_DIR)/%.o: src/%.cpp $(HEADERS)
	@mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

TEST_FLAGS = -std=c++17 -Wall -Wextra -g -I./include -fsanitize=address,undefined

test: tests/pf_tests.cpp $(SRCS) $(HEADERS) | build
	$(CXX) $(TEST_FLAGS) -o build/pf_tests tests/pf_tests.cpp $(SRCS)
	./build/pf_tests

build:
	@mkdir -p $@

clean:
	rm -rf build

.PHONY: all test clean
