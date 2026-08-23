set(matter_fill_sensor_ptx
    "${CMAKE_CURRENT_BINARY_DIR}/matter_physx_fill_sensor.ptx")
set(matter_fill_sensor_header
    "${CMAKE_CURRENT_BINARY_DIR}/matter_physx_fill_sensor_ptx.h")
add_custom_command(
    OUTPUT "${matter_fill_sensor_ptx}"
    COMMAND "${MATTER_CUDA_ROOT}/bin/nvcc.exe"
        --ptx --gpu-architecture=compute_89 --std=c++17
        "${CMAKE_CURRENT_SOURCE_DIR}/fill_sensor_kernels.cu"
        --output-file "${matter_fill_sensor_ptx}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/fill_sensor_kernels.cu"
    COMMENT "Compiling Matter PhysX fill-sensor PTX"
    VERBATIM
)
add_custom_command(
    OUTPUT "${matter_fill_sensor_header}"
    COMMAND "${MATTER_PYTHON_EXECUTABLE}" ${matter_python_arguments}
        "${CMAKE_SOURCE_DIR}/tools/physx/embed-ptx.py"
        "${matter_fill_sensor_header}" "${matter_fill_sensor_ptx}"
    DEPENDS "${matter_fill_sensor_ptx}"
        "${CMAKE_SOURCE_DIR}/tools/physx/embed-ptx.py"
    COMMENT "Embedding Matter PhysX fill-sensor PTX"
    COMMAND_EXPAND_LISTS
    VERBATIM
)

add_library(matter_physx_adapter STATIC
    physx_runtime.cpp
    gpu_fill_sensor.cpp
    "${matter_fill_sensor_header}"
)
set_source_files_properties("${matter_fill_sensor_header}"
    PROPERTIES GENERATED TRUE HEADER_FILE_ONLY TRUE)
target_include_directories(matter_physx_adapter PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${CMAKE_SOURCE_DIR}/MatterEngine3/src"
    "${CMAKE_SOURCE_DIR}/MatterEngine3/include"
)
target_link_libraries(matter_physx_adapter PRIVATE matter_physx_sdk)
matter_apply_project_defaults(matter_physx_adapter)
target_compile_options(matter_physx_adapter PRIVATE /WX)
