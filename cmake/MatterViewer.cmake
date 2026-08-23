include_guard(GLOBAL)

set(matter_vulkan_sdk_root "C:/VulkanSDK/1.4.357.0")
set(matter_vulkan_include "${matter_vulkan_sdk_root}/Include")
set(matter_vulkan_loader "${matter_vulkan_sdk_root}/Lib/vulkan-1.lib")
set(matter_vulkan_runtime "${matter_vulkan_sdk_root}/Bin")
foreach(required_vulkan_path IN ITEMS
        "${matter_vulkan_include}/vulkan/vulkan.h"
        "${matter_vulkan_loader}"
        "${matter_vulkan_runtime}/VkLayer_khronos_validation.json")
    if(NOT EXISTS "${required_vulkan_path}")
        message(FATAL_ERROR
            "Pinned Vulkan SDK component was not found: ${required_vulkan_path}")
    endif()
endforeach()

add_library(matter_vulkan_sdk INTERFACE)
target_include_directories(matter_vulkan_sdk INTERFACE "${matter_vulkan_include}")
target_link_libraries(matter_vulkan_sdk INTERFACE "${matter_vulkan_loader}")

matter_read_manifest(
    "${CMAKE_SOURCE_DIR}/cmake/manifests/engine-viewer.sources"
    matter_engine_viewer_sources
    PLATFORM windows
)
list(FILTER matter_engine_viewer_sources
    INCLUDE REGEX "^MatterEngine3/src/.*\\.cpp$")
list(LENGTH matter_engine_viewer_sources matter_engine_viewer_source_count)
if(NOT matter_engine_viewer_source_count EQUAL 17)
    message(FATAL_ERROR
        "engine-viewer.sources must provide exactly 17 viewer extensions; found ${matter_engine_viewer_source_count}")
endif()

# The current GNU editor defaults RETOPO=1. Preserve that behavior without
# adding mesh_retopo.cpp to the viewer-only manifest: it belongs to the
# MatterSurface object set and is enabled together with its only caller.
option(MATTER_ENABLE_AUTOREMESHER
    "Enable the source-built autoremesher in engine/editor object graphs" ON)
if(MATTER_ENABLE_AUTOREMESHER)
    target_sources(matter_engine_surface_objects PRIVATE
        "${CMAKE_SOURCE_DIR}/libs/MatterSurfaceLib/src/mesh_retopo.cpp")
    target_compile_definitions(matter_engine_core PRIVATE
        MATTER_HAVE_AUTOREMESHER)
    target_link_libraries(matter_engine_core PUBLIC matter_autoremesher)
    target_link_libraries(matter_engine_surface_objects PRIVATE matter_autoremesher)
    target_link_libraries(matter_engine_headless PUBLIC matter_autoremesher)
endif()

add_library(matter_engine_viewer_objects OBJECT ${matter_engine_viewer_sources})
matter_engine_include_directories(matter_engine_viewer_objects PRIVATE)
target_include_directories(matter_engine_viewer_objects BEFORE PRIVATE
    "${CMAKE_BINARY_DIR}/MatterEngine3"
    "${matter_vulkan_include}"
    "${CMAKE_SOURCE_DIR}/third_party/raylib/src/external/glfw/include"
)
target_compile_definitions(matter_engine_viewer_objects PRIVATE
    PLATFORM_DESKTOP
    NDEBUG
    NOMINMAX
    MATTER_HAVE_SCRIPT_HOST
    MATTER_VULKAN_VIEWER
    MATTER_VULKAN_ONLY
    MATTER_HAVE_STREAMLINE=0
)
target_link_libraries(matter_engine_viewer_objects PUBLIC
    matter_engine_headless
    matter_glfw
    matter_vulkan_sdk
)
matter_apply_project_defaults(matter_engine_viewer_objects)
add_dependencies(matter_engine_viewer_objects matter_embedded_spirv)

if(BUILD_TESTING)
    function(matter_configure_vulkan_test target)
        matter_engine_include_directories("${target}" PRIVATE)
        target_include_directories("${target}" BEFORE PRIVATE
            "${CMAKE_BINARY_DIR}/MatterEngine3"
            "${matter_vulkan_include}"
            "${CMAKE_SOURCE_DIR}/MatterEngine3/tests"
            "${CMAKE_SOURCE_DIR}/third_party/raylib/src/external/glfw/include"
        )
        target_compile_definitions("${target}" PRIVATE
            PLATFORM_DESKTOP
            NDEBUG
            NOMINMAX
            MATTER_VULKAN_ONLY
            MATTER_HAVE_STREAMLINE=0
            VK_USE_PLATFORM_WIN32_KHR
            "MATTER_VK_TEST_LAYER_PATH=\"${matter_vulkan_runtime}\""
        )
        target_link_libraries("${target}" PRIVATE
            matter_engine_viewer_objects
            matter_vulkan_sdk
        )
        matter_apply_project_defaults("${target}")
        matter_apply_test_assertion_policy("${target}")
        add_dependencies("${target}" matter_embedded_spirv)
    endfunction()

    add_executable(vulkan_compat_tests
        MatterEngine3/tests/vulkan_compat_tests.cpp
        MatterEngine3/src/render/vulkan_only_compat.cpp
    )
    matter_engine_include_directories(vulkan_compat_tests PRIVATE)
    target_compile_definitions(vulkan_compat_tests PRIVATE
        NDEBUG
        MATTER_VULKAN_COMPAT_TESTING
        MATTER_VULKAN_ONLY
    )
    matter_apply_project_defaults(vulkan_compat_tests)
    matter_apply_test_assertion_policy(vulkan_compat_tests)
    add_test(NAME vulkan_compat_tests COMMAND vulkan_compat_tests)
    set_tests_properties(vulkan_compat_tests PROPERTIES LABELS vulkan)

    set(matter_vulkan_smoke_sources
        MatterEngine3/src/render/streamline_bridge.cpp
        MatterEngine3/src/render/vk_context.cpp
        MatterEngine3/src/render/vk_resources.cpp
        MatterEngine3/src/render/vk_pipeline.cpp
        MatterEngine3/src/render/vk_scene_renderer.cpp
        MatterEngine3/src/render/vk_animation_skinning.cpp
        MatterEngine3/src/render/vk_animation_bounds.cpp
        MatterEngine3/src/animation/animation_budget.cpp
        MatterEngine3/src/render/tileset_slicer.cpp
        MatterEngine3/src/render/bc_encode.cpp
        MatterEngine3/src/render/vt_residency.cpp
        MatterEngine3/src/props/props.cpp
        MatterEngine3/src/util/json_doc.cpp
        MatterEngine3/src/render/vt_stub_filler.cpp
        MatterEngine3/src/render/vt_compositor.cpp
        MatterEngine3/src/render/vt_enrich.cpp
        MatterEngine3/src/terrain_field.cpp
        MatterEngine3/tests/vulkan_smoke_tileset_bake_stub.cpp
        MatterEngine3/src/tileset_gtex.cpp
        MatterEngine3/src/render/vk_volumetrics.cpp
        MatterEngine3/src/render/vk_atmosphere.cpp
        MatterEngine3/src/render/vk_cloud_shadows.cpp
        MatterEngine3/src/render/vk_emitter_gather.cpp
        MatterEngine3/src/render/raster_mesh.cpp
        MatterEngine3/src/render/indexed_part_geometry.cpp
        MatterEngine3/src/render/vk_instance_cache.cpp
        MatterEngine3/src/render/frame_matrices.cpp
        MatterEngine3/src/render/vk_temporal.cpp
        MatterEngine3/src/render/vk_gi_math.cpp
        MatterEngine3/src/render/vk_lighting_controls.cpp
        MatterEngine3/src/render/matrix_math.cpp
        MatterEngine3/src/render/lod_trace.cpp
    )
    add_library(matter_vulkan_smoke_objects OBJECT ${matter_vulkan_smoke_sources})
    matter_engine_include_directories(matter_vulkan_smoke_objects PRIVATE)
    target_include_directories(matter_vulkan_smoke_objects BEFORE PRIVATE
        "${CMAKE_BINARY_DIR}/MatterEngine3"
        "${matter_vulkan_include}"
        "${CMAKE_SOURCE_DIR}/MatterEngine3/tests"
        "${CMAKE_SOURCE_DIR}/third_party/raylib/src/external/glfw/include"
    )
    target_compile_definitions(matter_vulkan_smoke_objects PRIVATE
        PLATFORM_DESKTOP
        NOMINMAX
        MATTER_VK_TEST_FAULT_INJECTION
        MATTER_VULKAN_ONLY
        MATTER_HAVE_STREAMLINE=0
        VK_USE_PLATFORM_WIN32_KHR
    )
    target_link_libraries(matter_vulkan_smoke_objects PRIVATE
        matter_glfw
        matter_flecs
        matter_bc7enc
        matter_profile
        matter_vulkan_sdk
    )
    matter_apply_project_defaults(matter_vulkan_smoke_objects)
    matter_apply_test_assertion_policy(matter_vulkan_smoke_objects)
    add_dependencies(matter_vulkan_smoke_objects matter_embedded_spirv)

    add_executable(vulkan_smoke_tests
        MatterEngine3/tests/vulkan_smoke_tests.cpp
        MatterEditor/src/ui_lighting_controls.cpp
        $<TARGET_OBJECTS:matter_vulkan_smoke_objects>
    )
    matter_engine_include_directories(vulkan_smoke_tests PRIVATE)
    target_include_directories(vulkan_smoke_tests BEFORE PRIVATE
        "${CMAKE_BINARY_DIR}/MatterEngine3"
        "${matter_vulkan_include}"
        "${CMAKE_SOURCE_DIR}/MatterEngine3/tests"
        "${CMAKE_SOURCE_DIR}/third_party/raylib/src/external/glfw/include"
    )
    target_compile_definitions(vulkan_smoke_tests PRIVATE
        PLATFORM_DESKTOP
        NOMINMAX
        MATTER_VK_TEST_FAULT_INJECTION
        MATTER_VULKAN_ONLY
        MATTER_HAVE_STREAMLINE=0
        VK_USE_PLATFORM_WIN32_KHR
        "MATTER_VK_TEST_LAYER_PATH=\"${matter_vulkan_runtime}\""
    )
    target_link_libraries(vulkan_smoke_tests PRIVATE
        matter_glfw
        matter_flecs
        matter_bc7enc
        matter_profile
        matter_vulkan_sdk
    )
    matter_apply_project_defaults(vulkan_smoke_tests)
    matter_apply_test_assertion_policy(vulkan_smoke_tests)
    add_dependencies(vulkan_smoke_tests
        matter_embedded_spirv
        matter_engine_viewer_objects)
    add_test(NAME vulkan_smoke_tests COMMAND vulkan_smoke_tests)
    set_tests_properties(vulkan_smoke_tests PROPERTIES
        LABELS vulkan
        PASS_REGULAR_EXPRESSION "ALL PASS"
        FAIL_REGULAR_EXPRESSION "validation errors: [1-9][0-9]*"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    )

    add_executable(vt_compositor_tests
        MatterEngine3/tests/vt_compositor_tests.cpp
    )
    matter_configure_vulkan_test(vt_compositor_tests)
    add_test(NAME vt_compositor_tests COMMAND vt_compositor_tests)
    set_tests_properties(vt_compositor_tests PROPERTIES
        LABELS vulkan
        PASS_REGULAR_EXPRESSION "ALL PASS"
        FAIL_REGULAR_EXPRESSION "validation errors: [1-9][0-9]*"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    )

    add_test(NAME shader_rebuild_tests
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/cmake/tests/shader_rebuild_test.ps1"
    )
    set_tests_properties(shader_rebuild_tests PROPERTIES LABELS shader)
endif()
