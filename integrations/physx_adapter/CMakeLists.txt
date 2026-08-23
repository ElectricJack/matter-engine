add_library(matter_physx_adapter STATIC
    physx_runtime.cpp
)
target_include_directories(matter_physx_adapter PRIVATE
    "${CMAKE_SOURCE_DIR}/MatterEngine3/src"
    "${CMAKE_SOURCE_DIR}/MatterEngine3/include"
)
target_link_libraries(matter_physx_adapter PRIVATE matter_physx_sdk)
matter_apply_project_defaults(matter_physx_adapter)
target_compile_options(matter_physx_adapter PRIVATE /WX)
