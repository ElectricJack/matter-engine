enable_testing()
find_program(MATTER_WASM_NODE NAMES node REQUIRED)
set(_matter_contract_tests "${CMAKE_CURRENT_LIST_DIR}/../../../libs/MatterContracts/tests")
target_link_libraries(matter_contracts PRIVATE matter_wasm_profile)
function(matter_contract_smoke name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE matter_geometry_c matter_wasm_profile)
    set_target_properties(${name} PROPERTIES SUFFIX ".mjs" C_STANDARD 11 C_STANDARD_REQUIRED YES)
    add_test(NAME ${name} COMMAND "${MATTER_WASM_NODE}"
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/run-smoke.mjs" "$<TARGET_FILE:${name}>")
    set_tests_properties(${name} PROPERTIES TIMEOUT 60)
endfunction()
# Compile the source-of-truth fixtures, without copies or the monolithic archive.
matter_contract_smoke(wasm_contract_values "${_matter_contract_tests}/contracts_tests.cpp")
matter_contract_smoke(wasm_contract_services
    "${_matter_contract_tests}/service_producers.cpp" "${_matter_contract_tests}/service_consumers.cpp")
matter_contract_smoke(wasm_contract_c_ownership
    "${_matter_contract_tests}/abi_producer.c" "${_matter_contract_tests}/abi_consumer.c")
matter_contract_smoke(wasm_contract_vectors "${_matter_contract_tests}/vectors.cpp")
# Compile each public header alone in C++ and the ABI in C as in the frozen gate.
foreach(header contracts jobs generation hosts graphics terrain abi)
    set(source "${CMAKE_CURRENT_BINARY_DIR}/header-${header}.cpp")
    file(WRITE "${source}" "#include <matter/contracts/${header}.h>\n")
    add_library(wasm_header_${header} OBJECT "${source}")
    target_link_libraries(wasm_header_${header} PRIVATE matter_contracts matter_wasm_profile)
endforeach()
matter_write_component_graph("${CMAKE_CURRENT_BINARY_DIR}/component-graph.txt")
