# Production producer relocation. The canonical Vulkan resource helpers and
# the device-lifetime registry/accessors they need (vk_resources.cpp,
# vk_device_internal.cpp) are owned here; the host supplies the VulkanDevice
# itself (creation, queues, teardown, the immediate submit). This archive does
# not acquire a window, world, scene or generator.
set(_environment_spirv)
foreach(shader atmosphere_transmittance atmosphere_multiscatter atmosphere_sky_view
        atmosphere_irradiance cloud_shadow_density cloud_shadow_integrate
        cloud_shadow_reproject vol_density vol_scatter vol_integrate)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/${shader}.comp.spv")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_shader.py"
            "${MATTER_GRAPHICS_GLSLC}" "${_shader_dir}/${shader}.comp" "${output}" vulkan1.3 -O
        DEPENDS "${_shader_dir}/${shader}.comp" tools/compile_shader.py
        DEPFILE "${output}.d" VERBATIM)
    list(APPEND _environment_spirv "${output}")
endforeach()
set(_environment_header "${CMAKE_CURRENT_BINARY_DIR}/environment_embedded_spirv.h")
add_custom_command(OUTPUT "${_environment_header}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_environment.py"
        "${_environment_header}" ${_environment_spirv}
    DEPENDS ${_environment_spirv} tools/embed_environment.py
        "${_graphics_repo}/MatterEngine3/tools/embed_spirv.py" VERBATIM)
add_library(matter_graphics_environment STATIC src/native/vk_atmosphere.cpp
    src/native/vk_cloud_shadows.cpp src/native/vk_volumetrics.cpp
    src/native/vk_resources.cpp src/native/vk_device_internal.cpp "${_environment_header}")
target_compile_features(matter_graphics_environment PUBLIC cxx_std_17)
target_compile_definitions(matter_graphics_environment PRIVATE MATTER_GRAPHICS_NATIVE_ENVIRONMENT)
target_include_directories(matter_graphics_environment PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/native" "${_graphics_repo}/MatterEngine3/include")
target_include_directories(matter_graphics_environment PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(matter_graphics_environment PUBLIC Vulkan::Vulkan)
if(MSVC)
    target_compile_options(matter_graphics_environment PRIVATE /W4 /permissive- /utf-8)
endif()
if(BUILD_TESTING)
    # Pin the production wiring/layout during the build; CTest exercises only
    # generated numerical fixtures and does not resolve production source files.
    include(cmake/EnvironmentQualification.cmake)
    foreach(suite atmosphere cloud_layer cloud_shadow volumetric_quality)
        add_executable(matter_graphics_${suite}_tests
            "${_graphics_repo}/MatterEngine3/tests/${suite}_tests.cpp")
        target_link_libraries(matter_graphics_${suite}_tests PRIVATE matter_graphics_environment)
        if(suite STREQUAL "cloud_layer" OR suite STREQUAL "volumetric_quality")
            matter_qualify_environment_test(matter_graphics_${suite}_tests "${Python3_EXECUTABLE}")
        endif()
        add_test(NAME matter_graphics_${suite} COMMAND matter_graphics_${suite}_tests)
        set_tests_properties(matter_graphics_${suite} PROPERTIES
            LABELS "cpu;graphics" TIMEOUT 10)
    endforeach()
    add_executable(matter_graphics_cloud_shadow_reference_tests
        tests/cloud_shadow_reference_tests.cpp)
    target_link_libraries(matter_graphics_cloud_shadow_reference_tests PRIVATE matter_graphics_environment)
    add_test(NAME matter_graphics_cloud_shadow_reference COMMAND matter_graphics_cloud_shadow_reference_tests)
    set_tests_properties(matter_graphics_cloud_shadow_reference PROPERTIES LABELS "cpu;graphics" TIMEOUT 10)
endif()
