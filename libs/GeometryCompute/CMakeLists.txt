cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(MatterGeometryCompute LANGUAGES C CXX)
    include(CTest)
endif()
option(MATTER_GEOMETRY_COMPUTE "Enable the optional solid compute backend" OFF)
if(MATTER_GEOMETRY_COMPUTE)
    set(MATTER_COMPUTE_VULKAN ON)
endif()
if(NOT TARGET matter_compute_device)
    add_subdirectory(../ComputeDevice "${CMAKE_CURRENT_BINARY_DIR}/device")
endif()
get_filename_component(_repo "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
add_library(matter_geometry_compute STATIC src/solid.cpp)
target_link_libraries(matter_geometry_compute PUBLIC matter_compute_device)
target_include_directories(matter_geometry_compute PUBLIC include)
if(MATTER_GEOMETRY_COMPUTE)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    find_program(MATTER_COMPUTE_GLSLC NAMES glslc HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)
    set(_shaders "${_repo}/MatterEngine3/shaders_vk")
    set(_spirv)
    foreach(stage field classify scan emit)
        set(_output "${CMAKE_CURRENT_BINARY_DIR}/solid_${stage}.comp.spv")
        add_custom_command(OUTPUT "${_output}"
            COMMAND "${MATTER_COMPUTE_GLSLC}" --target-env=vulkan1.2
                "-I${_shaders}" "${_shaders}/solid_${stage}.comp" -o "${_output}"
            DEPENDS "${_shaders}/solid_${stage}.comp" "${_shaders}/solid_field.glsl"
                "${_shaders}/gpu_mesh_mc_tables.glsl" VERBATIM)
        list(APPEND _spirv "${_output}")
    endforeach()
    set(_header "${CMAKE_CURRENT_BINARY_DIR}/solid_spirv.h")
    add_custom_command(OUTPUT "${_header}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed.py" "${_header}" ${_spirv}
        DEPENDS ${_spirv} tools/embed.py VERBATIM)
    target_sources(matter_geometry_compute PRIVATE "${_header}"
        "${_repo}/MatterEngine3/src/render/gpu_meshing/gpu_solid_mesher_common.cpp")
    target_include_directories(matter_geometry_compute PRIVATE "${CMAKE_CURRENT_BINARY_DIR}"
        "${_repo}/MatterEngine3/include" "${_repo}/libs/MatterSurfaceLib/src")
    target_compile_definitions(matter_geometry_compute PRIVATE MATTER_GEOMETRY_COMPUTE=1)
endif()
if(MSVC)
    target_compile_options(matter_geometry_compute PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(matter_geometry_compute PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
if(BUILD_TESTING)
    add_executable(matter_geometry_compute_tests tests/solid_tests.cpp)
    target_link_libraries(matter_geometry_compute_tests PRIVATE matter_geometry_compute)
    if(MATTER_GEOMETRY_COMPUTE)
        target_link_libraries(matter_geometry_compute_tests PRIVATE matter_compute_vulkan)
        target_compile_definitions(matter_geometry_compute_tests PRIVATE MATTER_GEOMETRY_COMPUTE=1)
        target_include_directories(matter_geometry_compute_tests PRIVATE "${_repo}/MatterEngine3/include")
    endif()
    add_test(NAME matter_geometry_compute COMMAND matter_geometry_compute_tests)
    set_tests_properties(matter_geometry_compute PROPERTIES TIMEOUT 120)
    # Source/build reads are qualification, never runtime test fixture reads.
    # Request CMake's generated graph rather than assume a Ninja generator.
    set(_qualification_graph_args)
    if(COMMAND cmake_file_api)
        cmake_file_api(QUERY API_VERSION 1 CODEMODEL 2)
    elseif(CMAKE_GENERATOR STREQUAL "Ninja")
        # Keep the existing 3.25/Ninja toolchain supported. Other generators
        # need the current-invocation file-api query introduced in 3.27.
        list(APPEND _qualification_graph_args "-DNINJA=${CMAKE_MAKE_PROGRAM}")
    else()
        message(FATAL_ERROR "Compute graph qualification requires CMake 3.27 or Ninja")
    endif()
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/compute-dependencies-$<CONFIG>.txt" CONTENT
        "$<TARGET_PROPERTY:matter_compute_device,LINK_LIBRARIES>\n$<TARGET_PROPERTY:matter_geometry_compute,LINK_LIBRARIES>\n")
    # Always recheck: source edits, added headers, regenerated graphs and changed
    # transitive links must not reuse a stale success stamp. This also protects
    # an explicit build of the runtime test instead of the default ALL target.
    add_custom_target(matter_geometry_compute_qualification ALL
        COMMAND "${CMAKE_COMMAND}"
            "-DBUILD=${CMAKE_BINARY_DIR}" "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DLINKS=${CMAKE_CURRENT_BINARY_DIR}/compute-dependencies-$<CONFIG>.txt"
            "-DCONFIG=$<CONFIG>"
            ${_qualification_graph_args}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/dependencies.cmake"
        VERBATIM)
    add_dependencies(matter_geometry_compute_tests matter_geometry_compute_qualification)
endif()
