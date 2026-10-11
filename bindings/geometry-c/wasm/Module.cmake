find_program(MATTER_WASM_NODE NAMES node REQUIRED)
# The archive's explicit exports are linker roots; no main/embind/runtime host.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/module.cpp" "// C ABI exports live in matter_geometry_c.\n")
add_executable(matter_geometry_wasm "${CMAKE_CURRENT_BINARY_DIR}/module.cpp")
target_link_libraries(matter_geometry_wasm PRIVATE matter_geometry_c matter_wasm_profile)
set_target_properties(matter_geometry_wasm PROPERTIES SUFFIX ".mjs")
target_link_options(matter_geometry_wasm PRIVATE --no-entry
    "-sEXPORTED_FUNCTIONS=_malloc,_free,_mgw_create,_mgw_dispose,_mgw_budget,_mgw_call,_mgw_read,_mgw_release,_mgw_info"
    "-sEXPORTED_RUNTIME_METHODS=HEAPU8" "-sEXIT_RUNTIME=0" "-sWASM_BIGINT=1")
if(MATTER_GEOMETRY_C_TESTS)
    enable_testing()
    set(MATTER_GEOMETRY_C_NATIVE_CONSUMER "" CACHE FILEPATH "Optional native C consumer for cross-target fixture comparison")
    add_test(NAME matter_geometry_wasm_wrapper COMMAND "${MATTER_WASM_NODE}"
        "${CMAKE_CURRENT_LIST_DIR}/../tests/wasm.mjs" "$<TARGET_FILE:matter_geometry_wasm>"
        ${MATTER_GEOMETRY_C_NATIVE_CONSUMER})
    set_tests_properties(matter_geometry_wasm_wrapper PROPERTIES TIMEOUT 120)
endif()
