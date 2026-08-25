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
if(NOT matter_engine_viewer_source_count EQUAL 18)
    message(FATAL_ERROR
        "engine-viewer.sources must provide exactly 18 viewer extensions; found ${matter_engine_viewer_source_count}")
endif()

# The current GNU editor defaults RETOPO=1. The viewer uses a complete,
# separately compiled engine object graph because its NDEBUG/Vulkan ABI policy
# deliberately differs from the accepted headless graph.
option(MATTER_ENABLE_AUTOREMESHER
    "Enable the source-built autoremesher in the engine viewer graph" ON)
set(matter_engine_viewer_product_sources
    ${matter_engine_core_sources}
    ${matter_engine_surface_sources}
    ${matter_engine_viewer_sources}
)
if(MATTER_ENABLE_AUTOREMESHER)
    list(APPEND matter_engine_viewer_product_sources
        libs/MatterSurfaceLib/src/mesh_retopo.cpp)
endif()
set(matter_engine_viewer_product_sources_unique
    ${matter_engine_viewer_product_sources})
list(REMOVE_DUPLICATES matter_engine_viewer_product_sources_unique)
list(LENGTH matter_engine_viewer_product_sources matter_viewer_product_count)
list(LENGTH matter_engine_viewer_product_sources_unique matter_viewer_unique_count)
if(MATTER_ENABLE_AUTOREMESHER)
    set(matter_expected_viewer_product_count 170)
else()
    set(matter_expected_viewer_product_count 169)
endif()
if(NOT matter_viewer_product_count EQUAL matter_expected_viewer_product_count OR
        NOT matter_viewer_unique_count EQUAL matter_expected_viewer_product_count)
    message(FATAL_ERROR
        "viewer product graph must contain canonical core/surface/viewer sources exactly once: "
        "count=${matter_viewer_product_count}, unique=${matter_viewer_unique_count}, "
        "expected=${matter_expected_viewer_product_count}")
endif()

add_library(matter_engine_viewer_objects OBJECT
    ${matter_engine_viewer_product_sources})
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
if(MATTER_ENABLE_PHYSX)
    target_compile_definitions(matter_engine_viewer_objects PRIVATE
        MATTER_ENABLE_PHYSX)
    target_link_libraries(matter_engine_viewer_objects PUBLIC
        matter_physx_adapter)
endif()
if(MATTER_ENABLE_AUTOREMESHER)
    target_compile_definitions(matter_engine_viewer_objects PRIVATE
        MATTER_HAVE_AUTOREMESHER)
endif()
target_link_libraries(matter_engine_viewer_objects PUBLIC
    matter_memory
    matter_math
    matter_spatial
    matter_profile
    matter_particle_flow
    matter_mesh_charting
    matter_asset_store
    matter_quickjs
    matter_flecs
    matter_box3d
    matter_ozz_offline
    matter_bc7enc
    matter_glfw
    matter_vulkan_sdk
)
if(MATTER_ENABLE_AUTOREMESHER)
    target_link_libraries(matter_engine_viewer_objects PUBLIC
        matter_autoremesher)
endif()
matter_apply_project_defaults(matter_engine_viewer_objects)
add_dependencies(matter_engine_viewer_objects matter_embedded_spirv)

if(BUILD_TESTING)
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
        MatterEngine3/src/render/gpu_meshing/gpu_visual_mesher_common.cpp
        MatterEngine3/src/render/gpu_meshing/gpu_visual_mesher_vk.cpp
        MatterEngine3/src/render/gpu_meshing/water_scene_part.cpp
        MatterEngine3/src/hydrology/hydrology_artifact.cpp
        MatterEngine3/src/hydrology/water_visual_products.cpp
        MatterEngine3/tests/gpu_visual_mesher_vk_tests.cpp
        libs/MatterSurfaceLib/src/surface.c
        libs/MatterSurfaceLib/src/fat_primitive.c
        libs/SpatialQueryLib/src/spatial_hash.c
        libs/MemoryLib/src/mem_pool.c
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

    add_test(NAME vulkan_scene_oracle_tests
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/MatterEditor/tools/tests/vulkan_scene_oracle_tests.ps1"
            -RepositoryRoot "${CMAKE_SOURCE_DIR}"
    )
    set_tests_properties(vulkan_scene_oracle_tests PROPERTIES
        LABELS "vulkan;viewer")

    set(matter_vt_compositor_test_sources
        MatterEngine3/src/render/vt_compositor.cpp
        MatterEngine3/src/terrain_field.cpp
        MatterEngine3/src/render/vk_context.cpp
        MatterEngine3/src/render/vk_resources.cpp
        MatterEngine3/src/render/streamline_bridge.cpp
    )
    add_library(matter_vt_compositor_test_objects OBJECT
        ${matter_vt_compositor_test_sources})
    matter_engine_include_directories(matter_vt_compositor_test_objects PRIVATE)
    target_include_directories(matter_vt_compositor_test_objects BEFORE PRIVATE
        "${CMAKE_BINARY_DIR}/MatterEngine3"
        "${matter_vulkan_include}"
        "${CMAKE_SOURCE_DIR}/MatterEngine3/tests"
        "${CMAKE_SOURCE_DIR}/third_party/raylib/src/external/glfw/include"
    )
    target_compile_definitions(matter_vt_compositor_test_objects PRIVATE
        NOMINMAX
        MATTER_HAVE_STREAMLINE=0
        MATTER_PROFILE_ENABLED=0
        VK_USE_PLATFORM_WIN32_KHR
    )
    target_link_libraries(matter_vt_compositor_test_objects PRIVATE
        matter_glfw matter_flecs matter_vulkan_sdk)
    matter_apply_project_defaults(matter_vt_compositor_test_objects)
    matter_apply_test_assertion_policy(matter_vt_compositor_test_objects)
    add_dependencies(matter_vt_compositor_test_objects matter_embedded_spirv)

    add_executable(vt_compositor_tests
        MatterEngine3/tests/vt_compositor_tests.cpp
        $<TARGET_OBJECTS:matter_vt_compositor_test_objects>
    )
    matter_engine_include_directories(vt_compositor_tests PRIVATE)
    target_include_directories(vt_compositor_tests BEFORE PRIVATE
        "${CMAKE_BINARY_DIR}/MatterEngine3"
        "${matter_vulkan_include}"
        "${CMAKE_SOURCE_DIR}/MatterEngine3/tests"
        "${CMAKE_SOURCE_DIR}/third_party/raylib/src/external/glfw/include"
    )
    target_compile_definitions(vt_compositor_tests PRIVATE
        NOMINMAX
        MATTER_HAVE_STREAMLINE=0
        VK_USE_PLATFORM_WIN32_KHR
        "MATTER_VK_TEST_LAYER_PATH=\"${matter_vulkan_runtime}\""
    )
    target_link_libraries(vt_compositor_tests PRIVATE
        matter_glfw matter_flecs matter_vulkan_sdk)
    matter_apply_project_defaults(vt_compositor_tests)
    matter_apply_test_assertion_policy(vt_compositor_tests)
    add_dependencies(vt_compositor_tests matter_embedded_spirv)
    add_test(NAME vt_compositor_tests COMMAND vt_compositor_tests)
    set_tests_properties(vt_compositor_tests PROPERTIES
        LABELS vulkan
        PASS_REGULAR_EXPRESSION "ALL PASS"
        FAIL_REGULAR_EXPRESSION "validation errors: [1-9][0-9]*"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    )

    add_test(NAME vt_link_inventory_tests
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/cmake/tests/vt_link_inventory_test.ps1"
            -BuildDirectory "${CMAKE_BINARY_DIR}"
            -Ninja "${CMAKE_MAKE_PROGRAM}"
    )
    set_tests_properties(vt_link_inventory_tests PROPERTIES
        DEPENDS vt_compositor_tests
        LABELS "vulkan;compiler-policy")

    add_test(NAME viewer_graph_tests
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/cmake/tests/viewer_graph_tests.ps1"
    )
    set_tests_properties(viewer_graph_tests PROPERTIES
        LABELS compiler-policy)

    add_test(NAME python_contract_tests
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/cmake/tests/python_contract_tests.ps1"
    )
    set_tests_properties(python_contract_tests PROPERTIES
        LABELS compiler-policy)

    add_test(NAME shader_rebuild_tests
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/cmake/tests/shader_rebuild_test.ps1"
    )
    set_tests_properties(shader_rebuild_tests PROPERTIES LABELS shader)
endif()
