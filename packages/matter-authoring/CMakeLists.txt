cmake_minimum_required(VERSION 3.20)
project(matter_authoring LANGUAGES CXX)
include(CTest)
if(BUILD_TESTING)
  find_program(MATTER_AUTHORING_NODE node REQUIRED)
  find_program(MATTER_AUTHORING_TSC NAMES tsc tsc.cmd REQUIRED)
  # Compile the actual delivered data validator, without its broader gate suite.
  if(NOT TARGET matter_contracts)
    set(BUILD_TESTING OFF)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../libs/MatterContracts" "${CMAKE_CURRENT_BINARY_DIR}/contracts")
    set(BUILD_TESTING ON)
  endif()
  add_executable(matter_authoring_contract_check tests/contract_check.cpp)
  include(tests/EmbedSdk.cmake)
  target_include_directories(matter_authoring_contract_check PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
  target_link_libraries(matter_authoring_contract_check PRIVATE matter_contracts)
  add_test(NAME matter_authoring_sdk COMMAND "${MATTER_AUTHORING_NODE}" --experimental-vm-modules
    "${CMAKE_CURRENT_LIST_DIR}/tests/authoring_tests.mjs" "$<TARGET_FILE:matter_authoring_contract_check>")
  if(WIN32 AND MATTER_AUTHORING_TSC MATCHES "\\.(cmd|bat)$")
    # npm's batch shim cannot be launched directly by CTest when its path has
    # spaces. Run the same installed compiler entry point through Node.
    get_filename_component(_tsc_bin "${MATTER_AUTHORING_TSC}" DIRECTORY)
    find_file(MATTER_AUTHORING_TSC_JS tsc.js
      PATHS "${_tsc_bin}/../typescript/lib" "${_tsc_bin}/node_modules/typescript/lib"
      NO_DEFAULT_PATH REQUIRED)
    add_test(NAME matter_authoring_types COMMAND "${MATTER_AUTHORING_NODE}"
      "${MATTER_AUTHORING_TSC_JS}" -p "${CMAKE_CURRENT_LIST_DIR}/tests/tsconfig.json")
  else()
    add_test(NAME matter_authoring_types COMMAND "${MATTER_AUTHORING_TSC}" -p "${CMAKE_CURRENT_LIST_DIR}/tests/tsconfig.json")
  endif()
  find_program(MATTER_AUTHORING_BROWSER NAMES google-chrome chromium chromium-browser chrome.exe
    PATHS "$ENV{ProgramFiles}/Google/Chrome/Application")
  if(MATTER_AUTHORING_BROWSER)
    add_test(NAME matter_authoring_browser COMMAND "${MATTER_AUTHORING_NODE}" "${CMAKE_CURRENT_LIST_DIR}/tests/browser_tests.mjs")
    set_tests_properties(matter_authoring_browser PROPERTIES ENVIRONMENT "MATTER_AUTHORING_BROWSER=${MATTER_AUTHORING_BROWSER}")
  endif()
endif()
