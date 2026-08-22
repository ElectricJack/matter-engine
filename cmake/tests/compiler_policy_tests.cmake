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

include("${MATTER_REPOSITORY_ROOT}/cmake/MatterCompiler.cmake")

add_library(matter_compiler_policy_probe STATIC probe.c probe.cpp)
matter_apply_project_defaults(matter_compiler_policy_probe)

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
