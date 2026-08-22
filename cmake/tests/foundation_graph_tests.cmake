cmake_minimum_required(VERSION 3.25)

get_filename_component(repository_root "${CMAKE_CURRENT_LIST_DIR}/../.." REALPATH)
set(test_root "${repository_root}/MatterEditor/build/cmake/foundation-graph-test")
set(build_dir "${test_root}/build")
set(assertion_hook "${test_root}/assert-foundation-graph.cmake")
file(REMOVE_RECURSE "${test_root}")
file(MAKE_DIRECTORY "${test_root}")

file(WRITE "${assertion_hook}" [=[
if(CMAKE_SOURCE_DIR STREQUAL MATTER_REPOSITORY_ROOT)
    function(matter_assert_foundation_graph)
        get_target_property(foundation_dependencies matter_foundation MANUALLY_ADDED_DEPENDENCIES)
        foreach(required_target IN ITEMS
                matter_memory
                matter_math
                matter_spatial
                matter_profile
                matter_particle_flow
                matter_mesh_charting
                matter_asset_store)
            if(NOT TARGET "${required_target}")
                message(FATAL_ERROR "foundation target is missing: ${required_target}")
            endif()
            list(FIND foundation_dependencies "${required_target}" dependency_index)
            if(dependency_index EQUAL -1)
                message(FATAL_ERROR
                    "matter_foundation does not explicitly aggregate ${required_target}: '${foundation_dependencies}'")
            endif()
        endforeach()
        list(LENGTH foundation_dependencies dependency_count)
        if(NOT dependency_count EQUAL 7)
            message(FATAL_ERROR
                "matter_foundation must aggregate exactly seven targets with BUILD_TESTING=OFF: '${foundation_dependencies}'")
        endif()

        if(TARGET matter_math_tests OR TARGET matter_memory_tests)
            message(FATAL_ERROR "foundation tests were created with BUILD_TESTING=OFF")
        endif()
    endfunction()
    cmake_language(DEFER CALL matter_assert_foundation_graph)
endif()
]=])

if(NOT DEFINED ENV{VSINSTALLDIR})
    message(FATAL_ERROR "foundation_graph_tests.cmake must run after VsDevCmd")
endif()
set(ninja "$ENV{VSINSTALLDIR}/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe")
if(NOT EXISTS "${ninja}")
    message(FATAL_ERROR "VS-bundled Ninja was not found: ${ninja}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${repository_root}"
        -B "${build_dir}"
        -G Ninja
        "-DCMAKE_MAKE_PROGRAM=${ninja}"
        -DBUILD_TESTING=OFF
        "-DMATTER_REPOSITORY_ROOT=${repository_root}"
        "-DCMAKE_PROJECT_INCLUDE=${assertion_hook}"
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_stdout
    ERROR_VARIABLE configure_stderr
)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR
        "Foundation graph configure failed.\n"
        "stdout:\n${configure_stdout}\n"
        "stderr:\n${configure_stderr}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${build_dir}" --target matter_foundation
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_stdout
    ERROR_VARIABLE build_stderr
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR
        "Foundation graph build failed.\n"
        "stdout:\n${build_stdout}\n"
        "stderr:\n${build_stderr}")
endif()

message(STATUS "Matter foundation graph includes all seven targets with BUILD_TESTING=OFF")
