cmake_minimum_required(VERSION 3.25)
project(MatterCompatibilityCorpus LANGUAGES C CXX)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
get_filename_component(MATTER_REPO "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(BUILD_TESTING OFF CACHE BOOL "Only build the local corpus runner" FORCE)
add_subdirectory("${MATTER_REPO}/libs/MatterContracts" contracts)
add_executable(corpus_native native.cpp "${MATTER_REPO}/MatterEngine3/src/part_graph.cpp")
target_link_libraries(corpus_native PRIVATE matter_contracts)
target_include_directories(corpus_native PRIVATE
    "${MATTER_REPO}/MatterEngine3/src" "${MATTER_REPO}/MatterEngine3/include"
    "${MATTER_REPO}/libs/MatterSurfaceLib/include"
    "${MATTER_REPO}/libs/SpatialQueryLib/include" "${MATTER_REPO}/libs/ProfileLib/include"
    "${MATTER_REPO}/third_party/raylib/src")
# Compile the original legacy serializer, without its script-host adapter.
# Unused engine install functions are discarded; there is no copied producer.
if(MSVC)
    # Legacy part_graph.cpp also emits its unused memo-key helper. Resolve its
    # legacy FNV symbol from the supported headless archive, never a test stub.
    set(MATTER_CORPUS_LEGACY_ARCHIVE "${MATTER_REPO}/MatterEditor/build/cmake/windows-msvc/relwithdebinfo/matter_engine_headless.lib"
        CACHE FILEPATH "Supported legacy headless archive (build part_graph_tests first)")
    if(NOT EXISTS "${MATTER_CORPUS_LEGACY_ARCHIVE}")
        message(FATAL_ERROR "Build the supported RelWithDebInfo part_graph_tests target first")
    endif()
    target_link_libraries(corpus_native PRIVATE "${MATTER_CORPUS_LEGACY_ARCHIVE}")
    get_filename_component(legacy_directory "${MATTER_CORPUS_LEGACY_ARCHIVE}" DIRECTORY)
    target_link_libraries(corpus_native PRIVATE
        "${legacy_directory}/matter_spatial.lib" "${legacy_directory}/matter_memory.lib"
        "${legacy_directory}/matter_profile.lib")
    target_compile_options(corpus_native PRIVATE /Gy /utf-8 /permissive-)
    target_link_options(corpus_native PRIVATE /OPT:REF)
else()
    target_compile_options(corpus_native PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(corpus_native PRIVATE -Wl,--gc-sections)
endif()
enable_testing()
add_test(NAME compatibility_corpus COMMAND corpus_native "${CMAKE_CURRENT_SOURCE_DIR}")
