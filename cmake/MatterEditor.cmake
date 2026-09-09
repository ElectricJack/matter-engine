include_guard(GLOBAL)

matter_read_manifest(
    "${CMAKE_SOURCE_DIR}/cmake/manifests/editor.sources"
    matter_editor_manifest
    PLATFORM windows
)
set(matter_editor_sources ${matter_editor_manifest})
list(FILTER matter_editor_sources
    INCLUDE REGEX "^MatterEditor/src/.*\\.cpp$")
set(matter_editor_sources_unique ${matter_editor_sources})
list(REMOVE_DUPLICATES matter_editor_sources_unique)
list(LENGTH matter_editor_sources matter_editor_source_count)
list(LENGTH matter_editor_sources_unique matter_editor_unique_source_count)
if(NOT matter_editor_source_count EQUAL 43 OR
        NOT matter_editor_unique_source_count EQUAL 43)
    message(FATAL_ERROR
        "editor.sources must provide exactly 43 unique editor C++ sources; "
        "count=${matter_editor_source_count}, unique=${matter_editor_unique_source_count}")
endif()

add_executable(matter_editor WIN32 ${matter_editor_sources})
matter_engine_include_directories(matter_editor PRIVATE)
target_include_directories(matter_editor PRIVATE
    "${CMAKE_SOURCE_DIR}/MatterEditor/src"
    "${matter_vulkan_include}"
    "${CMAKE_SOURCE_DIR}/third_party/imgui"
    "${CMAKE_SOURCE_DIR}/third_party/imgui/backends"
    "${CMAKE_SOURCE_DIR}/third_party/ImGuizmo"
    "${CMAKE_SOURCE_DIR}/third_party/raylib/src/external/glfw/include"
)
# Headers owned by source-built dependencies retain their own warning policy.
# /WX below applies to Matter's 43 editor translation units, while MSVC treats
# these include roots as external at a narrowly suppressed warning level.
target_include_directories(matter_editor SYSTEM PRIVATE
    "${matter_vulkan_include}"
    "${CMAKE_SOURCE_DIR}/third_party/quickjs-ng"
    "${CMAKE_SOURCE_DIR}/third_party/flecs"
    "${CMAKE_SOURCE_DIR}/third_party/imgui"
    "${CMAKE_SOURCE_DIR}/third_party/imgui/backends"
    "${CMAKE_SOURCE_DIR}/third_party/ImGuizmo"
    "${CMAKE_SOURCE_DIR}/third_party/raylib/src"
    "${CMAKE_SOURCE_DIR}/third_party/raylib/src/external/glfw/include"
    "${CMAKE_SOURCE_DIR}/third_party/box3d/include"
    "${CMAKE_SOURCE_DIR}/third_party/bc7enc"
    "${CMAKE_SOURCE_DIR}/third_party/ozz-animation/include"
    "${CMAKE_SOURCE_DIR}/third_party/Vulkan-Headers/include"
)
target_compile_definitions(matter_editor PRIVATE
    PLATFORM_DESKTOP
    NDEBUG
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    NOGDI
    NOUSER
    MATTER_HAVE_SCRIPT_HOST
    MATTER_VULKAN_VIEWER
    MATTER_VULKAN_ONLY
    MATTER_HAVE_STREAMLINE=0
    VK_USE_PLATFORM_WIN32_KHR
    _CRT_SECURE_NO_WARNINGS
)
if(MATTER_ENABLE_AUTOREMESHER)
    target_compile_definitions(matter_editor PRIVATE MATTER_HAVE_AUTOREMESHER)
endif()
target_link_libraries(matter_editor PRIVATE
    matter_engine_viewer_objects
    matter_imgui
    matter_imguizmo
    matter_glfw
    matter_box3d
    matter_ozz_offline
    matter_autoremesher
    matter_quickjs
    matter_flecs
    matter_bc7enc
    matter_vulkan_sdk
    gdi32
    winmm
    user32
    shell32
    ws2_32
    dbghelp
)
matter_apply_project_defaults(matter_editor)
target_compile_options(matter_editor PRIVATE /WX)
if(MATTER_ENABLE_PHYSX)
    matter_stage_physx_runtime(matter_editor)
endif()
set_target_properties(matter_editor PROPERTIES
    OUTPUT_NAME editor
    WIN32_EXECUTABLE TRUE
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/MatterEditor/build/windows-msvc"
    PDB_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/MatterEditor/build/windows-msvc"
)

# Keep main() as the single portable entry point while still producing a
# window-subsystem executable (no console window for the editor). Redirected
# standard handles remain usable by the documented automation/control surface.
target_link_options(matter_editor PRIVATE /ENTRY:mainCRTStartup)

add_custom_target(editor DEPENDS matter_editor)

if(BUILD_TESTING)
    add_executable(agent_protocol_tests
        MatterEditor/tests/test_agent_protocol.cpp
        MatterEditor/src/agent_protocol.cpp
        MatterEngine3/src/util/json_doc.cpp
    )
    target_include_directories(agent_protocol_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/MatterEditor/src"
        "${CMAKE_SOURCE_DIR}/MatterEngine3/include"
    )
    matter_apply_project_defaults(agent_protocol_tests)
    matter_apply_test_assertion_policy(agent_protocol_tests)
    add_test(NAME agent_protocol_tests COMMAND agent_protocol_tests)
    set_tests_properties(agent_protocol_tests PROPERTIES LABELS "editor;cpu")

    add_executable(scene_inventory_tests
        MatterEditor/tests/test_scene_inventory.cpp
        MatterEditor/src/scene_inventory.cpp
        MatterEditor/src/agent_protocol.cpp
        MatterEngine3/src/util/json_doc.cpp
    )
    target_include_directories(scene_inventory_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/MatterEditor/src"
        "${CMAKE_SOURCE_DIR}/MatterEngine3/include"
    )
    matter_apply_project_defaults(scene_inventory_tests)
    matter_apply_test_assertion_policy(scene_inventory_tests)
    add_test(NAME scene_inventory_tests COMMAND scene_inventory_tests)
    set_tests_properties(scene_inventory_tests PROPERTIES LABELS "editor;cpu")

    add_test(NAME matter_agent_client_tests
        COMMAND "${MATTER_PYTHON_EXECUTABLE}" ${matter_python_arguments}
            "${CMAKE_SOURCE_DIR}/tools/tests/test_matter_agent.py"
    )
    set_tests_properties(matter_agent_client_tests PROPERTIES
        LABELS "editor;python")

    add_test(NAME windows_build_wrapper_contract
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/cmake/tests/build_wrapper_contract_tests.ps1"
            -RepositoryRoot "${CMAKE_SOURCE_DIR}"
    )
    set_tests_properties(windows_build_wrapper_contract PROPERTIES
        LABELS "editor;compiler-policy")

    add_test(NAME editor_registration_census
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/cmake/tests/editor_registration_census.ps1"
    )
    set_tests_properties(editor_registration_census PROPERTIES LABELS editor)
endif()
