cmake_minimum_required(VERSION 3.25)
project(MatterGraphicsNativeWaterTemporal LANGUAGES CXX)
include(CTest)
find_package(Vulkan REQUIRED)
get_filename_component(repo "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
# Compatibility build of the moved implementation, not a standalone scene API.
# Device/resource definitions are supplied by the native host at final link.
add_library(matter_graphics_native_water_temporal STATIC
    water_field_vk.cpp water_field_vk_resources.cpp water_animation_gpu.cpp
    water_mesh_animation_playback.cpp water_forward_reference.cpp
    water_surface_reference.cpp vk_temporal.cpp streamline_bridge.cpp)
target_compile_features(matter_graphics_native_water_temporal PUBLIC cxx_std_17)
target_include_directories(matter_graphics_native_water_temporal PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}" "${repo}/MatterEngine3/include"
    "${repo}/MatterEngine3/src" "${repo}/MatterEngine3/src/render"
    "${repo}/libs/MathLib/include" "${repo}/libs/ProfileLib/include")
target_link_libraries(matter_graphics_native_water_temporal PUBLIC Vulkan::Vulkan)
target_compile_definitions(matter_graphics_native_water_temporal PRIVATE
    MATTER_HAVE_STREAMLINE=0 NOMINMAX)
if(BUILD_TESTING)
    # Reuse the exact native fixtures and artifact codec; no producer is copied.
    add_library(water_playback_fixture_codec STATIC
        "${repo}/MatterEngine3/src/hydrology/water_mesh_animation_artifact.cpp"
        "${repo}/MatterEngine3/src/render/gpu_meshing/gpu_visual_mesher_common.cpp")
    target_link_libraries(water_playback_fixture_codec PUBLIC matter_graphics_native_water_temporal)
    foreach(fixture water_field_vk water_surface_reference water_forward_reference
                    water_mesh_animation_playback)
        add_executable(${fixture}_tests "${repo}/MatterEngine3/tests/${fixture}_tests.cpp")
        target_link_libraries(${fixture}_tests PRIVATE matter_graphics_native_water_temporal
            water_playback_fixture_codec)
        add_test(NAME ${fixture} COMMAND ${fixture}_tests)
    endforeach()
    # Compile the SDK boundary without the world's former transitive includes.
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/dlss_header.cpp"
        "#include \"streamline_bridge.h\"\nstatic_assert(sizeof(matter::DlssMode) == 1);\n")
    add_library(dlss_header OBJECT "${CMAKE_CURRENT_BINARY_DIR}/dlss_header.cpp")
    target_link_libraries(dlss_header PRIVATE matter_graphics_native_water_temporal)
endif()
