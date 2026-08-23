include_guard(GLOBAL)

function(matter_validate_dist_project_name project_name)
    if("${project_name}" STREQUAL "" OR
       "${project_name}" STREQUAL "." OR
       "${project_name}" STREQUAL ".." OR
       "${project_name}" MATCHES "[:/\\\\]" OR
       NOT "${project_name}" MATCHES "^[A-Za-z0-9][A-Za-z0-9._-]*$")
        message(FATAL_ERROR
            "unsafe MATTER_DIST_PROJECT '${project_name}': expected a basename matching "
            "^[A-Za-z0-9][A-Za-z0-9._-]*$")
    endif()
endfunction()
