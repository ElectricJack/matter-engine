include_guard(GLOBAL)

option(MATTER_ENABLE_STREAMLINE "Enable NVIDIA DLSS Super Resolution in the Windows editor" OFF)
set(MATTER_STREAMLINE_ROOT "" CACHE PATH "Locally installed NVIDIA Streamline SDK")
set(matter_streamline_enabled 0)

if(MATTER_ENABLE_STREAMLINE)
    set(matter_streamline_enabled 1)
    if(NOT MATTER_STREAMLINE_ROOT)
        message(FATAL_ERROR "MATTER_ENABLE_STREAMLINE requires MATTER_STREAMLINE_ROOT")
    endif()
    foreach(header IN ITEMS sl.h sl_helpers_vk.h sl_core_api.h sl_consts.h sl_dlss.h sl_security.h)
        if(NOT EXISTS "${MATTER_STREAMLINE_ROOT}/include/${header}")
            message(FATAL_ERROR "Streamline SDK header is missing: ${MATTER_STREAMLINE_ROOT}/include/${header}")
        endif()
    endforeach()
    # Only Super Resolution is loaded by StreamlineBridge. Stage the production
    # core and that plugin's exact dependencies, not unrelated SDK features.
    set(matter_streamline_runtime_files)
    foreach(runtime IN ITEMS sl.interposer.dll sl.common.dll sl.dlss.dll nvngx_dlss.dll)
        set(path "${MATTER_STREAMLINE_ROOT}/bin/x64/${runtime}")
        if(NOT EXISTS "${path}")
            message(FATAL_ERROR "Streamline production runtime is missing: ${path}")
        endif()
        list(APPEND matter_streamline_runtime_files "${path}")
    endforeach()
    foreach(license IN ITEMS license.txt bin/x64/nvngx_dlss.license.txt)
        if(NOT EXISTS "${MATTER_STREAMLINE_ROOT}/${license}")
            message(FATAL_ERROR "Streamline license is missing: ${MATTER_STREAMLINE_ROOT}/${license}")
        endif()
    endforeach()
endif()

function(matter_stage_streamline_runtime target)
    if(NOT MATTER_ENABLE_STREAMLINE)
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            ${matter_streamline_runtime_files} "$<TARGET_FILE_DIR:${target}>"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${target}>/licenses"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${MATTER_STREAMLINE_ROOT}/license.txt"
            "$<TARGET_FILE_DIR:${target}>/licenses/NVIDIA_Streamline_LICENSE.txt"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${MATTER_STREAMLINE_ROOT}/bin/x64/nvngx_dlss.license.txt"
            "$<TARGET_FILE_DIR:${target}>/licenses/NVIDIA_DLSS_LICENSE.txt"
        COMMENT "Staging NVIDIA DLSS Super Resolution production runtime"
        VERBATIM)
    # A changed SDK binary must restage even if the C++ link inputs are unchanged.
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
        ${matter_streamline_runtime_files}
        "${MATTER_STREAMLINE_ROOT}/license.txt"
        "${MATTER_STREAMLINE_ROOT}/bin/x64/nvngx_dlss.license.txt")
endfunction()
