set(matter_third_party_boundaries
    matter_quickjs
    matter_flecs
    matter_box3d
    matter_glfw
    matter_ozz_base
    matter_ozz_animation
    matter_ozz_offline
    matter_autoremesher
    matter_imgui
    matter_imguizmo
    matter_bc7enc
)

foreach(boundary IN LISTS matter_third_party_boundaries)
    if(NOT TARGET "${boundary}")
        message(FATAL_ERROR "matter_third_party_smoke requires missing target: ${boundary}")
    endif()
endforeach()

add_executable(matter_third_party_smoke third_party_smoke.cpp)
target_link_libraries(matter_third_party_smoke PRIVATE ${matter_third_party_boundaries})
matter_apply_project_defaults(matter_third_party_smoke)
add_test(NAME matter_third_party_smoke COMMAND matter_third_party_smoke)
set_tests_properties(matter_third_party_smoke PROPERTIES LABELS third_party)

add_test(NAME matter_third_party_binary_audit
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/verify_binary.ps1"
        -Executable "$<TARGET_FILE:matter_third_party_smoke>"
        -BuildDirectory "${CMAKE_BINARY_DIR}"
        -Ninja "${CMAKE_MAKE_PROGRAM}"
)
set_tests_properties(matter_third_party_binary_audit PROPERTIES
    DEPENDS matter_third_party_smoke
    LABELS third_party
)
