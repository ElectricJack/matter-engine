# ParticleFlowLib: generic agent-particle kernel + path recording.
# Standalone build for dev/tests; MatterEngine3 compiles src/ directly
# into libmatter_engine3.a (MatterSurfaceLib direct-source pattern).
CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -g -O2 -I./include
OBJ_DIR = obj
SRCS = $(wildcard src/*.cpp)
OBJS = $(SRCS:src/%.cpp=$(OBJ_DIR)/%.o)
HEADERS = $(wildcard include/*.h)

all: libparticleflow.a

libparticleflow.a: $(OBJS)
	ar rcs $@ $^

$(OBJ_DIR)/%.o: src/%.cpp $(HEADERS)
	@mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

TEST_FLAGS = -std=c++17 -Wall -Wextra -g -I./include -fsanitize=address,undefined

test: tests/pf_tests.cpp $(SRCS) $(HEADERS)
	$(CXX) $(TEST_FLAGS) -o pf_tests tests/pf_tests.cpp $(SRCS)
	./pf_tests

clean:
	rm -rf $(OBJ_DIR) libparticleflow.a pf_tests

.PHONY: all test clean
