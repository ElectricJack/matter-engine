include_guard(GLOBAL)

include(CMakeParseArguments)

function(matter_resolve_windows_python)
    set(one_value_args EXECUTABLE OUT_ARGUMENTS OUT_VERSION)
    cmake_parse_arguments(PARSE_ARGV 0 MATTER_PYTHON
        "" "${one_value_args}" "")
    foreach(required_arg IN ITEMS EXECUTABLE OUT_ARGUMENTS OUT_VERSION)
        if(NOT MATTER_PYTHON_${required_arg})
            message(FATAL_ERROR
                "matter_resolve_windows_python requires ${required_arg}")
        endif()
    endforeach()
    if(NOT EXISTS "${MATTER_PYTHON_EXECUTABLE}")
        message(FATAL_ERROR
            "Selected native Windows Python does not exist: ${MATTER_PYTHON_EXECUTABLE}")
    endif()

    get_filename_component(python_name "${MATTER_PYTHON_EXECUTABLE}" NAME)
    string(TOLOWER "${python_name}" python_name)
    set(python_arguments)
    if(python_name STREQUAL "py.exe" OR python_name STREQUAL "py")
        list(APPEND python_arguments -3)
    endif()
    execute_process(
        COMMAND "${MATTER_PYTHON_EXECUTABLE}" ${python_arguments} --version
        RESULT_VARIABLE python_result
        OUTPUT_VARIABLE python_stdout
        ERROR_VARIABLE python_stderr
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
    )
    set(python_version "${python_stdout}${python_stderr}")
    if(NOT python_result EQUAL 0)
        message(FATAL_ERROR
            "Selected native Windows Python failed: "
            "'${MATTER_PYTHON_EXECUTABLE} ${python_arguments} --version': "
            "${python_version}")
    endif()
    set("${MATTER_PYTHON_OUT_ARGUMENTS}" "${python_arguments}" PARENT_SCOPE)
    set("${MATTER_PYTHON_OUT_VERSION}" "${python_version}" PARENT_SCOPE)
endfunction()

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
        PYTHON_EXECUTABLE
        EMBED_SCRIPT
    )
    set(multi_value_args SHADERS SPECIALIZATIONS PYTHON_ARGUMENTS)
    cmake_parse_arguments(PARSE_ARGV 0 MATTER_SHADER
        "${options}" "${one_value_args}" "${multi_value_args}")

    foreach(required_arg IN ITEMS
            PREFIX SOURCE_DIR OUTPUT_DIR EMBEDDED_HEADER GLSLC
            PYTHON_EXECUTABLE EMBED_SCRIPT)
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
            "${MATTER_SHADER_PYTHON_EXECUTABLE}"
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
    # A full inventory of absolute paths can exceed CMD's 8191-character
    # limit in a long/spaced checkout. Keep the command bounded; generating
    # this list only updates its timestamp when the inventory actually changes.
    set(spirv_input_list "${CMAKE_CURRENT_BINARY_DIR}/${MATTER_SHADER_PREFIX}_spirv_inputs.txt")
    string(REPLACE ";" "\n" spirv_input_list_content "${spirv_outputs}")
    file(GENERATE OUTPUT "${spirv_input_list}" CONTENT "${spirv_input_list_content}\n")
    add_custom_command(
        OUTPUT "${MATTER_SHADER_EMBEDDED_HEADER}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${embedded_header_directory}"
        COMMAND "${MATTER_SHADER_PYTHON_EXECUTABLE}"
            ${MATTER_SHADER_PYTHON_ARGUMENTS}
            "${MATTER_SHADER_EMBED_SCRIPT}"
            "${MATTER_SHADER_EMBEDDED_HEADER}"
            --input-list "${spirv_input_list}"
        DEPENDS ${spirv_outputs} "${spirv_input_list}" "${MATTER_SHADER_EMBED_SCRIPT}"
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
