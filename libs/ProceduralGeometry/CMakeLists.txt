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
set(_surface "${MATTER_GEOMETRY_ROOT}/libs/MatterSurfaceLib/src/surface.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_surface}")
file(READ "${_surface}" _surface_source)
set(_surface_math "#pragma once\n#include <cmath>\n#include <matter_math_c.h>\nnamespace matter::geometry::detail {\nusing std::isinf; using std::isfinite;\n")
foreach(_helper smin_set VertexInterpolation)
    if(_helper STREQUAL "smin_set")
        set(_start "static float smin_set(")
        set(_end "// Ordered-CSG staged field evaluation")
    else()
        set(_start "static MtVec3 VertexInterpolation(MtVec3 v1, float val1, MtVec3 v2, float val2, float isovalue) {")
        set(_end "// Enumeration for edge indices in a cube")
    endif()
    string(FIND "${_surface_source}" "${_start}" _begin)
    string(FIND "${_surface_source}" "${_end}" _finish)
    if(_begin LESS 0 OR _finish LESS_EQUAL _begin)
        message(FATAL_ERROR "Canonical surface math seam changed; audit ${_helper}")
    endif()
    math(EXPR _length "${_finish} - ${_begin}")
    string(SUBSTRING "${_surface_source}" ${_begin} ${_length} _body)
    string(PREPEND _body "[[maybe_unused]] ")
    string(APPEND _surface_math "${_body}\n")
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/shared_surface_math.h" "${_surface_math}\n}\n")
add_library(matter_geometry STATIC src/geometry.cpp src/profile.cpp src/session.cpp src/csg.cpp src/csg_mesh.cpp
    src/query.cpp src/modifiers.cpp
    src/triangle_emit.cpp src/polygon_triangulate.cpp
    "${MATTER_GEOMETRY_ROOT}/libs/MatterSurfaceLib/src/fat_primitive.c")
include(cmake/FieldKernel.cmake)
# Simplify/smooth compile the canonical MatterSurfaceLib stages. OFF drops them
# and stops advertising their capabilities; modifier requests then fail as
# unsupported_capability. The optional isolated worker links its own backend.
option(MATTER_GEOMETRY_MODIFIERS "Compile the CPU mesh simplify/smooth modifier backend" ON)
if(MATTER_GEOMETRY_MODIFIERS)
    set(_modifier_sources)
    foreach(_stage mesh_indexed mesh_simplifier mesh_smooth mesh_transform)
        list(APPEND _modifier_sources "${MATTER_GEOMETRY_ROOT}/libs/MatterSurfaceLib/src/${_stage}.cpp")
    endforeach()
    # Allocate through raylib's malloc macros, not its windowing MemAlloc.
    set_source_files_properties(${_modifier_sources} PROPERTIES COMPILE_DEFINITIONS MATTER_VULKAN_ONLY)
    target_sources(matter_geometry PRIVATE ${_modifier_sources})
    target_compile_definitions(matter_geometry PRIVATE MATTER_GEOMETRY_HAVE_MODIFIERS=1)
endif()
option(MATTER_GEOMETRY_MESH_WORKER "Build optional subprocess mesh backend (native Linux)" OFF)
if(MATTER_GEOMETRY_MESH_WORKER)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT MATTER_GEOMETRY_MODIFIERS OR CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR "Mesh worker requires a native Linux build with MATTER_GEOMETRY_MODIFIERS=ON")
    endif()
    include(cmake/MeshWorker.cmake)
endif()
target_compile_features(matter_geometry PUBLIC cxx_std_17)
target_link_libraries(matter_geometry PUBLIC matter_contracts)
target_include_directories(matter_geometry PUBLIC include PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${MATTER_GEOMETRY_ROOT}/MatterEngine3/include"
    "${MATTER_GEOMETRY_ROOT}/libs/SpatialQueryLib/include"
    "${MATTER_GEOMETRY_ROOT}/libs/MatterSurfaceLib/include"
    "${MATTER_GEOMETRY_ROOT}/libs/MatterSurfaceLib/src"
    "${MATTER_GEOMETRY_ROOT}/third_party/raylib/src")
if(MSVC)
    target_compile_options(matter_geometry PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(matter_geometry PRIVATE -Wall -Wextra)
endif()
option(MATTER_GEOMETRY_TESTS "Build geometry fixtures without host/renderer/terrain" ON)
option(MATTER_GEOMETRY_FUZZING "Build the long-running standalone geometry property harness" OFF)
option(MATTER_GEOMETRY_LIBFUZZER "Build the Clang libFuzzer geometry harness with ASan/UBSan" OFF)
if(MATTER_GEOMETRY_LIBFUZZER AND NOT MATTER_GEOMETRY_FUZZING)
    message(FATAL_ERROR "MATTER_GEOMETRY_LIBFUZZER requires MATTER_GEOMETRY_FUZZING=ON")
endif()
function(matter_geometry_property_target target entry)
    add_executable(${target} ${entry} tests/geometry_properties.cpp)
    target_link_libraries(${target} PRIVATE matter_geometry)
    target_compile_definitions(${target} PRIVATE
        MATTER_GEOMETRY_EXPECT_MODIFIERS=$<BOOL:${MATTER_GEOMETRY_MODIFIERS}>)
    if(MATTER_GEOMETRY_LIBFUZZER)
        target_compile_options(${target} PRIVATE -fsanitize=fuzzer-no-link,address,undefined
            -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
    endif()
    target_include_directories(${target} PRIVATE
        "${MATTER_GEOMETRY_ROOT}/MatterEngine3/include"
        "${MATTER_GEOMETRY_ROOT}/libs/MatterSurfaceLib/include"
        "${MATTER_GEOMETRY_ROOT}/libs/SpatialQueryLib/include"
        "${MATTER_GEOMETRY_ROOT}/third_party/raylib/src")
endfunction()
if(MATTER_GEOMETRY_FUZZING)
    matter_geometry_property_target(matter_geometry_fuzz tests/geometry_property_tests.cpp)
    target_compile_definitions(matter_geometry_fuzz PRIVATE MATTER_GEOMETRY_STANDALONE_FUZZ=1)
    if(MATTER_GEOMETRY_LIBFUZZER)
        if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR MSVC)
            message(FATAL_ERROR "The libFuzzer harness requires the Clang GNU frontend")
        endif()
        # Instrument the algorithms and contract validators, not just the input
        # adapter. Propagate sanitizer runtimes to all consumers in this build.
        foreach(target matter_geometry matter_contracts)
            target_compile_options(${target} PRIVATE -fsanitize=fuzzer-no-link,address,undefined
                -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
            target_link_options(${target} INTERFACE -fsanitize=address,undefined)
        endforeach()
        matter_geometry_property_target(matter_geometry_libfuzzer tests/geometry_fuzzer.cpp)
        target_compile_options(matter_geometry_libfuzzer PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
        target_link_options(matter_geometry_libfuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
    endif()
endif()
add_executable(matter_geometry_recipe examples/direct_recipe.cpp)
target_link_libraries(matter_geometry_recipe PRIVATE matter_geometry)
if(MATTER_GEOMETRY_TESTS)
    enable_testing()
    if(MATTER_GEOMETRY_MESH_WORKER)
        add_executable(matter_geometry_mesh_worker_fake tests/mesh_worker_fake.cpp)
        target_include_directories(matter_geometry_mesh_worker_fake PRIVATE src
            "${MATTER_GEOMETRY_ROOT}/MatterEngine3/include"
            "${MATTER_GEOMETRY_ROOT}/libs/MatterSurfaceLib/include"
            "${MATTER_GEOMETRY_ROOT}/libs/SpatialQueryLib/include"
            "${MATTER_GEOMETRY_ROOT}/libs/MathLib/include"
            "${MATTER_GEOMETRY_ROOT}/third_party/raylib/src")
        target_compile_features(matter_geometry_mesh_worker_fake PRIVATE cxx_std_17)
        add_executable(matter_geometry_mesh_worker_tests tests/mesh_worker_tests.cpp)
        add_dependencies(matter_geometry_mesh_worker_tests matter_geometry_mesh_worker_fake)
        target_link_libraries(matter_geometry_mesh_worker_tests PRIVATE matter_geometry Threads::Threads)
        add_test(NAME matter_geometry_mesh_worker_tests COMMAND matter_geometry_mesh_worker_tests
            $<TARGET_FILE:matter_geometry_mesh_worker_fake>)
        add_executable(matter_geometry_mesh_worker_parity tests/mesh_worker_parity.cpp)
        add_dependencies(matter_geometry_mesh_worker_parity matter_geometry_mesh_worker)
        target_link_libraries(matter_geometry_mesh_worker_parity PRIVATE matter_geometry matter_geometry_worker_backend)
        add_test(NAME matter_geometry_mesh_worker_parity COMMAND matter_geometry_mesh_worker_parity
            $<TARGET_FILE:matter_geometry_mesh_worker>)
        set_tests_properties(matter_geometry_mesh_worker_tests PROPERTIES TIMEOUT 20)
        set_tests_properties(matter_geometry_mesh_worker_parity PROPERTIES TIMEOUT 120)
    endif()
    matter_geometry_property_target(matter_geometry_property_tests tests/geometry_property_tests.cpp)
    set(_property_groups primitives profiles csg queries)
    if(MATTER_GEOMETRY_MODIFIERS)
        list(APPEND _property_groups modifiers_lod)
    endif()
    set(_property_group 0)
    foreach(group IN LISTS _property_groups)
        add_test(NAME matter_geometry_properties_${group} COMMAND matter_geometry_property_tests ${_property_group})
        set_tests_properties(matter_geometry_properties_${group} PROPERTIES TIMEOUT 5 LABELS "cpu;property")
        math(EXPR _property_group "${_property_group} + 1")
    endforeach()
    # Production baseline is test-only. The shipped CPU target never links the
    # unbounded surface traversal, spatial hash or its scratch allocator.
    add_executable(matter_geometry_csg_tests tests/csg_tests.cpp
        "${MATTER_GEOMETRY_ROOT}/libs/MatterSurfaceLib/src/surface.c"
        "${MATTER_GEOMETRY_ROOT}/libs/SpatialQueryLib/src/spatial_hash.c"
        "${MATTER_GEOMETRY_ROOT}/libs/MemoryLib/src/mem_pool.c")
    target_link_libraries(matter_geometry_csg_tests PRIVATE matter_geometry)
    target_include_directories(matter_geometry_csg_tests PRIVATE src
        "${MATTER_GEOMETRY_ROOT}/MatterEngine3/src"
        "${MATTER_GEOMETRY_ROOT}/MatterEngine3/include"
        "${MATTER_GEOMETRY_ROOT}/libs/MatterSurfaceLib/include"
        "${MATTER_GEOMETRY_ROOT}/libs/SpatialQueryLib/include"
        "${MATTER_GEOMETRY_ROOT}/libs/MemoryLib/include"
        "${MATTER_GEOMETRY_ROOT}/third_party/raylib/src")
    add_test(NAME matter_geometry_csg_tests COMMAND matter_geometry_csg_tests)
    add_executable(matter_geometry_tests tests/geometry_tests.cpp)
    target_link_libraries(matter_geometry_tests PRIVATE matter_geometry)
    add_test(NAME matter_geometry_tests COMMAND matter_geometry_tests)
    add_test(NAME matter_geometry_recipe COMMAND matter_geometry_recipe)
    add_executable(matter_geometry_allocation_tests tests/allocation_tests.cpp)
    target_link_libraries(matter_geometry_allocation_tests PRIVATE matter_geometry)
    add_test(NAME matter_geometry_allocation_tests COMMAND matter_geometry_allocation_tests)
    add_test(NAME matter_geometry_csg_allocation_tests COMMAND matter_geometry_allocation_tests --csg)
    add_test(NAME matter_geometry_query_allocation_tests COMMAND matter_geometry_allocation_tests --query)
    if(MATTER_GEOMETRY_MODIFIERS)
        add_test(NAME matter_geometry_process_allocation_tests COMMAND matter_geometry_allocation_tests --process)
    endif()
    add_executable(matter_geometry_processing_tests tests/processing_tests.cpp)
    target_link_libraries(matter_geometry_processing_tests PRIVATE matter_geometry)
    target_compile_definitions(matter_geometry_processing_tests PRIVATE
        MATTER_GEOMETRY_EXPECT_MODIFIERS=$<BOOL:${MATTER_GEOMETRY_MODIFIERS}>)
    add_test(NAME matter_geometry_processing_tests COMMAND matter_geometry_processing_tests)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/geometry-dependencies-$<CONFIG>.cmake" CONTENT
"set(geometry_links [==[$<TARGET_PROPERTY:matter_geometry,LINK_LIBRARIES>]==])
set(geometry_public_links [==[$<TARGET_PROPERTY:matter_geometry,INTERFACE_LINK_LIBRARIES>]==])
set(geometry_sources [==[$<TARGET_PROPERTY:matter_geometry,SOURCES>]==])
set(geometry_public_includes [==[$<TARGET_PROPERTY:matter_geometry,INTERFACE_INCLUDE_DIRECTORIES>]==])
set(recipe_links [==[$<TARGET_PROPERTY:matter_geometry_recipe,LINK_LIBRARIES>]==])
set(field_links [==[$<TARGET_PROPERTY:matter_geometry_field,LINK_LIBRARIES>]==])
set(field_sources [==[$<TARGET_PROPERTY:matter_geometry_field,SOURCES>]==])
set(geometry_archive [==[$<TARGET_FILE:matter_geometry>]==])
set(symbol_tool [==[${CMAKE_NM}]==])
set(native_linker [==[${CMAKE_LINKER}]==])
set(msvc [==[${MSVC}]==])
set(module_root [==[${CMAKE_CURRENT_SOURCE_DIR}]==])
set(repo_root [==[${MATTER_GEOMETRY_ROOT}]==])
set(build_root [==[${CMAKE_CURRENT_BINARY_DIR}]==])
set(modifiers [==[${MATTER_GEOMETRY_MODIFIERS}]==])
set(mesh_worker [==[${MATTER_GEOMETRY_MESH_WORKER}]==])
")
    add_test(NAME matter_geometry_dependencies COMMAND "${CMAKE_COMMAND}"
        "-DGRAPH=${CMAKE_CURRENT_BINARY_DIR}/geometry-dependencies-$<CONFIG>.cmake"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/dependency_audit.cmake")
endif()
