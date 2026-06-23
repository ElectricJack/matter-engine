# Standalone mesh-charting utilities. Header-light, raylib-free.
CXX ?= g++
CXXFLAGS = -Wall -Wextra -O2 -std=c++17 -Iinclude

lib: src/mesh_charting.cpp
	$(CXX) $(CXXFLAGS) -c src/mesh_charting.cpp -o mesh_charting.o

test:
	$(MAKE) -C tests run

clean:
	rm -f mesh_charting.o tests/mesh_charting_tests
