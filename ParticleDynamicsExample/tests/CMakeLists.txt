cmake_minimum_required(VERSION 3.10)
project(ParticleDynamicsTests)

# Find required packages
find_package(GTest REQUIRED)
find_package(PkgConfig REQUIRED)

# Include directories
include_directories(../include)
include_directories(../../Libraries/raylib/src)
include_directories(../../SpatialQueryLib/include)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add source files
set(SOURCES
    test_gamified_physics.cpp
    ../src/particle_system.cpp
    ../src/material_manager.cpp
    ../src/gamified_physics.cpp
    ../../SpatialQueryLib/src/spatial_hash.c
    ../../MemoryLib/src/mem_pool.c
)

# Create test executable
add_executable(gamified_physics_tests ${SOURCES})

# Link libraries
target_link_libraries(gamified_physics_tests 
    ${GTEST_LIBRARIES} 
    ${GTEST_MAIN_LIBRARIES}
    pthread
    ../../Libraries/raylib/src/libraylib.a
    GL
    m
    pthread
    dl
    rt
    X11
)

# Add test
enable_testing()
add_test(NAME gamified_physics_tests COMMAND gamified_physics_tests) 