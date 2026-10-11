include_guard(GLOBAL)

# Measured >60 s in the retained 38100939715 Windows run. Keep these complete
# fixtures in staging/main instead of shortening their inputs or assertions.
set(matter_ci_slow_cpu_tests world_definition_tests viewer_logic_tests
    sector_streamer_tests part_flatten_tests script_host_tests)

function(matter_label_ci_tests directory)
    get_property(tests DIRECTORY "${directory}" PROPERTY TESTS)
    foreach(test IN LISTS tests)
        get_property(labels TEST "${test}" DIRECTORY "${directory}" PROPERTY LABELS)
        set_property(TEST "${test}" DIRECTORY "${directory}" APPEND PROPERTY LABELS ci-full)
        if("vulkan" IN_LIST labels OR "gpu" IN_LIST labels)
            # These tests share one device and several smoke output paths.
            set_property(TEST "${test}" DIRECTORY "${directory}" PROPERTY RESOURCE_LOCK matter-vulkan)
            continue()
        endif()
        if(test IN_LIST matter_ci_slow_cpu_tests)
            continue()
        endif()
        if("cpu" IN_LIST labels OR "foundation" IN_LIST labels OR
           "python" IN_LIST labels OR "compiler-policy" IN_LIST labels OR
           "third_party" IN_LIST labels OR "safety" IN_LIST labels OR
           test STREQUAL "shader_input_list_tests")
            set_property(TEST "${test}" DIRECTORY "${directory}" APPEND PROPERTY LABELS ci-fast)
            if(TARGET "${test}")
                set_property(GLOBAL APPEND PROPERTY MATTER_CI_FAST_TARGETS "${test}")
            endif()
        endif()
    endforeach()
    get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(child IN LISTS children)
        matter_label_ci_tests("${child}")
    endforeach()
endfunction()

add_test(NAME windows_ci_contract_tests
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tools/tests/windows_ci_contract_tests.ps1")
set_tests_properties(windows_ci_contract_tests PROPERTIES LABELS "compiler-policy")
add_test(NAME ci_test_report_tests
    COMMAND "${MATTER_PYTHON_EXECUTABLE}" ${matter_python_arguments}
        "${CMAKE_SOURCE_DIR}/tools/tests/test_ci_test_report.py")
set_tests_properties(ci_test_report_tests PROPERTIES LABELS python)
add_test(NAME glslc_depfile_tests
    COMMAND "${MATTER_PYTHON_EXECUTABLE}" ${matter_python_arguments}
        "${CMAKE_SOURCE_DIR}/tools/tests/test_glslc_depfile.py")
set_tests_properties(glslc_depfile_tests PROPERTIES LABELS python)
add_test(NAME shader_external_build_tests
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/cmake/tests/shader_rebuild_test.ps1" -ExternalBuild)
set_tests_properties(shader_external_build_tests PROPERTIES
    LABELS "compiler-policy" RESOURCE_LOCK matter-shader-fixture)
set_tests_properties(shader_rebuild_tests PROPERTIES RESOURCE_LOCK matter-shader-fixture)
matter_label_ci_tests("${CMAKE_SOURCE_DIR}")
get_property(matter_ci_fast_targets GLOBAL PROPERTY MATTER_CI_FAST_TARGETS)
add_custom_target(matter_ci_fast DEPENDS ${matter_ci_fast_targets})
