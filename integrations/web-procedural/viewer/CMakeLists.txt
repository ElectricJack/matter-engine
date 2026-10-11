cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(MatterWebViewer LANGUAGES NONE)
    include(CTest)
endif()
find_program(MATTER_WEB_TSC NAMES tsc REQUIRED)
set(MATTER_WEB_WASM_MODULE "" CACHE FILEPATH "Existing matter_geometry_wasm.mjs for browser tests")
set(MATTER_WEB_TEST_PYTHON "" CACHE FILEPATH "Python with Playwright installed")
set(MATTER_WEB_TEST_BROWSER "/opt/google/chrome/chrome" CACHE FILEPATH "Chromium executable for browser tests")
set(_web_runtime "${CMAKE_CURRENT_SOURCE_DIR}/../runtime")
file(GLOB_RECURSE _viewer_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.mts" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.d.ts")
file(GLOB _runtime_sources CONFIGURE_DEPENDS "${_web_runtime}/src/*.mts")
# The viewer library imports only 'three' and its own modules. The compiled runtime
# is built here solely for the demo page and the browser tests.
add_custom_target(matter_web_viewer ALL
    COMMAND "${MATTER_WEB_TSC}" -p "${CMAKE_CURRENT_SOURCE_DIR}/tsconfig.json"
        --outDir "${CMAKE_CURRENT_BINARY_DIR}/viewer"
    DEPENDS ${_viewer_sources} "${CMAKE_CURRENT_SOURCE_DIR}/tsconfig.json" VERBATIM)
add_custom_target(matter_web_viewer_runtime ALL
    COMMAND "${MATTER_WEB_TSC}" -p "${_web_runtime}/tsconfig.json"
        --outDir "${CMAKE_CURRENT_BINARY_DIR}/runtime"
    DEPENDS ${_runtime_sources} "${_web_runtime}/tsconfig.json" VERBATIM)
if(BUILD_TESTING)
    if(NOT MATTER_WEB_WASM_MODULE OR NOT MATTER_WEB_TEST_PYTHON)
        message(FATAL_ERROR "Viewer browser tests require MATTER_WEB_WASM_MODULE and MATTER_WEB_TEST_PYTHON with Playwright")
    endif()
    add_test(NAME matter_web_viewer_types
        COMMAND "${MATTER_WEB_TSC}" -p "${CMAKE_CURRENT_SOURCE_DIR}/tests/tsconfig.json")
    add_test(NAME matter_web_viewer_browser
        COMMAND "${MATTER_WEB_TEST_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/browser_tests.py"
            --wasm-module "${MATTER_WEB_WASM_MODULE}"
            --runtime-dir "${CMAKE_CURRENT_BINARY_DIR}/runtime"
            --viewer-dir "${CMAKE_CURRENT_BINARY_DIR}/viewer"
            --browser "${MATTER_WEB_TEST_BROWSER}" --tsc "${MATTER_WEB_TSC}"
            --output "${CMAKE_CURRENT_BINARY_DIR}/viewer-evidence.json"
            --screenshots "${CMAKE_CURRENT_BINARY_DIR}/viewer-screenshots")
    set_tests_properties(matter_web_viewer_browser PROPERTIES TIMEOUT 600)
endif()
