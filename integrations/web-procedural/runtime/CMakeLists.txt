cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(MatterWebRuntime LANGUAGES NONE)
    include(CTest)
endif()
find_program(MATTER_WEB_TSC NAMES tsc tsc.cmd REQUIRED)
set(MATTER_WEB_WASM_MODULE "" CACHE FILEPATH "Existing matter_geometry_wasm.mjs for browser tests")
set(MATTER_WEB_TEST_PYTHON "" CACHE FILEPATH "Python with Playwright installed")
file(GLOB _web_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.mts")
add_custom_target(matter_web_runtime ALL
    COMMAND "${MATTER_WEB_TSC}" -p "${CMAKE_CURRENT_SOURCE_DIR}/tsconfig.json"
        --outDir "${CMAKE_CURRENT_BINARY_DIR}/runtime"
    DEPENDS ${_web_sources} "${CMAKE_CURRENT_SOURCE_DIR}/tsconfig.json" VERBATIM)
if(EMSCRIPTEN)
    enable_language(C)
    enable_language(CXX)
    set(MATTER_GEOMETRY_C_TESTS OFF)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../../bindings/geometry-c" geometry)
    set(MATTER_WEB_WASM_MODULE "$<TARGET_FILE:matter_geometry_wasm>")
    add_dependencies(matter_web_runtime matter_geometry_wasm)
endif()
if(BUILD_TESTING)
    if(NOT MATTER_WEB_WASM_MODULE OR NOT MATTER_WEB_TEST_PYTHON)
        message(FATAL_ERROR "Browser tests require MATTER_WEB_WASM_MODULE (or Emscripten) and MATTER_WEB_TEST_PYTHON with Playwright")
    endif()
    add_test(NAME matter_web_browser
        COMMAND "${MATTER_WEB_TEST_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/browser_tests.py"
            --wasm-module "${MATTER_WEB_WASM_MODULE}" --runtime-dir "${CMAKE_CURRENT_BINARY_DIR}/runtime"
            --output "${CMAKE_CURRENT_BINARY_DIR}/browser-evidence.json")
    set_tests_properties(matter_web_browser PROPERTIES TIMEOUT 120)
endif()
