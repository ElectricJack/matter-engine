# Use the canonical vendor manifest without loading the editor's other vendors
# or changing central build ownership. Like the shared_surface_math.h extraction, this is a
# guarded configure-time extraction, not a second checked-in implementation.
if(NOT TARGET matter_autoremesher)
    set(_manifest "${MATTER_GEOMETRY_ROOT}/cmake/MatterThirdParty.cmake")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_manifest}")
    file(READ "${_manifest}" _text)
    string(FIND "${_text}" "set(matter_autoremesher_root " _start)
    string(FIND "${_text}" "matter_configure_vendor_target(matter_autoremesher)" _end)
    if(_start LESS 0 OR _end LESS_EQUAL _start)
        message(FATAL_ERROR "Canonical autoremesher manifest seam changed; audit worker build")
    endif()
    math(EXPR _length "${_end} - ${_start}")
    string(SUBSTRING "${_text}" ${_start} ${_length} _text)
    string(REPLACE "\${CMAKE_SOURCE_DIR}" "${MATTER_GEOMETRY_ROOT}" _text "${_text}")
    # The root manifest is MSVC-only. Preserve its assertion policy on Unix,
    # and the legacy Makefile's forced cstdint include for newer GCC headers.
    string(FIND "${_text}" "target_compile_options(matter_autoremesher PRIVATE" _flags)
    if(_flags LESS 0)
        message(FATAL_ERROR "Canonical autoremesher compiler flags changed")
    endif()
    string(SUBSTRING "${_text}" 0 ${_flags} _text)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/mesh_worker_vendor.cmake" "${_text}")
    include("${CMAKE_CURRENT_BINARY_DIR}/mesh_worker_vendor.cmake")
    target_compile_features(matter_autoremesher PRIVATE cxx_std_17)
    target_compile_options(matter_autoremesher PRIVATE
        -include "${MATTER_GEOMETRY_ROOT}/cmake/MatterAutoremesherConfig.h"
        "$<$<COMPILE_LANGUAGE:CXX>:-includecstdint>" -w)
endif()
find_package(Threads REQUIRED)
add_library(matter_geometry_worker_backend STATIC
    "${MATTER_GEOMETRY_ROOT}/libs/MatterSurfaceLib/src/mesh_retopo.cpp"
    "${MATTER_GEOMETRY_ROOT}/libs/MatterSurfaceLib/src/mesh_indexed.cpp"
    "${MATTER_GEOMETRY_ROOT}/libs/MatterSurfaceLib/src/mesh_transform.cpp")
target_compile_features(matter_geometry_worker_backend PUBLIC cxx_std_17)
target_compile_definitions(matter_geometry_worker_backend PRIVATE MATTER_VULKAN_ONLY)
target_include_directories(matter_geometry_worker_backend PUBLIC
    "${MATTER_GEOMETRY_ROOT}/MatterEngine3/include"
    "${MATTER_GEOMETRY_ROOT}/libs/MatterSurfaceLib/include"
    "${MATTER_GEOMETRY_ROOT}/libs/SpatialQueryLib/include"
    "${MATTER_GEOMETRY_ROOT}/libs/MathLib/include"
    "${MATTER_GEOMETRY_ROOT}/third_party/raylib/src")
target_link_libraries(matter_geometry_worker_backend PRIVATE matter_autoremesher Threads::Threads ${CMAKE_DL_LIBS})
add_executable(matter_geometry_mesh_worker src/mesh_worker_main.cpp)
target_link_libraries(matter_geometry_mesh_worker PRIVATE matter_geometry_worker_backend)
target_sources(matter_geometry PRIVATE src/mesh_worker.cpp)
target_compile_definitions(matter_geometry PRIVATE MATTER_GEOMETRY_HAVE_MESH_WORKER=1)
