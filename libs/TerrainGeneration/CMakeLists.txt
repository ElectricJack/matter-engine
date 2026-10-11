cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(MatterTerrain LANGUAGES C CXX)
    include(CTest)
endif()
include("${CMAKE_CURRENT_LIST_DIR}/../MatterContracts/cmake/ComponentScaffolds.cmake")
function(matter_terrain_register_contracts)
    # This module runs its own focused tests, independently of contract tooling.
    set(BUILD_TESTING OFF)
    matter_register_component(matter_contracts)
endfunction()
matter_terrain_register_contracts()
set(terrain_legacy "${CMAKE_CURRENT_LIST_DIR}/../../MatterEngine3")
# Compile the source of truth directly until ordered integration relocates it.
# The overlay dependency is a neutral virtual CPU interface; no concrete
# hydrology backend is linked into the production field library.
add_library(matter_terrain STATIC src/field.cpp "${terrain_legacy}/src/terrain_field.cpp")
target_compile_features(matter_terrain PUBLIC cxx_std_17)
target_link_libraries(matter_terrain PUBLIC matter_contracts)
target_include_directories(matter_terrain PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include"
    PRIVATE "${terrain_legacy}/src" "${terrain_legacy}/include")
if(MSVC)
    target_compile_options(matter_terrain PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(matter_terrain PRIVATE -Wall -Wextra -Wpedantic)
endif()
set_property(TARGET matter_terrain PROPERTY MATTER_COMPONENT_SCAFFOLD FALSE)
option(MATTER_TERRAIN_BUILD_TESTS "Build independent field and region fixtures" ${BUILD_TESTING})
if(MATTER_TERRAIN_BUILD_TESTS)
    enable_testing()
    add_executable(matter_terrain_region_tests tests/region_tests.cpp)
    target_link_libraries(matter_terrain_region_tests PRIVATE matter_terrain)
    add_test(NAME terrain.region_inputs.v1 COMMAND matter_terrain_region_tests)
    add_executable(matter_terrain_legacy_field_tests
        "${terrain_legacy}/tests/terrain_field_tests.cpp"
        "${terrain_legacy}/src/terrain_river_overlay.cpp"
        "${terrain_legacy}/src/hydrology/river_geometry.cpp")
    target_include_directories(matter_terrain_legacy_field_tests PRIVATE "${terrain_legacy}/include")
    target_link_libraries(matter_terrain_legacy_field_tests PRIVATE matter_terrain)
    add_test(NAME terrain.legacy_field_noise COMMAND matter_terrain_legacy_field_tests)
    add_executable(matter_terrain_legacy_surface_tests "${terrain_legacy}/tests/surface_field_tests.cpp")
    target_link_libraries(matter_terrain_legacy_surface_tests PRIVATE matter_terrain)
    add_test(NAME terrain.legacy_surface_noise COMMAND matter_terrain_legacy_surface_tests)
endif()
