include_guard(GLOBAL)

# Shared by the standalone graphics and native core registrations. Make reads
# the same input manifest and runs the same Python qualifier.
function(matter_qualify_environment_test target)
    if(NOT TARGET matter_graphics_environment_qualification)
        get_filename_component(repository "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../.." ABSOLUTE)
        set(qualifier "${repository}/libs/MatterGraphics/tools/qualify_environment.py")
        set(manifest "${repository}/libs/MatterGraphics/tools/environment_qualification.inputs")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${manifest}")
        file(STRINGS "${manifest}" inputs)
        list(TRANSFORM inputs PREPEND "${repository}/")
        set(stamp "${CMAKE_CURRENT_BINARY_DIR}/environment-qualified.stamp")
        add_custom_command(OUTPUT "${stamp}"
            COMMAND ${ARGN} "${qualifier}" "${repository}"
            COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
            DEPENDS "${qualifier}" "${manifest}" ${inputs} VERBATIM)
        add_custom_target(matter_graphics_environment_qualification DEPENDS "${stamp}")
    endif()
    add_dependencies(${target} matter_graphics_environment_qualification)
endfunction()
