# Included by the optional physics module; no central registration here.
add_library(matter_collision_terrain_adapter STATIC terrain.cpp)
target_include_directories(matter_collision_terrain_adapter PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(matter_collision_terrain_adapter PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../.." "${CMAKE_CURRENT_SOURCE_DIR}/../../../include")
target_link_libraries(matter_collision_terrain_adapter PUBLIC matter_collision)
