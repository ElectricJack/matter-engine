include_guard(GLOBAL)

include("${CMAKE_SOURCE_DIR}/cmake/MatterPackageValidation.cmake")

set(MATTER_DIST_PROJECT "world_demo" CACHE STRING
    "Project directory staged by the matter_dist target")
matter_validate_dist_project_name("${MATTER_DIST_PROJECT}")
set(matter_dist_project_source
    "${CMAKE_SOURCE_DIR}/projects/${MATTER_DIST_PROJECT}")
if(NOT IS_DIRECTORY "${matter_dist_project_source}")
    message(FATAL_ERROR
        "MATTER_DIST_PROJECT does not exist: ${matter_dist_project_source}")
endif()

if(NOT CMAKE_CXX_COMPILER_VERSION MATCHES "^19\\.44\\.35211(\\.0)?$")
    message(FATAL_ERROR
        "matter_dist requires pinned MSVC compiler 19.44.35211, got ${CMAKE_CXX_COMPILER_VERSION}")
endif()
set(matter_dist_compiler_version "19.44.35211")

set(matter_dist_root
    "${CMAKE_SOURCE_DIR}/MatterEditor/build/dist/${MATTER_DIST_PROJECT}")
set(matter_dist_autoremesher false)
if(MATTER_ENABLE_AUTOREMESHER)
    set(matter_dist_autoremesher true)
endif()

add_custom_target(matter_dist
    COMMAND "${MATTER_PYTHON_EXECUTABLE}" ${matter_python_arguments}
        "${CMAKE_SOURCE_DIR}/tools/stage-windows-msvc-package.py"
        --root "${CMAKE_SOURCE_DIR}"
        --editor "$<TARGET_FILE:matter_editor>"
        --pdb "$<TARGET_PDB_FILE:matter_editor>"
        --dist "${matter_dist_root}"
        --project "${MATTER_DIST_PROJECT}"
        --config "${CMAKE_BUILD_TYPE}"
        --compiler-version "${matter_dist_compiler_version}"
        --msvc-tools "14.44.35207"
        --windows-sdk "10.0.26100.0"
        --vulkan-sdk "1.4.357.0"
        --autoremesher "${matter_dist_autoremesher}"
        --dependency-library "autoremesher_core=$<TARGET_FILE:matter_autoremesher>"
        --dependency-library "bc7enc=$<TARGET_FILE:matter_bc7enc>"
        --dependency-library "box3d=$<TARGET_FILE:matter_box3d>"
        --dependency-library "flecs=$<TARGET_FILE:matter_flecs>"
        --dependency-library "glfw=$<TARGET_FILE:matter_glfw>"
        --dependency-library "dear_imgui=$<TARGET_FILE:matter_imgui>"
        --dependency-library "imguizmo=$<TARGET_FILE:matter_imguizmo>"
        --dependency-library "ozz_animation=$<TARGET_FILE:matter_ozz_base>"
        --dependency-library "ozz_animation=$<TARGET_FILE:matter_ozz_animation>"
        --dependency-library "ozz_animation=$<TARGET_FILE:matter_ozz_offline>"
        --dependency-library "quickjs_ng=$<TARGET_FILE:matter_quickjs>"
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tools/check-windows-msvc-package.ps1"
        -DistPath "${matter_dist_root}"
    DEPENDS matter_editor ${matter_third_party_targets}
        "${CMAKE_SOURCE_DIR}/tools/stage-windows-msvc-package.py"
        "${CMAKE_SOURCE_DIR}/tools/check-windows-msvc-package.ps1"
    USES_TERMINAL
    COMMENT "Staging and verifying the self-contained MSVC editor package"
)

if(BUILD_TESTING)
    add_test(NAME package_project_name_tests
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/cmake/tests/package_project_name_tests.ps1"
            -RepositoryRoot "${CMAKE_SOURCE_DIR}"
            -CMakePath "${CMAKE_COMMAND}"
    )
    set_tests_properties(package_project_name_tests PROPERTIES LABELS "editor;package;safety")

    add_test(NAME windows_package_stage_tests
        COMMAND "${MATTER_PYTHON_EXECUTABLE}" ${matter_python_arguments}
            "${CMAKE_SOURCE_DIR}/tools/tests/test_windows_package_stage.py"
    )
    set_tests_properties(windows_package_stage_tests PROPERTIES LABELS "editor;package;safety")

    add_test(NAME windows_package_tests
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tools/tests/windows_package_tests.ps1"
            -RepositoryRoot "${CMAKE_SOURCE_DIR}"
            -EditorPath "$<TARGET_FILE:matter_editor>"
    )
    set_tests_properties(windows_package_tests PROPERTIES LABELS "editor;package")
endif()
