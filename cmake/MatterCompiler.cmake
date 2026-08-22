include_guard(GLOBAL)

function(matter_apply_project_defaults target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "matter_apply_project_defaults: unknown target '${target}'")
    endif()
    if(NOT MSVC)
        message(FATAL_ERROR "matter_apply_project_defaults requires MSVC")
    endif()

    get_target_property(target_type "${target}" TYPE)
    if(target_type STREQUAL "INTERFACE_LIBRARY")
        set(usage_scope INTERFACE)
    else()
        set(usage_scope PRIVATE)
        set_target_properties("${target}" PROPERTIES
            C_STANDARD 17
            C_STANDARD_REQUIRED ON
            C_EXTENSIONS OFF
            CXX_STANDARD 17
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
    endif()

    target_compile_features("${target}" ${usage_scope} c_std_17 cxx_std_17)
    target_compile_options("${target}" ${usage_scope}
        /utf-8
        /W4
        "$<$<COMPILE_LANGUAGE:CXX>:/permissive->"
        "$<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>"
        "$<$<COMPILE_LANGUAGE:CXX>:/EHsc>"
    )
endfunction()
