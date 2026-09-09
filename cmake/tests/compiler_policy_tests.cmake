cmake_minimum_required(VERSION 3.25)

set(test_root "${CMAKE_CURRENT_LIST_DIR}/../../MatterEditor/build/cmake/compiler-policy-test")
set(source_dir "${test_root}/source")
set(build_dir "${test_root}/build")
file(REMOVE_RECURSE "${test_root}")
file(MAKE_DIRECTORY "${source_dir}")

file(WRITE "${source_dir}/probe.c" "int matter_c_probe(void) { return 0; }\n")
file(WRITE "${source_dir}/probe.cpp" "int matter_cpp_probe() { return 0; }\n")
file(WRITE "${source_dir}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.25)
project(matter_compiler_policy_probe LANGUAGES C CXX)

get_filename_component(matter_repository_root "${MATTER_REPOSITORY_ROOT}" REALPATH)
include("${matter_repository_root}/cmake/MatterCompiler.cmake")

add_library(matter_compiler_policy_probe STATIC probe.c probe.cpp)
matter_apply_project_defaults(matter_compiler_policy_probe)

add_library(matter_interface_policy_probe INTERFACE)
matter_apply_project_defaults(matter_interface_policy_probe)

add_executable(matter_test_policy_probe probe.c)
matter_apply_project_defaults(matter_test_policy_probe)
matter_apply_test_assertion_policy(matter_test_policy_probe)

function(require_target_property property expected)
    get_target_property(actual matter_compiler_policy_probe "${property}")
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR
            "matter_compiler_policy_probe ${property}: expected '${expected}', got '${actual}'")
    endif()
endfunction()

function(require_compile_option expected)
    get_target_property(actual matter_compiler_policy_probe COMPILE_OPTIONS)
    list(FIND actual "${expected}" option_index)
    if(option_index EQUAL -1)
        message(FATAL_ERROR
            "matter_compiler_policy_probe COMPILE_OPTIONS is missing '${expected}': '${actual}'")
    endif()
endfunction()

require_target_property(C_STANDARD "17")
require_target_property(C_STANDARD_REQUIRED "ON")
require_target_property(C_EXTENSIONS "OFF")
require_target_property(CXX_STANDARD "17")
require_target_property(CXX_STANDARD_REQUIRED "ON")
require_target_property(CXX_EXTENSIONS "OFF")
require_target_property(MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

require_compile_option("/utf-8")
require_compile_option("/W4")
require_compile_option("$<$<COMPILE_LANGUAGE:CXX>:/permissive->")
require_compile_option("$<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>")
require_compile_option("$<$<COMPILE_LANGUAGE:CXX>:/EHsc>")

get_target_property(interface_features matter_interface_policy_probe INTERFACE_COMPILE_FEATURES)
foreach(required_feature IN ITEMS c_std_17 cxx_std_17)
    list(FIND interface_features "${required_feature}" feature_index)
    if(feature_index EQUAL -1)
        message(FATAL_ERROR
            "matter_interface_policy_probe is missing usage feature '${required_feature}': '${interface_features}'")
    endif()
endforeach()

get_target_property(interface_options matter_interface_policy_probe INTERFACE_COMPILE_OPTIONS)
if(interface_options)
    message(FATAL_ERROR
        "matter_interface_policy_probe leaked project compile options: '${interface_options}'")
endif()

get_target_property(interface_runtime matter_interface_policy_probe MSVC_RUNTIME_LIBRARY)
if(interface_runtime)
    message(FATAL_ERROR
        "matter_interface_policy_probe leaked project runtime policy: '${interface_runtime}'")
endif()

get_target_property(test_options matter_test_policy_probe COMPILE_OPTIONS)
list(FIND test_options "/UNDEBUG" undefine_option_index)
if(NOT undefine_option_index EQUAL -1)
    message(FATAL_ERROR "matter_test_policy_probe uses conflicting /UNDEBUG: '${test_options}'")
endif()
set(expected_force_include
    "/FI${matter_repository_root}/cmake/MatterTestAssertions.h")
list(FIND test_options "${expected_force_include}" force_include_index)
if(force_include_index EQUAL -1)
    message(FATAL_ERROR
        "matter_test_policy_probe is missing '${expected_force_include}': '${test_options}'")
endif()
]=])

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${source_dir}"
        -B "${build_dir}"
        -G Ninja
        "-DMATTER_REPOSITORY_ROOT=${CMAKE_CURRENT_LIST_DIR}/../.."
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_stdout
    ERROR_VARIABLE configure_stderr
)

if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR
        "Compiler policy probe configure failed.\n"
        "stdout:\n${configure_stdout}\n"
        "stderr:\n${configure_stderr}")
endif()

message(STATUS "Matter compiler policy target properties are correct")
