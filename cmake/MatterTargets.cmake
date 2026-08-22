include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/ManifestSources.cmake")

function(matter_manifest_sources manifest prefix out_var)
    matter_read_manifest("${CMAKE_SOURCE_DIR}/cmake/manifests/${manifest}" manifest_sources
        PLATFORM windows)
    list(FILTER manifest_sources INCLUDE REGEX "^${prefix}")
    if(NOT manifest_sources)
        message(FATAL_ERROR "${manifest} contains no sources beneath ${prefix}")
    endif()
    set(${out_var} "${manifest_sources}" PARENT_SCOPE)
endfunction()

function(matter_configure_library target include_directory)
    target_include_directories("${target}" PUBLIC "${CMAKE_SOURCE_DIR}/${include_directory}")
    matter_apply_project_defaults("${target}")
endfunction()

add_library(matter_memory STATIC
    libs/MemoryLib/src/mem_arena.c
    libs/MemoryLib/src/mem_array.c
    libs/MemoryLib/src/mem_pool.c
)
matter_configure_library(matter_memory libs/MemoryLib/include)

add_library(matter_math INTERFACE)
target_include_directories(matter_math INTERFACE "${CMAKE_SOURCE_DIR}/libs/MathLib/include")
matter_apply_project_defaults(matter_math)

matter_manifest_sources(matter-surface.sources "libs/SpatialQueryLib/src/" spatial_sources)
add_library(matter_spatial STATIC ${spatial_sources})
matter_configure_library(matter_spatial libs/SpatialQueryLib/include)
target_link_libraries(matter_spatial PUBLIC matter_memory)

matter_manifest_sources(vendor.sources "libs/ProfileLib/src/" profile_sources)
add_library(matter_profile STATIC ${profile_sources})
matter_configure_library(matter_profile libs/ProfileLib/include)

matter_manifest_sources(engine-core.sources "libs/ParticleFlowLib/src/" particle_flow_sources)
add_library(matter_particle_flow STATIC ${particle_flow_sources})
matter_configure_library(matter_particle_flow libs/ParticleFlowLib/include)

matter_manifest_sources(matter-surface.sources "libs/MeshChartingLib/src/" mesh_charting_sources)
add_library(matter_mesh_charting STATIC ${mesh_charting_sources})
matter_configure_library(matter_mesh_charting libs/MeshChartingLib/include)

add_library(matter_asset_store STATIC
    libs/AssetStoreLib/src/store_hash.cpp
    libs/AssetStoreLib/src/store_os.cpp
    libs/AssetStoreLib/src/blob_store.cpp
    libs/AssetStoreLib/src/ref_table.cpp
)
matter_configure_library(matter_asset_store libs/AssetStoreLib/include)
target_include_directories(matter_asset_store PRIVATE "${CMAKE_SOURCE_DIR}/libs/AssetStoreLib/src")
target_link_libraries(matter_asset_store PUBLIC matter_memory)

set(foundation_targets
    matter_memory
    matter_math
    matter_spatial
    matter_profile
    matter_particle_flow
    matter_mesh_charting
    matter_asset_store
)

if(BUILD_TESTING)
    add_executable(matter_memory_tests libs/MemoryLib/tests/memory_tests.c)
    target_link_libraries(matter_memory_tests PRIVATE matter_memory)
    matter_apply_project_defaults(matter_memory_tests)
    add_test(NAME matter_memory_tests COMMAND matter_memory_tests)

    add_executable(matter_memory_hpp_tests libs/MemoryLib/tests/memory_hpp_tests.cpp)
    target_link_libraries(matter_memory_hpp_tests PRIVATE matter_memory)
    matter_apply_project_defaults(matter_memory_hpp_tests)
    add_test(NAME matter_memory_hpp_tests COMMAND matter_memory_hpp_tests)

    add_executable(matter_math_tests libs/MathLib/tests/mathlib_tests.cpp)
    target_include_directories(matter_math_tests PRIVATE "${CMAKE_SOURCE_DIR}/MatterEngine3/include")
    target_link_libraries(matter_math_tests PRIVATE matter_math)
    matter_apply_project_defaults(matter_math_tests)
    add_test(NAME matter_math_tests COMMAND matter_math_tests)

    add_executable(matter_math_c_smoke libs/MathLib/tests/matter_math_c_smoke.c)
    target_link_libraries(matter_math_c_smoke PRIVATE matter_math)
    matter_apply_project_defaults(matter_math_c_smoke)
    add_test(NAME matter_math_c_smoke COMMAND matter_math_c_smoke)

    add_executable(matter_spatial_tests libs/SpatialQueryLib/tests/spatial_hash_tests.c)
    target_link_libraries(matter_spatial_tests PRIVATE matter_spatial)
    matter_apply_project_defaults(matter_spatial_tests)
    add_test(NAME matter_spatial_tests COMMAND matter_spatial_tests)

    add_executable(matter_profile_tests libs/ProfileLib/tests/profile_tests.cpp)
    target_link_libraries(matter_profile_tests PRIVATE matter_profile)
    matter_apply_project_defaults(matter_profile_tests)
    add_test(NAME matter_profile_tests COMMAND matter_profile_tests)

    add_executable(matter_particle_flow_tests libs/ParticleFlowLib/tests/pf_tests.cpp)
    target_link_libraries(matter_particle_flow_tests PRIVATE matter_particle_flow)
    matter_apply_project_defaults(matter_particle_flow_tests)
    add_test(NAME matter_particle_flow_tests COMMAND matter_particle_flow_tests)

    add_executable(matter_mesh_charting_tests libs/MeshChartingLib/tests/mesh_charting_tests.cpp)
    target_link_libraries(matter_mesh_charting_tests PRIVATE matter_mesh_charting)
    matter_apply_project_defaults(matter_mesh_charting_tests)
    add_test(NAME matter_mesh_charting_tests COMMAND matter_mesh_charting_tests)

    add_executable(matter_asset_store_tests libs/AssetStoreLib/tests/asset_store_tests.cpp)
    target_include_directories(matter_asset_store_tests PRIVATE "${CMAKE_SOURCE_DIR}/libs/AssetStoreLib/src")
    target_link_libraries(matter_asset_store_tests PRIVATE matter_asset_store)
    matter_apply_project_defaults(matter_asset_store_tests)
    add_test(NAME matter_asset_store_tests COMMAND matter_asset_store_tests)

    list(APPEND foundation_targets
        matter_memory_tests
        matter_memory_hpp_tests
        matter_math_tests
        matter_math_c_smoke
        matter_spatial_tests
        matter_profile_tests
        matter_particle_flow_tests
        matter_mesh_charting_tests
        matter_asset_store_tests
    )
    set_tests_properties(
        matter_memory_tests
        matter_memory_hpp_tests
        matter_math_tests
        matter_math_c_smoke
        matter_spatial_tests
        matter_profile_tests
        matter_particle_flow_tests
        matter_mesh_charting_tests
        matter_asset_store_tests
        PROPERTIES LABELS foundation
    )
    foreach(test_target IN LISTS foundation_targets)
        if(test_target MATCHES "_tests$" OR test_target STREQUAL "matter_math_c_smoke")
            matter_apply_test_assertion_policy("${test_target}")
        endif()
    endforeach()
endif()

add_custom_target(matter_foundation)
add_dependencies(matter_foundation ${foundation_targets})
