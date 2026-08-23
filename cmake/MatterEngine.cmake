include_guard(GLOBAL)

function(matter_engine_include_directories target visibility)
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
matter_apply_project_defaults(matter_engine_headless)

if(BUILD_TESTING)
    function(matter_add_engine_cpu_test target)
        add_executable("${target}" ${ARGN})
        matter_engine_include_directories("${target}" PRIVATE)
        target_include_directories("${target}" PRIVATE
            "${CMAKE_SOURCE_DIR}/MatterEngine3/tests"
        )
        target_compile_definitions("${target}" PRIVATE
            PLATFORM_DESKTOP
            GRAPHICS_API_OPENGL_43
            MATTER_HAVE_SCRIPT_HOST
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
    matter_add_engine_cpu_test(script_host_tests
        MatterEngine3/tests/script_host_tests.cpp)
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

    get_property(matter_engine_cpu_targets GLOBAL PROPERTY MATTER_ENGINE_CPU_TARGETS)
    add_custom_target(matter_engine_cpu_tests)
    add_dependencies(matter_engine_cpu_tests
        compiler_portability_tests
        matter_mesh_allocator_policy_tests
        ${matter_engine_cpu_targets}
    )
endif()
