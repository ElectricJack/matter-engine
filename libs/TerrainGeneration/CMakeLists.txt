cmake_minimum_required(VERSION 3.25)
set(terrain_top_level OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(terrain_top_level ON)
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
cmake_path(NORMAL_PATH terrain_legacy)
# Compile the source of truth directly until ordered integration relocates it.
# The overlay dependency is a neutral virtual CPU interface; no concrete
# hydrology backend is linked into the production field library.
add_library(matter_terrain STATIC src/field.cpp src/meshing.cpp
    "${terrain_legacy}/src/terrain_field.cpp" "${terrain_legacy}/src/terrain_mesher.cpp")
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
function(matter_terrain_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()
# Headless tool and independent consumer; neither needs a renderer or GPU.
# The consumer links the neutral contracts only, never matter_terrain. They
# default on only in a standalone build: a parent project linking
# matter_terrain gets the library alone unless it opts in.
option(MATTER_TERRAIN_BUILD_EXAMPLES "Build the headless terrain tool and its product consumer"
    ${terrain_top_level})
if(MATTER_TERRAIN_BUILD_EXAMPLES)
    add_executable(matter_terrain_example_tool tests/examples/terrain_headless_tool.cpp)
    target_link_libraries(matter_terrain_example_tool PRIVATE matter_terrain)
    add_executable(matter_terrain_example_consumer tests/examples/terrain_product_consumer.cpp)
    target_link_libraries(matter_terrain_example_consumer PRIVATE matter_contracts)
    matter_terrain_warnings(matter_terrain_example_tool)
    matter_terrain_warnings(matter_terrain_example_consumer)
endif()
option(MATTER_TERRAIN_BUILD_TESTS "Build independent field, region and meshing fixtures" ${BUILD_TESTING})
if(MATTER_TERRAIN_BUILD_TESTS)
    enable_testing()
    add_executable(matter_terrain_region_tests tests/region_tests.cpp)
    target_link_libraries(matter_terrain_region_tests PRIVATE matter_terrain)
    add_test(NAME terrain.region_inputs.v1 COMMAND matter_terrain_region_tests)
    add_executable(matter_terrain_meshing_tests tests/meshing_tests.cpp)
    target_link_libraries(matter_terrain_meshing_tests PRIVATE matter_terrain)
    add_test(NAME terrain.mesh_products.v1 COMMAND matter_terrain_meshing_tests)
    add_test(NAME terrain.mesh_rollback_rejected.v1 COMMAND matter_terrain_meshing_tests rollback)
    set_tests_properties(terrain.mesh_rollback_rejected.v1 PROPERTIES ENVIRONMENT "MATTER_CONTOUR_SEAMS=0")
    add_executable(matter_terrain_native_parity_tests tests/native_parity_tests.cpp)
    target_include_directories(matter_terrain_native_parity_tests PRIVATE "${terrain_legacy}/include")
    target_link_libraries(matter_terrain_native_parity_tests PRIVATE matter_terrain)
    add_test(NAME terrain.mesh_native_parity.v1 COMMAND matter_terrain_native_parity_tests)
    set_tests_properties(terrain.mesh_products.v1 terrain.mesh_native_parity.v1
        PROPERTIES ENVIRONMENT "MATTER_CONTOUR_SEAMS=1")
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/terrain-graph.cmake" CONTENT
"set(terrain_links [==[$<TARGET_PROPERTY:matter_terrain,LINK_LIBRARIES>]==])
set(terrain_public_links [==[$<TARGET_PROPERTY:matter_terrain,INTERFACE_LINK_LIBRARIES>]==])
set(terrain_sources [==[$<TARGET_PROPERTY:matter_terrain,SOURCES>]==])
set(module_root [==[${CMAKE_CURRENT_SOURCE_DIR}]==])
set(repo_root [==[${CMAKE_CURRENT_SOURCE_DIR}/../..]==])
cmake_path(NORMAL_PATH repo_root)
set(archive [==[$<TARGET_FILE:matter_terrain>]==])
set(msvc ${MSVC})
set(symbol_tool [==[$<IF:$<BOOL:${MSVC}>,${CMAKE_LINKER},${CMAKE_NM}>]==])
")
    add_test(NAME terrain.dependencies.v1 COMMAND "${CMAKE_COMMAND}"
        "-DGRAPH=${CMAKE_CURRENT_BINARY_DIR}/terrain-graph.cmake"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/dependency_audit.cmake")
    foreach(fixture terrain_mesher contour_engine)
        add_executable(matter_terrain_${fixture}_tests "${terrain_legacy}/tests/${fixture}_tests.cpp")
        target_include_directories(matter_terrain_${fixture}_tests PRIVATE "${terrain_legacy}/include")
        target_link_libraries(matter_terrain_${fixture}_tests PRIVATE matter_terrain)
        add_test(NAME terrain.legacy_${fixture} COMMAND matter_terrain_${fixture}_tests)
    endforeach()
    target_sources(matter_terrain_terrain_mesher_tests PRIVATE
        "${terrain_legacy}/src/terrain_river_overlay.cpp"
        "${terrain_legacy}/src/hydrology/river_geometry.cpp")
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
    if(MATTER_TERRAIN_BUILD_EXAMPLES)
        # Generate once, reload with the independent consumer, and prove the
        # package bytes are reproducible from a second clean generation.
        set(example_out "${CMAKE_CURRENT_BINARY_DIR}/example-package")
        add_test(NAME terrain.example.generate.v1
            COMMAND matter_terrain_example_tool "${example_out}/package")
        add_test(NAME terrain.example.reload.v1
            COMMAND matter_terrain_example_consumer "${example_out}/package")
        add_test(NAME terrain.example.reproducible.v1 COMMAND "${CMAKE_COMMAND}"
            "-DTOOL=$<TARGET_FILE:matter_terrain_example_tool>" "-DFIRST=${example_out}/package"
            "-DSECOND=${example_out}/repeat" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/example_reproducible.cmake")
        set_tests_properties(terrain.example.generate.v1 PROPERTIES FIXTURES_SETUP terrain_example_package)
        set_tests_properties(terrain.example.reload.v1 terrain.example.reproducible.v1
            PROPERTIES FIXTURES_REQUIRED terrain_example_package)
        set(example_tests terrain.example.generate.v1 terrain.example.reload.v1
            terrain.example.reproducible.v1)
        # Each fault package must be refused with its exact contract code.
        foreach(fault_case "asset-capability|unsupported_capability" "package-capability|unsupported_capability"
                "stale-source|stale_result" "tampered-payload|invalid_input")
            string(REPLACE "|" ";" fault_case "${fault_case}")
            list(GET fault_case 0 fault)
            list(GET fault_case 1 code)
            add_test(NAME terrain.example.fault_${fault}_generate.v1
                COMMAND matter_terrain_example_tool "${example_out}/${fault}" --fault ${fault})
            add_test(NAME terrain.example.fault_${fault}.v1
                COMMAND matter_terrain_example_consumer "${example_out}/${fault}" --expect ${code})
            set_tests_properties(terrain.example.fault_${fault}_generate.v1
                PROPERTIES FIXTURES_SETUP terrain_example_${fault})
            set_tests_properties(terrain.example.fault_${fault}.v1
                PROPERTIES FIXTURES_REQUIRED terrain_example_${fault})
            list(APPEND example_tests terrain.example.fault_${fault}_generate.v1 terrain.example.fault_${fault}.v1)
        endforeach()
        set_tests_properties(${example_tests} PROPERTIES ENVIRONMENT "MATTER_CONTOUR_SEAMS=1")
    endif()
    # The CPU service behind the contracts-only coordinator adapter: adjacent
    # chunks, late completion after invalidation, detail changes and faults.
    if(NOT TARGET matter_terrain_coordinator)
        function(matter_terrain_add_coordinator)
            set(BUILD_TESTING OFF)
            add_subdirectory("${terrain_legacy}/src/component_adapters/terrain_coordinator"
                "${CMAKE_CURRENT_BINARY_DIR}/terrain_coordinator")
        endfunction()
        matter_terrain_add_coordinator()
    endif()
    find_package(Threads REQUIRED)
    add_executable(matter_terrain_coordinator_integration_tests tests/coordinator_integration_tests.cpp)
    target_link_libraries(matter_terrain_coordinator_integration_tests
        PRIVATE matter_terrain matter_terrain_coordinator Threads::Threads)
    matter_terrain_warnings(matter_terrain_coordinator_integration_tests)
    foreach(fixture adjacent late_completion resolution_change faults)
        add_test(NAME terrain.example.coordinator_${fixture}.v1
            COMMAND matter_terrain_coordinator_integration_tests ${fixture})
        set_tests_properties(terrain.example.coordinator_${fixture}.v1
            PROPERTIES ENVIRONMENT "MATTER_CONTOUR_SEAMS=1" TIMEOUT 120)
    endforeach()
endif()
