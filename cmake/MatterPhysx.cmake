include_guard(GLOBAL)

include(ExternalProject)

set(matter_physx_configuration_inputs
    "${CMAKE_CURRENT_LIST_FILE}"
    "${CMAKE_SOURCE_DIR}/tools/deps/physx.lock.json"
    "${CMAKE_SOURCE_DIR}/tools/physx/MatterPhysxDependency.psm1"
    "${CMAKE_SOURCE_DIR}/tools/physx/build-physx-control.ps1"
    "${CMAKE_SOURCE_DIR}/tools/physx/configure-physx-static.ps1"
    "${CMAKE_SOURCE_DIR}/tools/windows/MatterWindowsToolchain.psm1"
)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${matter_physx_configuration_inputs})
set(matter_physx_configuration_material)
foreach(input IN LISTS matter_physx_configuration_inputs)
    file(SHA256 "${input}" input_hash)
    string(APPEND matter_physx_configuration_material "${input_hash};")
endforeach()
string(APPEND matter_physx_configuration_material
    "build-targets=PhysXCooking,PhysXExtensions;"
    "configuration=release;abi=win.x86_64.vc143.mt;"
    "cmake=${CMAKE_VERSION};generator=${CMAKE_GENERATOR};"
    "generator-platform=${CMAKE_GENERATOR_PLATFORM};"
    "generator-toolset=${CMAKE_GENERATOR_TOOLSET};"
    "compiler=${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION};"
    "compiler-path=${CMAKE_CXX_COMPILER};msvc=${MSVC_VERSION};"
    "windows-sdk=${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION};")
string(SHA256 matter_physx_configuration_hash
    "${matter_physx_configuration_material}")
string(SUBSTRING "${matter_physx_configuration_hash}" 0 16
    matter_physx_configuration_key)

option(MATTER_ENABLE_PHYSX
    "Build the opt-in in-process NVIDIA PhysX GPU fluid adapter" OFF)
set(MATTER_PHYSX_ROOT "" CACHE PATH
    "External pinned NVIDIA PhysX 5.6.1 source checkout")
set(MATTER_CUDA_ROOT "" CACHE PATH
    "CUDA Toolkit 12.8 root used by the pinned PhysX build")

if(NOT MATTER_ENABLE_PHYSX)
    return()
endif()
if(NOT MSVC OR NOT WIN32)
    message(FATAL_ERROR "MATTER_ENABLE_PHYSX requires native Windows MSVC")
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(FATAL_ERROR
        "The pinned PhysX control currently provides release-ABI libraries; "
        "use RelWithDebInfo or Release for MATTER_ENABLE_PHYSX")
endif()
if(NOT IS_DIRECTORY "${MATTER_PHYSX_ROOT}")
    message(FATAL_ERROR
        "MATTER_PHYSX_ROOT must name the external pinned checkout: "
        "'${MATTER_PHYSX_ROOT}'")
endif()
if(NOT IS_DIRECTORY "${MATTER_CUDA_ROOT}")
    message(FATAL_ERROR
        "MATTER_CUDA_ROOT must name CUDA Toolkit 12.8: "
        "'${MATTER_CUDA_ROOT}'")
endif()

execute_process(
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tools/physx/build-physx-control.ps1"
        -Mode Validate
        -RepositoryRoot "${CMAKE_SOURCE_DIR}"
        -PhysxRoot "${MATTER_PHYSX_ROOT}"
        -CudaRoot "${MATTER_CUDA_ROOT}"
    RESULT_VARIABLE matter_physx_validation_result
    OUTPUT_VARIABLE matter_physx_validation_output
    ERROR_VARIABLE matter_physx_validation_error
)
if(NOT matter_physx_validation_result EQUAL 0)
    message(FATAL_ERROR
        "Pinned PhysX dependency validation failed.\n"
        "stdout:\n${matter_physx_validation_output}\n"
        "stderr:\n${matter_physx_validation_error}")
endif()
string(STRIP "${matter_physx_validation_output}"
    matter_physx_validation_output)
message(STATUS "${matter_physx_validation_output}")

set(matter_physx_sdk_root "${MATTER_PHYSX_ROOT}/physx")
set(matter_physx_control_bin
    "${matter_physx_sdk_root}/bin/win.x86_64.vc143.mt/release")
set(matter_physx_gpu_runtime
    "${matter_physx_control_bin}/PhysXGpu_64.dll")
set(matter_physx_license "${MATTER_PHYSX_ROOT}/LICENSE.md")
set(matter_cuda_license "${MATTER_CUDA_ROOT}/EULA.txt")
if(NOT EXISTS "${matter_physx_gpu_runtime}")
    message(FATAL_ERROR
        "Pinned PhysX GPU runtime is missing '${matter_physx_gpu_runtime}'. "
        "Run tools/physx/build-physx-control.ps1 -Mode Build first.")
endif()
foreach(notice IN ITEMS "${matter_physx_license}" "${matter_cuda_license}")
    if(NOT EXISTS "${notice}")
        message(FATAL_ERROR "Pinned NVIDIA package notice is missing '${notice}'")
    endif()
endforeach()

# The upstream control uses the official shared Windows preset. Matter builds
# a separate static-core SDK variant into its own build tree so editor startup
# never imports optional PhysX core DLLs. PhysXGpu remains the one deliberately
# dynamic, lazily loaded module.
set(matter_physx_build_dir
    "${CMAKE_BINARY_DIR}/physx/${matter_physx_configuration_key}/sdk-build")
set(matter_physx_output_root
    "${CMAKE_BINARY_DIR}/physx/${matter_physx_configuration_key}/sdk-output")
set(matter_physx_static_dir
    "${matter_physx_output_root}/bin/win.x86_64.vc143.mt/release")
set(matter_physx_static_names
    PhysX_static.lib
    PhysXCommon_static.lib
    PhysXFoundation_static.lib
    PhysXCooking_static.lib
    PhysXExtensions_static.lib
    PhysXPvdSDK_static.lib
)
set(matter_physx_static_libraries)
foreach(filename IN LISTS matter_physx_static_names)
    list(APPEND matter_physx_static_libraries
        "${matter_physx_static_dir}/${filename}")
endforeach()

set(matter_physx_control_cache
    "${matter_physx_sdk_root}/compiler/vc17win64/CMakeCache.txt")
if(NOT EXISTS "${matter_physx_control_cache}")
    message(FATAL_ERROR
        "Official PhysX control cache is missing "
        "'${matter_physx_control_cache}'. Run the control generator first.")
endif()
file(STRINGS "${matter_physx_control_cache}"
    matter_physx_freeglut_cache_line
    REGEX "^PHYSX_SLN_FREEGLUT_PATH:INTERNAL="
    LIMIT_COUNT 1)
string(REGEX REPLACE "^[^=]*=" ""
    matter_physx_freeglut_path "${matter_physx_freeglut_cache_line}")
if(NOT IS_DIRECTORY "${matter_physx_freeglut_path}/win64")
    message(FATAL_ERROR
        "The validated PhysX control did not retain its FreeGLUT dependency: "
        "'${matter_physx_freeglut_path}'")
endif()

ExternalProject_Add(matter_physx_sdk_build
    SOURCE_DIR "${matter_physx_sdk_root}/compiler/public"
    BINARY_DIR "${matter_physx_build_dir}"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    PATCH_COMMAND ""
    CONFIGURE_COMMAND
        powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tools/physx/configure-physx-static.ps1"
            -CMakeExecutable "${CMAKE_COMMAND}"
            -SourceDirectory "${matter_physx_sdk_root}/compiler/public"
            -BinaryDirectory "${matter_physx_build_dir}"
            -PhysxSdkRoot "${matter_physx_sdk_root}"
            -CudaRoot "${MATTER_CUDA_ROOT}"
            -OutputRoot "${matter_physx_output_root}"
            -FreeglutPath "${matter_physx_freeglut_path}"
    BUILD_COMMAND
        "${CMAKE_COMMAND}" --build <BINARY_DIR> --config release
            --target PhysXCooking PhysXExtensions
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS ${matter_physx_static_libraries}
    USES_TERMINAL_CONFIGURE TRUE
    USES_TERMINAL_BUILD TRUE
)

set(matter_cuda_driver_library "${MATTER_CUDA_ROOT}/lib/x64/cuda.lib")
if(NOT EXISTS "${matter_cuda_driver_library}")
    message(FATAL_ERROR
        "CUDA driver import library is missing: ${matter_cuda_driver_library}")
endif()

add_library(matter_physx_sdk INTERFACE)
add_dependencies(matter_physx_sdk matter_physx_sdk_build)
target_include_directories(matter_physx_sdk SYSTEM INTERFACE
    "${matter_physx_sdk_root}/include"
    "${matter_physx_sdk_root}/include/cudamanager"
    "${MATTER_CUDA_ROOT}/include"
)
target_compile_definitions(matter_physx_sdk INTERFACE
    PX_PUBLIC_RELEASE=1
    PX_PHYSX_STATIC_LIB=1
    PX_SUPPORT_GPU_PHYSX=1
    PX_SUPPORT_PVD=0
    PX_SUPPORT_OMNI_PVD=0
)
target_link_libraries(matter_physx_sdk INTERFACE
    ${matter_physx_static_libraries}
    "${matter_cuda_driver_library}"
    delayimp.lib
    ws2_32.lib
)

set(MATTER_PHYSX_RUNTIME_FILES "${matter_physx_gpu_runtime}")

function(matter_stage_physx_runtime target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR
            "matter_stage_physx_runtime: unknown target '${target}'")
    endif()
    foreach(runtime IN LISTS MATTER_PHYSX_RUNTIME_FILES)
        add_custom_command(TARGET "${target}" POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${runtime}" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM
        )
    endforeach()
endfunction()

add_subdirectory(integrations/physx_adapter)
