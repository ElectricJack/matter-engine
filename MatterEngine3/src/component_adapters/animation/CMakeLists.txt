cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(MatterAnimationNative VERSION 1.0.0 LANGUAGES C CXX)
    include(CTest)
endif()
get_filename_component(_animation_root "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
if(NOT TARGET matter_animation_asset)
    # Prevent the portable subdirectory from registering this directory again.
    set(MATTER_ANIMATION_NATIVE OFF)
    add_subdirectory("${_animation_root}/libs/AssetPipeline/animation" "${CMAKE_CURRENT_BINARY_DIR}/portable")
endif()
if(TARGET matter_ozz_offline)
    set(_animation_ozz matter_ozz_offline)
else()
    foreach(flag tools fbx gltf data samples howtos tests postfix)
        set(ozz_build_${flag} OFF)
    endforeach()
    set(ozz_build_msvc_rt_dll ON)
    add_subdirectory("${_animation_root}/third_party/ozz-animation" "${CMAKE_CURRENT_BINARY_DIR}/ozz" EXCLUDE_FROM_ALL)
    set(_animation_ozz ozz_animation_offline)
endif()
# Compile the existing implementation in place. No source copies, monolithic
# archive, runtime store/systems, renderer/device, ScriptHost or ECS target.
set(_animation_sources)
foreach(source animation_ir animation_validate animation_binding_bake
               animation_runtime_asset animation_evaluator animation_targets
               animation_controllers animation_budget ozz_adapter ozz_bake)
    list(APPEND _animation_sources "${_animation_root}/MatterEngine3/src/animation/${source}.cpp")
endforeach()
add_library(matter_animation_native STATIC native.cpp ${_animation_sources}
    "${_animation_root}/MatterEngine3/src/render/indexed_part_geometry.cpp")
target_compile_features(matter_animation_native PUBLIC cxx_std_17)
target_link_libraries(matter_animation_native PUBLIC matter_animation_asset PRIVATE ${_animation_ozz})
target_include_directories(matter_animation_native PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${_animation_root}/MatterEngine3/src"
    "${_animation_root}/MatterEngine3/include"
    PRIVATE
    "${_animation_root}/MatterEngine3/src/render"
    "${_animation_root}/libs/SpatialQueryLib/include"
    "${_animation_root}/libs/MatterSurfaceLib/include"
    "${_animation_root}/third_party/raylib/src")
target_compile_definitions(matter_animation_native PRIVATE MATTER_VULKAN_ONLY)
if(MSVC)
    target_compile_options(matter_animation_native PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(matter_animation_native PRIVATE -Wall -Wextra -msse4.1)
endif()
if(BUILD_TESTING)
    # Existing fixture sources remain authoritative, compiled without the engine.
    foreach(fixture animation_ir animation_binding animation_ozz_adapter animation_runtime_asset)
        string(REPLACE "animation_" "" fixture_short "${fixture}")
        set(fixture_target "anim_${fixture_short}_test")
        add_executable(${fixture_target} "${_animation_root}/MatterEngine3/tests/${fixture}_tests.cpp")
        target_link_libraries(${fixture_target} PRIVATE matter_animation_native)
        target_include_directories(${fixture_target} PRIVATE
            "${_animation_root}/MatterEngine3/src/render"
            "${_animation_root}/libs/SpatialQueryLib/include"
            "${_animation_root}/libs/MatterSurfaceLib/include"
            "${_animation_root}/third_party/raylib/src")
        target_compile_definitions(${fixture_target} PRIVATE MATTER_VULKAN_ONLY)
        if(NOT MSVC)
            target_compile_options(${fixture_target} PRIVATE -msse4.1)
        endif()
        add_test(NAME matter_extracted_${fixture} COMMAND ${fixture_target})
    endforeach()
    add_executable(matter_animation_native_tests
        "${_animation_root}/libs/AssetPipeline/animation/tests/native_tests.cpp")
    target_link_libraries(matter_animation_native_tests PRIVATE matter_animation_native)
    target_include_directories(matter_animation_native_tests PRIVATE
        "${_animation_root}/MatterEngine3/src/render"
        "${_animation_root}/libs/SpatialQueryLib/include"
        "${_animation_root}/libs/MatterSurfaceLib/include"
        "${_animation_root}/third_party/raylib/src")
    target_compile_definitions(matter_animation_native_tests PRIVATE MATTER_VULKAN_ONLY)
    if(NOT MSVC)
        target_compile_options(matter_animation_native_tests PRIVATE -msse4.1)
    endif()
    add_test(NAME matter_animation_native COMMAND matter_animation_native_tests)
endif()
