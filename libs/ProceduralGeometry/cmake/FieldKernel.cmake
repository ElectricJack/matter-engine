# The CPU field/surface tape kernel, registered alone so terrain and the native
# asset surface producer can link it without configuring the rest of geometry
# (no canonical surface-math extraction, no modifier backend). Every graph
# registers it once, so the kernel is compiled once whoever includes this first.
if(NOT TARGET matter_geometry_field)
    get_filename_component(_field_module "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
    add_library(matter_geometry_field STATIC "${_field_module}/src/terrain_field.cpp")
    target_include_directories(matter_geometry_field PUBLIC "${_field_module}/include")
    target_compile_features(matter_geometry_field PUBLIC cxx_std_17)
    if(MSVC)
        target_compile_options(matter_geometry_field PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(matter_geometry_field PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()
