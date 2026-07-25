CXX ?= g++
CXXFLAGS = -Wall -Wextra -O2 -std=c++17 -I../include

mesh_charting_tests: mesh_charting_tests.cpp ../src/mesh_charting.cpp
	$(CXX) $(CXXFLAGS) $^ -o $@

run: mesh_charting_tests
	./mesh_charting_tests
