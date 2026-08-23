include_guard(GLOBAL)

include(CMakeParseArguments)

# Define deterministic glslc -> SPIR-V -> embedded-header targets. Shader stage
# names are supplied by the caller so the production build can discover the
# existing source directory while focused tests can use a one-file fixture.
# glslc depfiles own nested #include tracking; this is intentionally stronger
# than duplicating the Makefile's hand-maintained .glsl edge list.
function(matter_add_vulkan_shader_pipeline)
    set(options)
    set(one_value_args
        PREFIX
        SOURCE_DIR
        OUTPUT_DIR
        EMBEDDED_HEADER
        GLSLC
        PYTHON_LAUNCHER
        EMBED_SCRIPT
    )
    set(multi_value_args SHADERS SPECIALIZATIONS)
    cmake_parse_arguments(PARSE_ARGV 0 MATTER_SHADER
        "${options}" "${one_value_args}" "${multi_value_args}")

    foreach(required_arg IN ITEMS
            PREFIX SOURCE_DIR OUTPUT_DIR EMBEDDED_HEADER GLSLC
            PYTHON_LAUNCHER EMBED_SCRIPT)
        if(NOT MATTER_SHADER_${required_arg})
            message(FATAL_ERROR
                "matter_add_vulkan_shader_pipeline requires ${required_arg}")
        endif()
    endforeach()
    if(NOT MATTER_SHADER_SHADERS)
        message(FATAL_ERROR
            "matter_add_vulkan_shader_pipeline requires at least one SHADERS entry")
    endif()
    foreach(required_file IN ITEMS
            "${MATTER_SHADER_GLSLC}"
            "${MATTER_SHADER_PYTHON_LAUNCHER}"
            "${MATTER_SHADER_EMBED_SCRIPT}")
        if(NOT EXISTS "${required_file}")
            message(FATAL_ERROR "Required shader tool was not found: ${required_file}")
        endif()
    endforeach()

    set(spirv_outputs)
    foreach(shader IN LISTS MATTER_SHADER_SHADERS)
        set(shader_source "${MATTER_SHADER_SOURCE_DIR}/${shader}")
        set(shader_output "${MATTER_SHADER_OUTPUT_DIR}/${shader}.spv")
        set(shader_depfile "${shader_output}.d")
        if(NOT EXISTS "${shader_source}")
            message(FATAL_ERROR "Shader source was not found: ${shader_source}")
        endif()
        get_filename_component(shader_output_directory "${shader_output}" DIRECTORY)
        add_custom_command(
            OUTPUT "${shader_output}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${shader_output_directory}"
            COMMAND "${MATTER_SHADER_GLSLC}"
                --target-env=vulkan1.3
                -O
                -I "${MATTER_SHADER_SOURCE_DIR}"
                -MD
                -MF "${shader_depfile}"
                "${shader_source}"
                -o "${shader_output}"
            DEPENDS "${shader_source}"
            DEPFILE "${shader_depfile}"
            BYPRODUCTS "${shader_depfile}"
            COMMENT "Compiling Vulkan shader ${shader}"
            VERBATIM
        )
        list(APPEND spirv_outputs "${shader_output}")
    endforeach()

    # Each specialization is OUTPUT_NAME|SOURCE_NAME|DEFINE. This preserves
    # raster_skin.vert.spv as a distinct binary compiled from raster.vert.
    foreach(specialization IN LISTS MATTER_SHADER_SPECIALIZATIONS)
        string(REPLACE "|" ";" specialization_fields "${specialization}")
        list(LENGTH specialization_fields specialization_length)
        if(NOT specialization_length EQUAL 3)
            message(FATAL_ERROR
                "Invalid shader specialization '${specialization}'; expected OUTPUT|SOURCE|DEFINE")
        endif()
        list(GET specialization_fields 0 output_name)
        list(GET specialization_fields 1 source_name)
        list(GET specialization_fields 2 compile_define)
        set(shader_source "${MATTER_SHADER_SOURCE_DIR}/${source_name}")
        set(shader_output "${MATTER_SHADER_OUTPUT_DIR}/${output_name}.spv")
        set(shader_depfile "${shader_output}.d")
        if(NOT EXISTS "${shader_source}")
            message(FATAL_ERROR "Specialized shader source was not found: ${shader_source}")
        endif()
        get_filename_component(shader_output_directory "${shader_output}" DIRECTORY)
        add_custom_command(
            OUTPUT "${shader_output}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${shader_output_directory}"
            COMMAND "${MATTER_SHADER_GLSLC}"
                --target-env=vulkan1.3
                -O
                -I "${MATTER_SHADER_SOURCE_DIR}"
                "-D${compile_define}"
                -MD
                -MF "${shader_depfile}"
                "${shader_source}"
                -o "${shader_output}"
            DEPENDS "${shader_source}"
            DEPFILE "${shader_depfile}"
            BYPRODUCTS "${shader_depfile}"
            COMMENT "Compiling Vulkan shader specialization ${output_name}"
            VERBATIM
        )
        list(APPEND spirv_outputs "${shader_output}")
    endforeach()

    list(SORT spirv_outputs)
    set(spirv_target "${MATTER_SHADER_PREFIX}_vulkan_spirv")
    set(embed_target "${MATTER_SHADER_PREFIX}_embedded_spirv")
    add_custom_target("${spirv_target}" DEPENDS ${spirv_outputs})

    get_filename_component(embedded_header_directory
        "${MATTER_SHADER_EMBEDDED_HEADER}" DIRECTORY)
    add_custom_command(
        OUTPUT "${MATTER_SHADER_EMBEDDED_HEADER}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${embedded_header_directory}"
        COMMAND "${MATTER_SHADER_PYTHON_LAUNCHER}" -3
            "${MATTER_SHADER_EMBED_SCRIPT}"
            "${MATTER_SHADER_EMBEDDED_HEADER}"
            ${spirv_outputs}
        DEPENDS ${spirv_outputs} "${MATTER_SHADER_EMBED_SCRIPT}"
        COMMENT "Embedding Vulkan SPIR-V"
        COMMAND_EXPAND_LISTS
        VERBATIM
    )
    add_custom_target("${embed_target}" DEPENDS "${MATTER_SHADER_EMBEDDED_HEADER}")
    add_dependencies("${embed_target}" "${spirv_target}")

    set("${MATTER_SHADER_PREFIX}_SPIRV_OUTPUTS" "${spirv_outputs}" PARENT_SCOPE)
    set("${MATTER_SHADER_PREFIX}_EMBEDDED_SPIRV_HEADER"
        "${MATTER_SHADER_EMBEDDED_HEADER}" PARENT_SCOPE)
endfunction()
