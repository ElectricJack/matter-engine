# Shared build config (TMP/TEMP, GLSLC, parallelism, ccache detection).
include ../../../platform.mk

CXX ?= g++
CXXFLAGS = -Wall -Wextra -O2 -std=c++17 -I../include

build:
	@mkdir -p $@

build/mesh_charting_tests: mesh_charting_tests.cpp ../src/mesh_charting.cpp ../src/chart_packing_order.h ../include/mesh_charting.h | build
	$(CXX) $(CXXFLAGS) mesh_charting_tests.cpp ../src/mesh_charting.cpp -o $@

run: build/mesh_charting_tests
	./build/mesh_charting_tests

clean:
	rm -rf build

.PHONY: run clean
