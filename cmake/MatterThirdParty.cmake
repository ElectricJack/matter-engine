include_guard(GLOBAL)

function(matter_configure_vendor_target target)
    set_target_properties("${target}" PROPERTIES
        C_STANDARD 17
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    target_compile_options("${target}" PRIVATE /utf-8 /w)
endfunction()

matter_read_manifest("${CMAKE_SOURCE_DIR}/cmake/manifests/vendor.sources" matter_vendor_sources
    PLATFORM windows)

set(matter_quickjs_sources ${matter_vendor_sources})
list(FILTER matter_quickjs_sources INCLUDE REGEX "^third_party/quickjs-ng/.*\\.c$")
add_library(matter_quickjs STATIC ${matter_quickjs_sources})
target_include_directories(matter_quickjs PUBLIC "${CMAKE_SOURCE_DIR}/third_party/quickjs-ng")
target_compile_definitions(matter_quickjs PRIVATE
    CONFIG_VERSION="0.10.0"
    WIN32_LEAN_AND_MEAN
)
matter_configure_vendor_target(matter_quickjs)

set(matter_flecs_sources ${matter_vendor_sources})
list(FILTER matter_flecs_sources INCLUDE REGEX "^third_party/flecs/flecs\\.c$")
add_library(matter_flecs STATIC ${matter_flecs_sources})
target_include_directories(matter_flecs PUBLIC "${CMAKE_SOURCE_DIR}/third_party/flecs")
target_link_libraries(matter_flecs PUBLIC ws2_32 dbghelp)
matter_configure_vendor_target(matter_flecs)

set(matter_box3d_sources
    third_party/box3d/src/aabb.c
    third_party/box3d/src/arena_allocator.c
    third_party/box3d/src/bitset.c
    third_party/box3d/src/block_allocator.c
    third_party/box3d/src/body.c
    third_party/box3d/src/broad_phase.c
    third_party/box3d/src/capsule.c
    third_party/box3d/src/compound.c
    third_party/box3d/src/constraint_graph.c
    third_party/box3d/src/contact.c
    third_party/box3d/src/contact_solver.c
    third_party/box3d/src/convex_manifold.c
    third_party/box3d/src/core.c
    third_party/box3d/src/distance.c
    third_party/box3d/src/distance_joint.c
    third_party/box3d/src/dynamic_tree.c
    third_party/box3d/src/height_field.c
    third_party/box3d/src/hull.c
    third_party/box3d/src/id_pool.c
    third_party/box3d/src/island.c
    third_party/box3d/src/joint.c
    third_party/box3d/src/manifold.c
    third_party/box3d/src/math_functions.c
    third_party/box3d/src/mesh.c
    third_party/box3d/src/mesh_contact.c
    third_party/box3d/src/motor_joint.c
    third_party/box3d/src/mover.c
    third_party/box3d/src/parallel_for.c
    third_party/box3d/src/parallel_joint.c
    third_party/box3d/src/physics_world.c
    third_party/box3d/src/prismatic_joint.c
    third_party/box3d/src/recording.c
    third_party/box3d/src/recording_replay.c
    third_party/box3d/src/revolute_joint.c
    third_party/box3d/src/scheduler.c
    third_party/box3d/src/sensor.c
    third_party/box3d/src/shape.c
    third_party/box3d/src/simd.c
    third_party/box3d/src/solver.c
    third_party/box3d/src/solver_set.c
    third_party/box3d/src/sphere.c
    third_party/box3d/src/spherical_joint.c
    third_party/box3d/src/table.c
    third_party/box3d/src/timer.c
    third_party/box3d/src/triangle_manifold.c
    third_party/box3d/src/types.c
    third_party/box3d/src/weld_joint.c
    third_party/box3d/src/wheel_joint.c
    third_party/box3d/src/world_snapshot.c
)
add_library(matter_box3d STATIC ${matter_box3d_sources})
target_include_directories(matter_box3d
    PUBLIC "${CMAKE_SOURCE_DIR}/third_party/box3d/include"
    PRIVATE "${CMAKE_SOURCE_DIR}/third_party/box3d/src"
)
target_compile_definitions(matter_box3d PUBLIC "$<$<CONFIG:RELWITHDEBINFO>:B3_ENABLE_ASSERT>")
matter_configure_vendor_target(matter_box3d)

set(matter_glfw_sources ${matter_vendor_sources})
list(FILTER matter_glfw_sources INCLUDE REGEX "^third_party/raylib/src/external/glfw/src/.*\\.c$")
list(APPEND matter_glfw_sources MatterEditor/src/glfw_vulkan_only_context.c)
add_library(matter_glfw STATIC ${matter_glfw_sources})
target_include_directories(matter_glfw
    PUBLIC "${CMAKE_SOURCE_DIR}/third_party/raylib/src/external/glfw/include"
    PRIVATE "${CMAKE_SOURCE_DIR}/third_party/raylib/src/external/glfw/src"
)
target_compile_definitions(matter_glfw PRIVATE _GLFW_WIN32)
target_link_libraries(matter_glfw PUBLIC gdi32 winmm user32 shell32)
matter_configure_vendor_target(matter_glfw)

set(matter_ozz_base_sources
    third_party/ozz-animation/src/base/memory/allocator.cc
    third_party/ozz-animation/src/base/platform.cc
    third_party/ozz-animation/src/base/log.cc
    third_party/ozz-animation/src/base/containers/string_archive.cc
    third_party/ozz-animation/src/base/encode/group_varint.cc
    third_party/ozz-animation/src/base/io/archive.cc
    third_party/ozz-animation/src/base/io/stream.cc
    third_party/ozz-animation/src/base/maths/box.cc
    third_party/ozz-animation/src/base/maths/simd_math.cc
    third_party/ozz-animation/src/base/maths/math_archive.cc
    third_party/ozz-animation/src/base/maths/soa_math_archive.cc
    third_party/ozz-animation/src/base/maths/simd_math_archive.cc
)
add_library(matter_ozz_base STATIC ${matter_ozz_base_sources})
target_include_directories(matter_ozz_base
    PUBLIC "${CMAKE_SOURCE_DIR}/third_party/ozz-animation/include"
    PRIVATE "${CMAKE_SOURCE_DIR}/third_party/ozz-animation/src"
)
target_compile_definitions(matter_ozz_base PRIVATE _CRT_SECURE_NO_WARNINGS)
matter_configure_vendor_target(matter_ozz_base)

set(matter_ozz_animation_sources
    third_party/ozz-animation/src/animation/runtime/animation.cc
    third_party/ozz-animation/src/animation/runtime/animation_utils.cc
    third_party/ozz-animation/src/animation/runtime/blending_job.cc
    third_party/ozz-animation/src/animation/runtime/ik_aim_job.cc
    third_party/ozz-animation/src/animation/runtime/ik_two_bone_job.cc
    third_party/ozz-animation/src/animation/runtime/local_to_model_job.cc
    third_party/ozz-animation/src/animation/runtime/motion_blending_job.cc
    third_party/ozz-animation/src/animation/runtime/sampling_job.cc
    third_party/ozz-animation/src/animation/runtime/skeleton.cc
    third_party/ozz-animation/src/animation/runtime/skeleton_utils.cc
    third_party/ozz-animation/src/animation/runtime/track.cc
    third_party/ozz-animation/src/animation/runtime/track_sampling_job.cc
    third_party/ozz-animation/src/animation/runtime/track_triggering_job.cc
)
add_library(matter_ozz_animation STATIC ${matter_ozz_animation_sources})
target_include_directories(matter_ozz_animation PRIVATE "${CMAKE_SOURCE_DIR}/third_party/ozz-animation/src")
target_link_libraries(matter_ozz_animation PUBLIC matter_ozz_base)
matter_configure_vendor_target(matter_ozz_animation)

set(matter_ozz_offline_sources
    third_party/ozz-animation/src/animation/offline/raw_animation.cc
    third_party/ozz-animation/src/animation/offline/raw_animation_archive.cc
    third_party/ozz-animation/src/animation/offline/raw_animation_utils.cc
    third_party/ozz-animation/src/animation/offline/animation_builder.cc
    third_party/ozz-animation/src/animation/offline/animation_optimizer.cc
    third_party/ozz-animation/src/animation/offline/additive_animation_builder.cc
    third_party/ozz-animation/src/animation/offline/motion_extractor.cc
    third_party/ozz-animation/src/animation/offline/raw_skeleton.cc
    third_party/ozz-animation/src/animation/offline/raw_skeleton_archive.cc
    third_party/ozz-animation/src/animation/offline/skeleton_builder.cc
    third_party/ozz-animation/src/animation/offline/raw_track.cc
    third_party/ozz-animation/src/animation/offline/raw_track_utils.cc
    third_party/ozz-animation/src/animation/offline/track_builder.cc
    third_party/ozz-animation/src/animation/offline/track_optimizer.cc
)
add_library(matter_ozz_offline STATIC ${matter_ozz_offline_sources})
target_include_directories(matter_ozz_offline PRIVATE "${CMAKE_SOURCE_DIR}/third_party/ozz-animation/src")
target_link_libraries(matter_ozz_offline PUBLIC matter_ozz_animation)
matter_configure_vendor_target(matter_ozz_offline)

set(matter_autoremesher_root "${CMAKE_SOURCE_DIR}/third_party/autoremesher_core")
set(matter_autoremesher_sources
    ${matter_autoremesher_root}/src/remesh.cpp
    ${matter_autoremesher_root}/src/autoremesher.cpp
    ${matter_autoremesher_root}/src/mesh_sanitize.cpp
    ${matter_autoremesher_root}/src/mesh_separator.cpp
    ${matter_autoremesher_root}/src/param_hdc.cpp
    ${matter_autoremesher_root}/src/position_key.cpp
    ${matter_autoremesher_root}/src/quad_extract.cpp
    ${matter_autoremesher_root}/src/quad_to_tri.cpp
    ${matter_autoremesher_root}/src/nl_ext_stubs.c
)
set(matter_geogram_root "${matter_autoremesher_root}/thirdparty/geogram/src/lib/geogram")
foreach(directory IN ITEMS . basic api mesh delaunay voronoi points numerics image parameterization bibliography NL)
    file(GLOB directory_sources CONFIGURE_DEPENDS
        "${matter_geogram_root}/${directory}/*.cpp"
        "${matter_geogram_root}/${directory}/*.c"
    )
    list(APPEND matter_autoremesher_sources ${directory_sources})
endforeach()
list(FILTER matter_autoremesher_sources EXCLUDE REGEX "/NL/nl_amgcl\\.cpp$")
set(matter_exploragram_root "${matter_autoremesher_root}/thirdparty/geogram/src/lib/exploragram")
foreach(directory IN ITEMS . hexdom optimal_transport)
    file(GLOB directory_sources CONFIGURE_DEPENDS "${matter_exploragram_root}/${directory}/*.cpp")
    list(APPEND matter_autoremesher_sources ${directory_sources})
endforeach()
file(GLOB matter_isotropic_sources CONFIGURE_DEPENDS
    "${matter_autoremesher_root}/thirdparty/isotropicremesher/*.cpp")
file(GLOB matter_zlib_sources CONFIGURE_DEPENDS
    "${matter_geogram_root}/third_party/zlib/*.c")
list(APPEND matter_autoremesher_sources
    ${matter_isotropic_sources}
    ${matter_geogram_root}/third_party/libMeshb/sources/libmeshb7.c
    ${matter_geogram_root}/third_party/rply/rply.c
    ${matter_zlib_sources}
)

set(matter_nl_superlu_source "${matter_geogram_root}/NL/nl_superlu.c")
set(matter_nl_cholmod_source "${matter_geogram_root}/NL/nl_cholmod.c")
set(matter_nl_mkl_source "${matter_geogram_root}/NL/nl_mkl.c")
set(matter_nl_cuda_source "${matter_geogram_root}/NL/nl_cuda.c")
set_source_files_properties("${matter_nl_superlu_source}" PROPERTIES COMPILE_DEFINITIONS
    "nlInitExtension_SUPERLU=matter_unused_real_nlInitExtension_SUPERLU;nlExtensionIsInitialized_SUPERLU=matter_unused_real_nlExtensionIsInitialized_SUPERLU;nlMatrixFactorize_SUPERLU=matter_unused_real_nlMatrixFactorize_SUPERLU"
)
set_source_files_properties("${matter_nl_cholmod_source}" PROPERTIES COMPILE_DEFINITIONS
    "nlInitExtension_CHOLMOD=matter_unused_real_nlInitExtension_CHOLMOD;nlExtensionIsInitialized_CHOLMOD=matter_unused_real_nlExtensionIsInitialized_CHOLMOD;nlMatrixFactorize_CHOLMOD=matter_unused_real_nlMatrixFactorize_CHOLMOD"
)
set_source_files_properties("${matter_nl_mkl_source}" PROPERTIES COMPILE_DEFINITIONS
    "nlInitExtension_MKL=matter_unused_real_nlInitExtension_MKL;nlExtensionIsInitialized_MKL=matter_unused_real_nlExtensionIsInitialized_MKL;NLMultMatrixVector_MKL=matter_unused_real_NLMultMatrixVector_MKL;nlMKLMatrixNewFromCRSMatrix=matter_unused_real_nlMKLMatrixNewFromCRSMatrix;nlMKLMatrixNewFromSparseMatrix=matter_unused_real_nlMKLMatrixNewFromSparseMatrix"
)
set_source_files_properties("${matter_nl_cuda_source}" PROPERTIES COMPILE_DEFINITIONS
    "nlInitExtension_CUDA=matter_unused_real_nlInitExtension_CUDA;nlExtensionIsInitialized_CUDA=matter_unused_real_nlExtensionIsInitialized_CUDA;nlCUDABlas=matter_unused_real_nlCUDABlas;nlCUDAJacobiPreconditionerNewFromCRSMatrix=matter_unused_real_nlCUDAJacobiPreconditionerNewFromCRSMatrix;nlCUDAMatrixNewFromCRSMatrix=matter_unused_real_nlCUDAMatrixNewFromCRSMatrix"
)

add_library(matter_autoremesher STATIC ${matter_autoremesher_sources})
target_include_directories(matter_autoremesher
    PUBLIC "${matter_autoremesher_root}/include"
    PRIVATE
        "${matter_autoremesher_root}/thirdparty/eigen"
        "${matter_autoremesher_root}/thirdparty/geogram"
        "${matter_autoremesher_root}/thirdparty/compat_include"
        "${matter_autoremesher_root}/thirdparty/geogram/src/lib"
        "${matter_geogram_root}/third_party/libMeshb/sources"
        "${matter_geogram_root}/third_party/rply"
        "${matter_geogram_root}/third_party/zlib"
        "${matter_autoremesher_root}/thirdparty/isotropicremesher"
        "${matter_autoremesher_root}/thirdparty/tbb_shim/include"
)
target_compile_definitions(matter_autoremesher PRIVATE
    _USE_MATH_DEFINES
    GEOGRAM_WITH_PDEL
    AUTOREMESHER_FORCE_SHIM
)
target_compile_options(matter_autoremesher PRIVATE
    "/FI${CMAKE_SOURCE_DIR}/cmake/MatterAutoremesherConfig.h"
)
matter_configure_vendor_target(matter_autoremesher)

set(matter_imgui_sources ${matter_vendor_sources})
list(FILTER matter_imgui_sources INCLUDE REGEX "^third_party/imgui/.*\\.cpp$")
add_library(matter_imgui STATIC ${matter_imgui_sources})
target_include_directories(matter_imgui PUBLIC
    "${CMAKE_SOURCE_DIR}/third_party/imgui"
    "${CMAKE_SOURCE_DIR}/third_party/imgui/backends"
    "${CMAKE_SOURCE_DIR}/third_party/Vulkan-Headers/include"
)
set(matter_vulkan_loader "C:/VulkanSDK/1.4.357.0/Lib/vulkan-1.lib")
if(NOT EXISTS "${matter_vulkan_loader}")
    message(FATAL_ERROR "Pinned Vulkan loader import library not found: ${matter_vulkan_loader}")
endif()
target_link_libraries(matter_imgui PUBLIC matter_glfw "${matter_vulkan_loader}")
matter_configure_vendor_target(matter_imgui)

set(matter_imguizmo_sources ${matter_vendor_sources})
list(FILTER matter_imguizmo_sources INCLUDE REGEX "^third_party/ImGuizmo/.*\\.cpp$")
add_library(matter_imguizmo STATIC ${matter_imguizmo_sources})
target_include_directories(matter_imguizmo PUBLIC "${CMAKE_SOURCE_DIR}/third_party/ImGuizmo")
target_link_libraries(matter_imguizmo PUBLIC matter_imgui)
matter_configure_vendor_target(matter_imguizmo)

set(matter_bc7enc_sources ${matter_vendor_sources})
list(FILTER matter_bc7enc_sources INCLUDE REGEX "^third_party/bc7enc/.*\\.cpp$")
add_library(matter_bc7enc STATIC ${matter_bc7enc_sources})
target_include_directories(matter_bc7enc PUBLIC "${CMAKE_SOURCE_DIR}/third_party/bc7enc")
matter_configure_vendor_target(matter_bc7enc)

set(matter_third_party_targets
    matter_quickjs
    matter_flecs
    matter_box3d
    matter_glfw
    matter_ozz_base
    matter_ozz_animation
    matter_ozz_offline
    matter_autoremesher
    matter_imgui
    matter_imguizmo
    matter_bc7enc
)
add_custom_target(matter_third_party)
add_dependencies(matter_third_party ${matter_third_party_targets})
