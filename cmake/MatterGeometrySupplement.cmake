# Use each component's actual standalone graph for the two incompatible
# backend configurations. Both rebuild their source/generated-graph ALL
# qualification on every parent build; no copied binaries or success stamp.
function(matter_add_geometry_compute_test name backend tier repository glslc)
    set(build "${CMAKE_CURRENT_BINARY_DIR}/geometry-compute/${name}")
    set(configure_args
        -S "${repository}/libs/GeometryCompute" -B "${build}"
        -G "${CMAKE_GENERATOR}"
        "-DCMAKE_BUILD_TYPE=$<CONFIG>"
        "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        -DBUILD_TESTING=ON
        "-DMATTER_GEOMETRY_COMPUTE=${backend}"
        "-DMATTER_COMPUTE_VULKAN=${backend}")
    foreach(variable CMAKE_C_COMPILER CMAKE_MAKE_PROGRAM
            CMAKE_GENERATOR_PLATFORM CMAKE_GENERATOR_TOOLSET
            Vulkan_INCLUDE_DIR Vulkan_LIBRARY Python3_EXECUTABLE)
        if(DEFINED ${variable} AND NOT "${${variable}}" STREQUAL "")
            list(APPEND configure_args "-D${variable}=${${variable}}")
        endif()
    endforeach()
    if(glslc)
        list(APPEND configure_args "-DMATTER_COMPUTE_GLSLC=${glslc}")
    endif()
    get_property(multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(multi_config)
        set(binary "${build}/$<CONFIG>/matter_geometry_compute_tests${CMAKE_EXECUTABLE_SUFFIX}")
    else()
        set(binary "${build}/matter_geometry_compute_tests${CMAKE_EXECUTABLE_SUFFIX}")
    endif()
    add_custom_target(${name} ALL
        COMMAND "${CMAKE_COMMAND}" ${configure_args}
        COMMAND "${CMAKE_COMMAND}" --build "${build}" --config "$<CONFIG>" --parallel 1
        BYPRODUCTS "${binary}"
        USES_TERMINAL VERBATIM)
    add_test(NAME ${name} COMMAND "${binary}")
    set_tests_properties(${name} PROPERTIES LABELS "${tier};geometry-compute"
        TIMEOUT 120)
    if(tier STREQUAL "gpu")
        set_tests_properties(${name} PROPERTIES RESOURCE_LOCK matter_gpu)
    else()
        set_property(GLOBAL APPEND PROPERTY MATTER_ENGINE_CPU_TARGETS ${name})
    endif()
endfunction()
