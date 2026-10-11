cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(MatterProceduralGeometry VERSION 1.0.0 LANGUAGES C CXX)
    include(CTest)
endif()
include("${CMAKE_CURRENT_LIST_DIR}/../MatterContracts/cmake/ComponentScaffolds.cmake")
# Contract test programs are a separate gate; this module needs only the library.
if(NOT TARGET matter_contracts)
    set(_geometry_testing "${BUILD_TESTING}")
    set(BUILD_TESTING OFF)
    matter_register_component(matter_contracts)
    set(BUILD_TESTING "${_geometry_testing}")
endif()
get_filename_component(MATTER_GEOMETRY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
# Compile the geometry portion of the canonical TU. Its final VariationRecorder
# function depends on the cache/world backend and MSVC resolves that reference
# even with /Gy + /OPT:REF. No algorithm copy is checked in. Reconfigure whenever
# the source changes; a moved seam fails explicitly instead of linking a cache.
set(_emitter "${MATTER_GEOMETRY_ROOT}/MatterEngine3/src/triangle_emit.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_emitter}")
file(READ "${_emitter}" _emitter_source)
string(FIND "${_emitter_source}" "\nuint64_t VariationRecorder::instance(" _cache_seam)
if(_cache_seam LESS 0)
    message(FATAL_ERROR "Canonical triangle emitter/cache seam changed; audit the source split")
endif()
string(SUBSTRING "${_emitter_source}" 0 ${_cache_seam} _geometry_source)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/shared_triangle_emit.cpp"
    "${_geometry_source}\n} // namespace tri_emit\n")
add_library(matter_geometry STATIC src/geometry.cpp src/profile.cpp src/session.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/shared_triangle_emit.cpp"
    "${MATTER_GEOMETRY_ROOT}/MatterEngine3/src/polygon_triangulate.cpp")
target_compile_features(matter_geometry PUBLIC cxx_std_17)
target_link_libraries(matter_geometry PUBLIC matter_contracts)
target_include_directories(matter_geometry PUBLIC include PRIVATE
    "${MATTER_GEOMETRY_ROOT}/MatterEngine3/src"
    "${MATTER_GEOMETRY_ROOT}/MatterEngine3/include"
    "${MATTER_GEOMETRY_ROOT}/libs/SpatialQueryLib/include"
    "${MATTER_GEOMETRY_ROOT}/libs/MatterSurfaceLib/include"
    "${MATTER_GEOMETRY_ROOT}/third_party/raylib/src")
if(MSVC)
    target_compile_options(matter_geometry PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(matter_geometry PRIVATE -Wall -Wextra)
endif()
option(MATTER_GEOMETRY_TESTS "Build geometry fixtures without host/renderer/terrain" ON)
add_executable(matter_geometry_recipe examples/direct_recipe.cpp)
target_link_libraries(matter_geometry_recipe PRIVATE matter_geometry)
if(MATTER_GEOMETRY_TESTS)
    enable_testing()
    add_executable(matter_geometry_tests tests/geometry_tests.cpp)
    target_link_libraries(matter_geometry_tests PRIVATE matter_geometry)
    add_test(NAME matter_geometry_tests COMMAND matter_geometry_tests)
    add_test(NAME matter_geometry_recipe COMMAND matter_geometry_recipe)
    add_executable(matter_geometry_allocation_tests tests/allocation_tests.cpp)
    target_link_libraries(matter_geometry_allocation_tests PRIVATE matter_geometry)
    add_test(NAME matter_geometry_allocation_tests COMMAND matter_geometry_allocation_tests)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/geometry-dependencies-$<CONFIG>.cmake" CONTENT
"set(geometry_links [==[$<TARGET_PROPERTY:matter_geometry,LINK_LIBRARIES>]==])
set(geometry_public_links [==[$<TARGET_PROPERTY:matter_geometry,INTERFACE_LINK_LIBRARIES>]==])
set(geometry_sources [==[$<TARGET_PROPERTY:matter_geometry,SOURCES>]==])
set(geometry_public_includes [==[$<TARGET_PROPERTY:matter_geometry,INTERFACE_INCLUDE_DIRECTORIES>]==])
set(recipe_links [==[$<TARGET_PROPERTY:matter_geometry_recipe,LINK_LIBRARIES>]==])
set(geometry_archive [==[$<TARGET_FILE:matter_geometry>]==])
set(symbol_tool [==[${CMAKE_NM}]==])
set(native_linker [==[${CMAKE_LINKER}]==])
set(msvc [==[${MSVC}]==])
set(module_root [==[${CMAKE_CURRENT_SOURCE_DIR}]==])
set(repo_root [==[${MATTER_GEOMETRY_ROOT}]==])
set(build_root [==[${CMAKE_CURRENT_BINARY_DIR}]==])
")
    add_test(NAME matter_geometry_dependencies COMMAND "${CMAKE_COMMAND}"
        "-DGRAPH=${CMAKE_CURRENT_BINARY_DIR}/geometry-dependencies-$<CONFIG>.cmake"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/dependency_audit.cmake")
endif()
