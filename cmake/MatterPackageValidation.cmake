include_guard(GLOBAL)

function(matter_validate_dist_project_name project_name)
    string(TOUPPER "${project_name}" project_name_upper)
    string(REGEX REPLACE "\\..*$" "" project_name_device_stem
        "${project_name_upper}")
    if("${project_name}" STREQUAL "" OR
       "${project_name}" STREQUAL "." OR
       "${project_name}" STREQUAL ".." OR
       "${project_name}" MATCHES "[:/\\\\]" OR
       "${project_name}" MATCHES "\\.$" OR
       "${project_name_device_stem}" MATCHES
           "^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$" OR
       NOT "${project_name}" MATCHES "^[A-Za-z0-9][A-Za-z0-9._-]*$")
        message(FATAL_ERROR
            "unsafe MATTER_DIST_PROJECT '${project_name}': expected a basename matching "
            "^[A-Za-z0-9][A-Za-z0-9._-]*$ without Windows trailing-dot or "
            "DOS-device aliases")
    endif()
endfunction()
