cmake_minimum_required(VERSION 3.25)
project(MatterScriptingTests LANGUAGES C CXX)
include(CTest)
get_filename_component(_repo "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
# A separate Linux/native Windows graph; the product root remains Windows-only.
add_subdirectory("${_repo}/MatterEngine3/src/script/portable_host" portable_host)
add_subdirectory("${_repo}/packages/matter-authoring" authoring)
add_subdirectory("${_repo}/bindings/geometry-c" binding)
add_executable(matter_module_snapshot_tests source_snapshot_tests.cpp
    "${_repo}/MatterEngine3/src/module_resolver.cpp")
target_compile_features(matter_module_snapshot_tests PRIVATE cxx_std_17)
target_include_directories(matter_module_snapshot_tests PRIVATE "${_repo}/MatterEngine3/src")
add_test(NAME matter_module_source_snapshot COMMAND matter_module_snapshot_tests)

# The engine's legacy ScriptHost consumer is built by its existing Make/root
# target, rather than reproducing that dependency closure in this graph.
set(MATTER_SCRIPT_HOST_TEST_BINARY "" CACHE FILEPATH "Built script_host_tests executable")
if(MATTER_SCRIPT_HOST_TEST_BINARY)
    file(STRINGS "${_repo}/MatterEngine3/tests/script_host_cases.inc" _cases REGEX "^SCRIPT_HOST_CASE")
    foreach(_case IN LISTS _cases)
        string(REGEX REPLACE "SCRIPT_HOST_CASE\\(([^)]+)\\)" "\\1" _case "${_case}")
        add_test(NAME script_host_${_case} COMMAND "${MATTER_SCRIPT_HOST_TEST_BINARY}" --case "${_case}")
    endforeach()
endif()

set(MATTER_SCRIPTING_WASM_BUILD "" CACHE PATH "Built geometry-c Emscripten test graph")
if(MATTER_SCRIPTING_WASM_BUILD)
    add_test(NAME matter_geometry_wasm_wrapper COMMAND "${MATTER_HOST_NODE}"
        "${_repo}/bindings/geometry-c/tests/wasm.mjs"
        "${MATTER_SCRIPTING_WASM_BUILD}/matter_geometry_wasm.mjs" "$<TARGET_FILE:matter_geometry_c_tests>")
    add_test(NAME matter_geometry_c_consumer_wasm COMMAND "${MATTER_HOST_NODE}"
        "${_repo}/bindings/geometry-c/wasm/run-smoke.mjs"
        "${MATTER_SCRIPTING_WASM_BUILD}/matter_geometry_c_tests.mjs")
    foreach(_smoke wasm_contract_values wasm_contract_services wasm_contract_c_ownership wasm_contract_vectors)
        if(EXISTS "${MATTER_SCRIPTING_WASM_BUILD}/${_smoke}.mjs")
            add_test(NAME ${_smoke} COMMAND "${MATTER_HOST_NODE}"
                "${_repo}/bindings/geometry-c/wasm/run-smoke.mjs" "${MATTER_SCRIPTING_WASM_BUILD}/${_smoke}.mjs")
        endif()
    endforeach()
endif()
set(MATTER_WEB_WASM_MODULE "" CACHE FILEPATH "Built WASM module for the real browser worker")
set(MATTER_WEB_TEST_PYTHON "" CACHE FILEPATH "Python with Playwright")
if(MATTER_WEB_WASM_MODULE AND MATTER_WEB_TEST_PYTHON)
    add_subdirectory("${_repo}/integrations/web-procedural/runtime" web_runtime)
endif()
