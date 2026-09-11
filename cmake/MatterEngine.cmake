include_guard(GLOBAL)

function(matter_engine_include_directories target visibility)
    add_dependencies("${target}"
        matter_embedded_shader_text
        matter_embedded_spirv)
    target_include_directories("${target}" BEFORE ${visibility}
        "${CMAKE_BINARY_DIR}/MatterEngine3")
    target_include_directories("${target}" ${visibility}
        "${CMAKE_SOURCE_DIR}/MatterEngine3"
        "${CMAKE_SOURCE_DIR}/MatterEngine3/include"
        "${CMAKE_SOURCE_DIR}/MatterEngine3/src"
        "${CMAKE_SOURCE_DIR}/MatterEngine3/src/render"
        "${CMAKE_SOURCE_DIR}/MatterEngine3/src/provider"
        "${CMAKE_SOURCE_DIR}/libs/MatterSurfaceLib/include"
        "${CMAKE_SOURCE_DIR}/libs/MatterSurfaceLib/src"
        "${CMAKE_SOURCE_DIR}/libs/SpatialQueryLib/include"
        "${CMAKE_SOURCE_DIR}/libs/MemoryLib/include"
        "${CMAKE_SOURCE_DIR}/libs/MathLib/include"
        "${CMAKE_SOURCE_DIR}/libs/ProfileLib/include"
        "${CMAKE_SOURCE_DIR}/libs/ParticleFlowLib/include"
        "${CMAKE_SOURCE_DIR}/libs/MeshChartingLib/include"
        "${CMAKE_SOURCE_DIR}/third_party/quickjs-ng"
        "${CMAKE_SOURCE_DIR}/third_party/raylib/src"
        "${CMAKE_SOURCE_DIR}/third_party/box3d/include"
        "${CMAKE_SOURCE_DIR}/third_party/bc7enc"
        "${CMAKE_SOURCE_DIR}/third_party/flecs"
        "${CMAKE_SOURCE_DIR}/third_party/ozz-animation/include"
        "${CMAKE_SOURCE_DIR}/third_party/Vulkan-Headers/include"
    )
endfunction()

function(matter_require_headless_assertion_policy target)
    get_target_property(target_options "${target}" COMPILE_OPTIONS)
    set(required_force_include
        "/FI${CMAKE_SOURCE_DIR}/cmake/MatterHeadlessConfig.h")
    list(FIND target_options "${required_force_include}" force_include_index)
    if(force_include_index EQUAL -1)
        message(FATAL_ERROR
            "${target} does not preserve headless no-NDEBUG semantics: '${target_options}'")
    endif()
    list(FIND target_options "/UNDEBUG" undefine_option_index)
    if(NOT undefine_option_index EQUAL -1)
        message(FATAL_ERROR
            "${target} uses conflicting /UNDEBUG policy: '${target_options}'")
    endif()
endfunction()

function(matter_apply_headless_config target)
    target_compile_options("${target}" PRIVATE
        "/FI${CMAKE_SOURCE_DIR}/cmake/MatterHeadlessConfig.h"
    )
endfunction()

matter_read_manifest(
    "${CMAKE_SOURCE_DIR}/cmake/manifests/engine-core.sources"
    matter_engine_core_manifest
    PLATFORM windows
)
set(matter_engine_core_sources ${matter_engine_core_manifest})
list(FILTER matter_engine_core_sources INCLUDE REGEX "^MatterEngine3/src/.*\\.cpp$")
if(WIN32)
    # The inotify implementation is Linux-only. Its source retains the existing
    # Linux guards and remains in the canonical manifest/Make build.
    list(REMOVE_ITEM matter_engine_core_sources MatterEngine3/src/inotify_watcher.cpp)
endif()
if(NOT matter_engine_core_sources)
    message(FATAL_ERROR "engine-core.sources contains no MatterEngine3 core sources")
endif()

matter_read_manifest(
    "${CMAKE_SOURCE_DIR}/cmake/manifests/matter-surface.sources"
    matter_surface_manifest
    PLATFORM windows
)
set(matter_engine_surface_sources ${matter_surface_manifest})
list(FILTER matter_engine_surface_sources INCLUDE REGEX "^libs/MatterSurfaceLib/src/.*\\.(c|cpp)$")
if(NOT matter_engine_surface_sources)
    message(FATAL_ERROR "matter-surface.sources contains no MatterSurfaceLib sources")
endif()

add_library(matter_engine_core OBJECT ${matter_engine_core_sources})
matter_engine_include_directories(matter_engine_core PUBLIC)
target_compile_definitions(matter_engine_core PRIVATE
    PLATFORM_DESKTOP
    GRAPHICS_API_OPENGL_43
    MATTER_HAVE_SCRIPT_HOST
    MATTER_VULKAN_ONLY
)
if(BUILD_TESTING)
    target_compile_definitions(matter_engine_core PRIVATE
        MATTER_TEST_CACHE_VALIDATION_HOOK)
endif()
if(MATTER_ENABLE_PHYSX)
    target_compile_definitions(matter_engine_core PRIVATE MATTER_ENABLE_PHYSX)
endif()
target_link_libraries(matter_engine_core PUBLIC
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
)
matter_apply_project_defaults(matter_engine_core)
matter_apply_headless_config(matter_engine_core)
matter_require_headless_assertion_policy(matter_engine_core)

# MatterSurfaceLib contains C-linkage translation units as well as C++ mesh
# implementations. Keeping it as objects lets final consumers retain the full
# engine registration/algorithm surface without linker archive extraction.
add_library(matter_engine_surface_objects OBJECT ${matter_engine_surface_sources})
matter_engine_include_directories(matter_engine_surface_objects PRIVATE)
target_compile_definitions(matter_engine_surface_objects PRIVATE
    PLATFORM_DESKTOP
    GRAPHICS_API_OPENGL_43
    MATTER_VULKAN_ONLY
)
target_link_libraries(matter_engine_surface_objects PRIVATE
    matter_memory
    matter_math
    matter_spatial
    matter_mesh_charting
)
matter_apply_project_defaults(matter_engine_surface_objects)
matter_apply_headless_config(matter_engine_surface_objects)
matter_require_headless_assertion_policy(matter_engine_surface_objects)

add_library(matter_engine_headless STATIC
    $<TARGET_OBJECTS:matter_engine_core>
    $<TARGET_OBJECTS:matter_engine_surface_objects>
)
matter_engine_include_directories(matter_engine_headless PUBLIC)
target_compile_definitions(matter_engine_headless INTERFACE
    MATTER_HAVE_SCRIPT_HOST
)
if(BUILD_TESTING)
    target_compile_definitions(matter_engine_headless INTERFACE
        MATTER_TEST_CACHE_VALIDATION_HOOK)
endif()
target_link_libraries(matter_engine_headless PUBLIC
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
)
if(MATTER_ENABLE_PHYSX)
    target_link_libraries(matter_engine_headless PUBLIC matter_physx_adapter)
endif()
matter_apply_project_defaults(matter_engine_headless)

if(BUILD_TESTING)
    # Shared normal math is header-only; keep its correctness gate independent
    # of the engine archive, graphics/device setup and generated asset caches.
    add_executable(mat_math_tests MatterEngine3/tests/mat_math_tests.cpp)
    target_include_directories(mat_math_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/MatterEngine3/src"
        "${CMAKE_SOURCE_DIR}/MatterEngine3/tests"
    )
    target_link_libraries(mat_math_tests PRIVATE matter_math)
    matter_apply_project_defaults(mat_math_tests)
    matter_apply_test_assertion_policy(mat_math_tests)
    add_test(NAME mat_math_tests COMMAND mat_math_tests)
    set_tests_properties(mat_math_tests PROPERTIES LABELS cpu)
    set_property(GLOBAL APPEND PROPERTY MATTER_ENGINE_CPU_TARGETS mat_math_tests)

    function(matter_add_engine_cpu_test target)
        add_executable("${target}" ${ARGN})
        matter_engine_include_directories("${target}" PRIVATE)
        target_include_directories("${target}" PRIVATE
            "${CMAKE_SOURCE_DIR}/MatterEngine3/tests"
        )
        target_compile_definitions("${target}" PRIVATE
            PLATFORM_DESKTOP
            GRAPHICS_API_OPENGL_43
            MATTER_VULKAN_ONLY
        )
        target_link_libraries("${target}" PRIVATE matter_engine_headless)
        matter_apply_project_defaults("${target}")
        matter_apply_test_assertion_policy("${target}")
        add_test(NAME "${target}" COMMAND "${target}")
        set_tests_properties("${target}" PROPERTIES
            LABELS cpu
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/MatterEngine3/tests"
        )
        set_property(GLOBAL APPEND PROPERTY MATTER_ENGINE_CPU_TARGETS "${target}")
    endfunction()

    add_executable(compiler_portability_tests
        MatterEngine3/tests/compiler_portability_tests.cpp
    )
    matter_engine_include_directories(compiler_portability_tests PRIVATE)
    target_include_directories(compiler_portability_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/MatterEngine3/tests"
    )
    target_link_libraries(compiler_portability_tests PRIVATE matter_engine_headless)
    matter_apply_project_defaults(compiler_portability_tests)
    matter_apply_test_assertion_policy(compiler_portability_tests)
    get_target_property(compiler_portability_links
        compiler_portability_tests LINK_LIBRARIES)
    list(FIND compiler_portability_links matter_engine_headless
        compiler_portability_headless_index)
    if(compiler_portability_headless_index EQUAL -1)
        message(FATAL_ERROR
            "compiler_portability_tests must exercise matter_engine_headless: "
            "'${compiler_portability_links}'")
    endif()
    add_test(NAME compiler_portability_tests COMMAND compiler_portability_tests)
    set_tests_properties(compiler_portability_tests PROPERTIES
        LABELS cpu
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/MatterEngine3/tests"
    )
    add_executable(matter_engine_headless_consumer_tests
        MatterEngine3/tests/headless_consumer_interface_tests.cpp
    )
    target_link_libraries(matter_engine_headless_consumer_tests
        PRIVATE matter_engine_headless)
    matter_apply_project_defaults(matter_engine_headless_consumer_tests)
    matter_apply_test_assertion_policy(matter_engine_headless_consumer_tests)
    add_test(NAME matter_engine_headless_consumer_tests
        COMMAND matter_engine_headless_consumer_tests)
    set_tests_properties(matter_engine_headless_consumer_tests PROPERTIES
        LABELS compiler-policy
    )
    add_test(NAME compiler_format_negative_tests
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/cmake/tests/compiler_format_negative_tests.ps1"
    )
    set_tests_properties(compiler_format_negative_tests PROPERTIES
        LABELS compiler-policy
    )
    add_test(NAME compiler_return_address_gnu_tests
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/cmake/tests/compiler_return_address_gnu_tests.ps1"
    )
    set_tests_properties(compiler_return_address_gnu_tests PROPERTIES
        LABELS compiler-policy
    )
    add_custom_target(physx_dependency_contract_tests
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tools/tests/physx_dependency_contract_tests.ps1"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        VERBATIM
    )
    add_test(NAME physx_dependency_contract_tests
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tools/tests/physx_dependency_contract_tests.ps1"
    )
    set_tests_properties(physx_dependency_contract_tests PROPERTIES
        LABELS compiler-policy
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    add_executable(matter_mesh_allocator_policy_tests
        libs/MatterSurfaceLib/tests/mesh_allocator_policy_tests.cpp
        libs/MatterSurfaceLib/tests/mesh_allocator_graphical_probe.cpp
        libs/MatterSurfaceLib/tests/mesh_allocator_headless_probe.cpp
    )
    matter_engine_include_directories(matter_mesh_allocator_policy_tests PRIVATE)
    target_include_directories(matter_mesh_allocator_policy_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/MatterEngine3/tests"
    )
    matter_apply_project_defaults(matter_mesh_allocator_policy_tests)
    matter_apply_test_assertion_policy(matter_mesh_allocator_policy_tests)
    add_test(NAME matter_mesh_allocator_policy_tests
        COMMAND matter_mesh_allocator_policy_tests)
    set_tests_properties(matter_mesh_allocator_policy_tests PROPERTIES
        LABELS compiler-policy
    )

    matter_add_engine_cpu_test(world_definition_tests
        MatterEngine3/tests/world_definition_tests.cpp)
    matter_add_engine_cpu_test(local_light_index_tests
        MatterEngine3/tests/local_light_index_tests.cpp)
    matter_add_engine_cpu_test(resolve_cache_tests
        MatterEngine3/tests/resolve_cache_tests.cpp)
    matter_add_engine_cpu_test(material_registry_tests
        libs/MatterSurfaceLib/tests/material_registry_tests.cpp)
    matter_add_engine_cpu_test(terrain_collision_definition_tests
        MatterEngine3/tests/terrain_collision_definition_tests.cpp)
    matter_add_engine_cpu_test(terrain_collision_artifact_tests
        MatterEngine3/tests/terrain_collision_artifact_tests.cpp)
    matter_add_engine_cpu_test(shared_lib_tests
        MatterEngine3/tests/shared_lib_tests.cpp)
    matter_add_engine_cpu_test(river_network_tests
        MatterEngine3/tests/river_network_tests.cpp)
    matter_add_engine_cpu_test(river_geometry_tests
        MatterEngine3/tests/river_geometry_tests.cpp)
    matter_add_engine_cpu_test(authored_fluid_colliders_tests
        MatterEngine3/tests/authored_fluid_colliders_tests.cpp)
    matter_add_engine_cpu_test(river_section_graph_tests
        MatterEngine3/tests/river_section_graph_tests.cpp)
    matter_add_engine_cpu_test(spillway_handoff_tests
        MatterEngine3/tests/spillway_handoff_tests.cpp)
    matter_add_engine_cpu_test(dsl_determinism_tests
        MatterEngine3/tests/dsl_determinism_tests.cpp
        MatterEngine3/tests/field_probe.cpp)
    matter_add_engine_cpu_test(script_host_tests
        MatterEngine3/tests/script_host_tests.cpp)
    # The part-graph logic suite is host-free (its Baker/ModuleResolver are
    # fakes), so unlike part_graph_integration_tests.cpp -- which needs POSIX
    # unistd.h and stays a Make/MinGW target -- it builds under MSVC and can
    # gate the snapshot contract on the canonical build.
    matter_add_engine_cpu_test(part_graph_tests
        MatterEngine3/tests/part_graph_tests.cpp)
    matter_add_engine_cpu_test(partstore_tests
        MatterEngine3/tests/partstore_tests.cpp)
    # Also the only suite that exercises replace_file_atomic's Windows
    # transient-sharing retry, which POSIX rename never reaches.
    matter_add_engine_cpu_test(part_asset_v2_tests
        MatterEngine3/tests/part_asset_v2_tests.cpp)
    matter_add_engine_cpu_test(eval_world_tests
        MatterEngine3/tests/eval_world_tests.cpp)
    matter_add_engine_cpu_test(lod_distance_tests
        MatterEngine3/tests/lod_distance_tests.cpp)
    matter_add_engine_cpu_test(event_channel_tests
        MatterEngine3/tests/event_channel_tests.cpp)
    matter_add_engine_cpu_test(event_hub_tests
        MatterEngine3/tests/event_hub_tests.cpp)
    matter_add_engine_cpu_test(error_events_tests
        MatterEngine3/tests/error_events_tests.cpp)
    matter_add_engine_cpu_test(event_command_tests
        MatterEngine3/tests/event_command_tests.cpp)
    matter_add_engine_cpu_test(event_property_tests
        MatterEngine3/tests/event_property_tests.cpp)
    matter_add_engine_cpu_test(sector_streamer_tests
        MatterEngine3/tests/sector_streamer_tests.cpp)
    matter_add_engine_cpu_test(sector_streaming_coordinator_tests
        MatterEngine3/tests/sector_streaming_coordinator_tests.cpp)
    matter_add_engine_cpu_test(terrain_field_tests
        MatterEngine3/tests/terrain_field_tests.cpp)
    matter_add_engine_cpu_test(terrain_mesher_tests
        MatterEngine3/tests/terrain_mesher_tests.cpp)
    matter_add_engine_cpu_test(seam_weld_tests
        MatterEngine3/tests/seam_weld_tests.cpp)
    matter_add_engine_cpu_test(contour_seam_tests
        MatterEngine3/tests/contour_seam_tests.cpp)
    matter_add_engine_cpu_test(contour_mesh_tests
        MatterEngine3/tests/contour_mesh_tests.cpp)
    matter_add_engine_cpu_test(contour_engine_tests
        MatterEngine3/tests/contour_engine_tests.cpp)
    matter_add_engine_cpu_test(viewer_logic_tests
        MatterEngine3/tests/viewer_logic_tests.cpp
        MatterEditor/src/camera_controller.cpp
        MatterEditor/src/streaming_anchor_controller.cpp)
    target_link_libraries(viewer_logic_tests PRIVATE matter_glfw)
    matter_add_engine_cpu_test(props_tests
        MatterEngine3/tests/props_tests.cpp)
    matter_add_engine_cpu_test(physics_tests
        MatterEngine3/tests/physics_tests.cpp)
    matter_add_engine_cpu_test(ecs_tests
        MatterEngine3/tests/ecs_tests.cpp)
    matter_add_engine_cpu_test(character_controller_tests
        MatterEngine3/tests/character_controller_tests.cpp)
    matter_add_engine_cpu_test(character_walk_controller_tests
        MatterEngine3/tests/character_walk_controller_tests.cpp
        MatterEditor/src/character_walk_controller.cpp)
    matter_add_engine_cpu_test(terrain_collision_physics_tests
        MatterEngine3/tests/terrain_collision_physics_tests.cpp)
    matter_add_engine_cpu_test(terrain_collision_session_tests
        MatterEngine3/tests/terrain_collision_session_tests.cpp)
    matter_add_engine_cpu_test(river_float_system_tests
        MatterEngine3/tests/river_float_system_tests.cpp)
    matter_add_engine_cpu_test(scene_registry_tests
        MatterEngine3/tests/scene_registry_tests.cpp)
    matter_add_engine_cpu_test(simulation_control_tests
        MatterEngine3/tests/simulation_control_tests.cpp)
    matter_add_engine_cpu_test(scene_tracker_tests
        MatterEngine3/tests/scene_tracker_tests.cpp)
    matter_add_engine_cpu_test(entity_recipe_tests
        MatterEngine3/tests/entity_recipe_tests.cpp)
    matter_add_engine_cpu_test(dynamic_instance_slots_tests
        MatterEngine3/tests/dynamic_instance_slots_tests.cpp
        MatterEngine3/src/render/dynamic_instance_slots.cpp)
    matter_add_engine_cpu_test(dynamic_scene_bridge_tests
        MatterEngine3/tests/dynamic_scene_bridge_tests.cpp
        MatterEngine3/src/ecs/dynamic_scene_bridge.cpp
        MatterEngine3/src/render/dynamic_instance_slots.cpp)
    matter_add_engine_cpu_test(animation_rigid_bridge_tests
        MatterEngine3/tests/animation_rigid_bridge_tests.cpp)
    matter_add_engine_cpu_test(gpu_visual_mesher_cpu_tests
        MatterEngine3/tests/gpu_visual_mesher_cpu_tests.cpp)
    matter_add_engine_cpu_test(water_mesh_animation_capture_tests
        MatterEngine3/tests/water_mesh_animation_capture_tests.cpp)
    matter_add_engine_cpu_test(water_mesh_animation_tests
        MatterEngine3/tests/water_mesh_animation_tests.cpp)
    matter_add_engine_cpu_test(water_mesh_animation_artifact_tests
        MatterEngine3/tests/water_mesh_animation_artifact_tests.cpp)
    matter_add_engine_cpu_test(water_boundary_animation_source_tests
        MatterEngine3/tests/water_boundary_animation_source_tests.cpp)
    matter_add_engine_cpu_test(water_mesh_animation_playback_tests
        MatterEngine3/tests/water_mesh_animation_playback_tests.cpp)
    matter_add_engine_cpu_test(river_presentation_field_tests
        MatterEngine3/tests/river_presentation_field_tests.cpp)
    matter_add_engine_cpu_test(river_runtime_tests
        MatterEngine3/tests/river_runtime_tests.cpp)
    matter_add_engine_cpu_test(hydrology_artifact_tests
        MatterEngine3/tests/hydrology_artifact_tests.cpp)
    matter_add_engine_cpu_test(hydrology_network_artifact_tests
        MatterEngine3/tests/hydrology_network_artifact_tests.cpp)
    matter_add_engine_cpu_test(river_section_coordinator_tests
        MatterEngine3/tests/river_section_coordinator_tests.cpp)
    matter_add_engine_cpu_test(hydrology_handoff_products_tests
        MatterEngine3/tests/hydrology_handoff_products_tests.cpp)
    matter_add_engine_cpu_test(physx_adapter_contract_tests
        MatterEngine3/tests/physx_adapter_contract_tests.cpp)
    target_compile_definitions(physx_adapter_contract_tests PRIVATE
        MATTER_LOCAL_PROVIDER_FLUID_PATH_TEST)
    matter_add_engine_cpu_test(async_bake_tests
        MatterEngine3/tests/async_bake_tests.cpp)
    if(MATTER_ENABLE_PHYSX)
        add_executable(physx_fluid_integration_tests
            MatterEngine3/tests/physx_fluid_integration_tests.cpp)
        matter_engine_include_directories(
            physx_fluid_integration_tests PRIVATE)
        target_include_directories(physx_fluid_integration_tests PRIVATE
            "${CMAKE_SOURCE_DIR}/MatterEngine3/tests")
        target_link_libraries(physx_fluid_integration_tests PRIVATE
            matter_engine_viewer_objects
            matter_physx_adapter
            matter_glfw
            matter_vulkan_sdk
            gdi32 winmm user32 shell32 ws2_32 dbghelp)
        target_compile_definitions(physx_fluid_integration_tests PRIVATE
            MATTER_HAVE_SCRIPT_HOST MATTER_VULKAN_VIEWER MATTER_VULKAN_ONLY)
        matter_apply_project_defaults(physx_fluid_integration_tests)
        matter_apply_test_assertion_policy(physx_fluid_integration_tests)
        add_test(NAME physx_fluid_integration_tests
            COMMAND physx_fluid_integration_tests)
        set_tests_properties(physx_fluid_integration_tests PROPERTIES
            LABELS "gpu;physx"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/MatterEngine3/tests")
        matter_stage_physx_runtime(physx_fluid_integration_tests)
    endif()
    matter_add_engine_cpu_test(gpu_water_render_tests
        MatterEngine3/tests/gpu_water_render_tests.cpp
        MatterEngine3/src/render/gpu_meshing/water_scene_part.cpp)
    matter_add_engine_cpu_test(gpu_water_animation_render_tests
        MatterEngine3/tests/gpu_water_animation_render_tests.cpp
        MatterEngine3/src/render/water_animation_gpu.cpp)
    matter_add_engine_cpu_test(water_field_vk_tests
        MatterEngine3/tests/water_field_vk_tests.cpp)
    matter_add_engine_cpu_test(water_surface_reference_tests
        MatterEngine3/tests/water_surface_reference_tests.cpp)
    matter_add_engine_cpu_test(water_forward_reference_tests
        MatterEngine3/tests/water_forward_reference_tests.cpp)
    matter_add_engine_cpu_test(shader_source_tests
        MatterEngine3/tests/shader_source_tests.cpp)

    add_custom_target(matter_character_integration_tests)
    add_dependencies(matter_character_integration_tests
        character_controller_tests
        character_walk_controller_tests
        ecs_tests
        physics_tests
        terrain_collision_definition_tests
        terrain_collision_artifact_tests
        terrain_collision_physics_tests
        terrain_collision_session_tests
        river_float_system_tests
        scene_registry_tests
        simulation_control_tests
        scene_tracker_tests
        entity_recipe_tests
        viewer_logic_tests)

    get_property(matter_engine_cpu_targets GLOBAL PROPERTY MATTER_ENGINE_CPU_TARGETS)
    add_custom_target(matter_engine_cpu_tests)
    add_dependencies(matter_engine_cpu_tests
        compiler_portability_tests
        matter_engine_headless_consumer_tests
        matter_mesh_allocator_policy_tests
        physx_dependency_contract_tests
        ${matter_engine_cpu_targets}
    )
endif()
