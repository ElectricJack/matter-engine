include_guard(GLOBAL)

# Measured >60 s in the retained 38100939715 Windows run. Keep these complete
# fixtures in PR and promotion instead of shortening their inputs or assertions.
set(matter_ci_slow_cpu_tests world_definition_tests viewer_logic_tests
    sector_streamer_tests part_flatten_tests script_host_tests)

# Exact-name conversions are reviewed alongside their unchanged command semantics.
# New compiler/shader contracts require an explicit disposition, not a broad label.
set(matter_ci_manifest_path "${CMAKE_CURRENT_LIST_DIR}/ci-checks.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${matter_ci_manifest_path}")
file(READ "${matter_ci_manifest_path}" matter_ci_manifest)
foreach(section source_checks behavioral_policy_checks)
    string(JSON count LENGTH "${matter_ci_manifest}" ${section})
    math(EXPR last "${count} - 1")
    foreach(index RANGE ${last})
        string(JSON name GET "${matter_ci_manifest}" ${section} ${index} name)
        list(APPEND matter_ci_${section} "${name}")
    endforeach()
endforeach()

function(matter_label_ci_tests directory)
    get_property(tests DIRECTORY "${directory}" PROPERTY TESTS)
    foreach(test IN LISTS tests)
        get_property(labels TEST "${test}" DIRECTORY "${directory}" PROPERTY LABELS)
        # Keep the complete historical inventory for promotion. Packaging and
        # shader/compiler source checks are separate from the PR behavioral run.
        if(("compiler-policy" IN_LIST labels OR "shader" IN_LIST labels) AND
           NOT test IN_LIST matter_ci_source_checks AND
           NOT test IN_LIST matter_ci_behavioral_policy_checks)
            message(FATAL_ERROR "Unclassified source/build policy check '${test}'; update ci-checks.json")
        endif()
        set_property(GLOBAL APPEND PROPERTY MATTER_CI_REGISTERED_TESTS "${test}")
        if(NOT "package" IN_LIST labels AND NOT test IN_LIST matter_ci_source_checks)
            set_property(TEST "${test}" DIRECTORY "${directory}" APPEND PROPERTY LABELS ci-pr)
            set_property(TEST "${test}" DIRECTORY "${directory}" APPEND PROPERTY LABELS ci-full)
        elseif(NOT "package" IN_LIST labels)
            set_property(TEST "${test}" DIRECTORY "${directory}" APPEND PROPERTY LABELS ci-source-lint)
            if(TARGET "${test}")
                set_property(GLOBAL APPEND PROPERTY MATTER_CI_SOURCE_LINT_TARGETS "${test}")
            endif()
        else()
            # Package behavior uses synthetic scratch outputs in the retained
            # tests and only runs on promotion.
            set_property(TEST "${test}" DIRECTORY "${directory}" APPEND PROPERTY LABELS ci-full)
        endif()
        if("vulkan" IN_LIST labels OR "gpu" IN_LIST labels)
            # These tests share one device and several smoke output paths.
            set_property(TEST "${test}" DIRECTORY "${directory}" PROPERTY RESOURCE_LOCK matter-vulkan)
            continue()
        endif()
        if(test IN_LIST matter_ci_slow_cpu_tests)
            continue()
        endif()
        if(test IN_LIST matter_ci_source_checks OR "package" IN_LIST labels)
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

string(JSON matter_ci_custom_count LENGTH "${matter_ci_manifest}" custom_checks)
math(EXPR matter_ci_custom_last "${matter_ci_custom_count} - 1")
foreach(index RANGE ${matter_ci_custom_last})
    string(JSON target GET "${matter_ci_manifest}" custom_checks ${index} target)
    string(JSON script GET "${matter_ci_manifest}" custom_checks ${index} command_suffix)
    string(JSON argc LENGTH "${matter_ci_manifest}" custom_checks ${index} arguments)
    set(arguments)
    if(argc GREATER 0)
        math(EXPR last "${argc} - 1")
        foreach(arg RANGE ${last})
            string(JSON value GET "${matter_ci_manifest}" custom_checks ${index} arguments ${arg})
            list(APPEND arguments "${value}")
        endforeach()
    endif()
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${script}")
        message(FATAL_ERROR "Required CI qualifier '${script}' is missing")
    endif()
    add_custom_target("${target}"
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/${script}" ${arguments}
        VERBATIM)
endforeach()
add_test(NAME ci_test_report_tests
    COMMAND "${MATTER_PYTHON_EXECUTABLE}" ${matter_python_arguments}
        "${CMAKE_SOURCE_DIR}/tools/tests/test_ci_test_report.py")
set_tests_properties(ci_test_report_tests PROPERTIES LABELS python)
add_test(NAME ci_source_lint_tests
    COMMAND "${MATTER_PYTHON_EXECUTABLE}" ${matter_python_arguments}
        "${CMAKE_SOURCE_DIR}/tools/tests/test_ci_source_lint.py")
set_tests_properties(ci_source_lint_tests PROPERTIES LABELS python)
add_test(NAME glslc_depfile_tests
    COMMAND "${MATTER_PYTHON_EXECUTABLE}" ${matter_python_arguments}
        "${CMAKE_SOURCE_DIR}/tools/tests/test_glslc_depfile.py")
set_tests_properties(glslc_depfile_tests PROPERTIES LABELS python)
set_tests_properties(shader_rebuild_tests PROPERTIES RESOURCE_LOCK matter-shader-fixture)
matter_label_ci_tests("${CMAKE_SOURCE_DIR}")
get_property(matter_ci_registered GLOBAL PROPERTY MATTER_CI_REGISTERED_TESTS)
foreach(test IN LISTS matter_ci_source_checks matter_ci_behavioral_policy_checks)
    if(NOT test IN_LIST matter_ci_registered)
        message(FATAL_ERROR "Required CI contract '${test}' is not registered")
    endif()
endforeach()
get_property(matter_ci_lint_targets GLOBAL PROPERTY MATTER_CI_SOURCE_LINT_TARGETS)
if(matter_ci_lint_targets)
    add_dependencies(matter_ci_lint ${matter_ci_lint_targets})
endif()
get_property(matter_ci_fast_targets GLOBAL PROPERTY MATTER_CI_FAST_TARGETS)
add_custom_target(matter_ci_fast DEPENDS ${matter_ci_fast_targets})
