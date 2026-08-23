include_guard(GLOBAL)

set(MATTER_DIST_PROJECT "world_demo" CACHE STRING
    "Project directory staged by the matter_dist target")
set(matter_dist_project_source
    "${CMAKE_SOURCE_DIR}/projects/${MATTER_DIST_PROJECT}")
if(NOT IS_DIRECTORY "${matter_dist_project_source}")
    message(FATAL_ERROR
        "MATTER_DIST_PROJECT does not exist: ${matter_dist_project_source}")
endif()

execute_process(
    COMMAND git rev-parse HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE matter_source_revision
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE matter_git_result
)
if(NOT matter_git_result EQUAL 0 OR matter_source_revision STREQUAL "")
    message(FATAL_ERROR "Could not record the source revision for matter_dist")
endif()

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
        --compiler-version "${CMAKE_CXX_COMPILER_VERSION}"
        --msvc-tools "14.44.35207"
        --windows-sdk "10.0.26100.0"
        --vulkan-sdk "1.4.357.0"
        --revision "${matter_source_revision}"
        --autoremesher "${matter_dist_autoremesher}"
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tools/check-windows-msvc-package.ps1"
        -DistPath "${matter_dist_root}"
    DEPENDS matter_editor
        "${CMAKE_SOURCE_DIR}/tools/stage-windows-msvc-package.py"
        "${CMAKE_SOURCE_DIR}/tools/check-windows-msvc-package.ps1"
    USES_TERMINAL
    COMMENT "Staging and verifying the self-contained MSVC editor package"
)

if(BUILD_TESTING)
    add_test(NAME windows_package_tests
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tools/tests/windows_package_tests.ps1"
            -RepositoryRoot "${CMAKE_SOURCE_DIR}"
            -EditorPath "$<TARGET_FILE:matter_editor>"
    )
    set_tests_properties(windows_package_tests PROPERTIES LABELS "editor;package")
endif()
