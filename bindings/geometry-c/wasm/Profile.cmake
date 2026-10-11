# One supported profile; flags are versioned with the toolchain lock.
file(READ "${CMAKE_CURRENT_LIST_DIR}/toolchain.json" _matter_wasm_lock)
string(JSON _matter_em_version GET "${_matter_wasm_lock}" emscripten)
string(JSON _matter_em_revision GET "${_matter_wasm_lock}" emscripten_revision)
execute_process(COMMAND "${CMAKE_C_COMPILER}" --version
    OUTPUT_VARIABLE _matter_em_output RESULT_VARIABLE _matter_em_status)
string(FIND "${_matter_em_output}" " ${_matter_em_version} " _matter_em_version_position)
string(FIND "${_matter_em_output}" "${_matter_em_revision}" _matter_em_revision_position)
if(NOT _matter_em_status EQUAL 0 OR _matter_em_version_position LESS 0 OR _matter_em_revision_position LESS 0)
    message(FATAL_ERROR "Baseline requires Emscripten ${_matter_em_version}; use wasm/build-smoke.py")
endif()
add_library(matter_wasm_profile INTERFACE)
foreach(kind compile link)
    string(JSON count LENGTH "${_matter_wasm_lock}" ${kind}_options)
    math(EXPR last "${count} - 1")
    foreach(i RANGE 0 ${last})
        string(JSON flag GET "${_matter_wasm_lock}" ${kind}_options ${i})
        # Entrypoint lifecycle is target-specific: contract executables call
        # main/exit; the production library has neither. Keep the locked smoke
        # settings on its executable targets rather than leaking them transitively.
        if(kind STREQUAL "link" AND flag MATCHES "^-s(EXPORTED_RUNTIME_METHODS|EXIT_RUNTIME)=")
            continue()
        endif()
        if(kind STREQUAL "compile")
            target_compile_options(matter_wasm_profile INTERFACE "${flag}")
        else()
            target_link_options(matter_wasm_profile INTERFACE "${flag}")
        endif()
    endforeach()
endforeach()
# Compile-time enforcement also catches injected -pthread/SIMD flags.
target_compile_options(matter_wasm_profile INTERFACE
    "SHELL:-include \"${CMAKE_CURRENT_LIST_DIR}/profile_guard.h\"")
