cmake_minimum_required(VERSION 3.25)
project(MatterParityHarness LANGUAGES C CXX)
# Native half of the native/browser parity and result-lifetime harness. It
# compiles the production C ABI from its source of truth; run_parity.py drives
# the resulting step runner alongside the WASM module and the browser runtime.
# The frozen job budget allows 5 s of execution; an unoptimised build spends
# longer than that on the ordered-CSG corpus case, so a budget verdict would
# depend on the build type. Default single-config builds to Release.
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
get_filename_component(MATTER_REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(MATTER_GEOMETRY_C_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${MATTER_REPO}/bindings/geometry-c" geometry-c)
add_executable(matter_parity_native native_runner.cpp)
target_compile_features(matter_parity_native PRIVATE cxx_std_17)
target_link_libraries(matter_parity_native PRIVATE matter_geometry_c)
if(MSVC)
    target_compile_options(matter_parity_native PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(matter_parity_native PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
