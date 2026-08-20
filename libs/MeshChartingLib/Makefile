# Standalone mesh-charting utilities. Header-light, raylib-free.

# Shared build config (TMP/TEMP, GLSLC, parallelism, ccache detection).
include ../../platform.mk

CXX ?= g++
CXXFLAGS = -Wall -Wextra -O2 -std=c++17 -Iinclude

lib: src/mesh_charting.cpp
	@mkdir -p build/obj
	$(CXX) $(CXXFLAGS) -MMD -MP -c src/mesh_charting.cpp -o build/obj/mesh_charting.o

test:
	$(MAKE) -C tests run

clean:
	rm -rf build
	$(MAKE) -C tests clean

-include build/obj/mesh_charting.d
