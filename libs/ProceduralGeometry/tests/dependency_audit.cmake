include("${GRAPH}")
if(NOT geometry_links STREQUAL "matter_contracts" OR
   NOT geometry_public_links STREQUAL "matter_contracts" OR
   NOT recipe_links STREQUAL "matter_geometry")
    message(FATAL_ERROR "Geometry/direct recipe acquired an unexpected library dependency")
endif()
set(public_includes)
foreach(path IN LISTS geometry_public_includes)
    cmake_path(NORMAL_PATH path OUTPUT_VARIABLE normal)
    list(APPEND public_includes "${normal}")
endforeach()
set(expected_includes "${module_root}/include" "${repo_root}/libs/MatterContracts/include"
    "${repo_root}/libs/MathLib/include")
if(NOT public_includes STREQUAL expected_includes)
    message(FATAL_ERROR "Legacy or non-neutral includes leaked through the geometry public target")
endif()
set(expected_sources src/geometry.cpp src/profile.cpp src/session.cpp src/csg.cpp src/csg_mesh.cpp
    src/query.cpp src/modifiers.cpp
    src/triangle_emit.cpp src/polygon_triangulate.cpp
    "${repo_root}/libs/MatterSurfaceLib/src/fat_primitive.c")
if(modifiers)
    foreach(stage mesh_indexed mesh_simplifier mesh_smooth mesh_transform)
        list(APPEND expected_sources "${repo_root}/libs/MatterSurfaceLib/src/${stage}.cpp")
    endforeach()
endif()
if(mesh_worker)
    list(APPEND expected_sources src/mesh_worker.cpp)
endif()
if(NOT geometry_sources STREQUAL expected_sources)
    message(FATAL_ERROR "Geometry acquired a source outside the primitive/profile/CSG/query/modifier extraction")
endif()
file(READ "${module_root}/include/matter/geometry/geometry.h" header)
if(header MATCHES "raylib|triangle_emit|part_asset|dsl_state|Vulkan|quickjs|terrain")
    message(FATAL_ERROR "Non-neutral public header dependency")
endif()
file(READ "${module_root}/src/triangle_emit.cpp" shared)
if(shared MATCHES "VariationRecorder::instance|compute_resolved_hash")
    message(FATAL_ERROR "Cache/world backend entered the shared geometry implementation")
endif()
if(msvc)
    execute_process(COMMAND "${native_linker}" /dump /symbols "${geometry_archive}"
        RESULT_VARIABLE status OUTPUT_VARIABLE symbols ERROR_VARIABLE errors)
else()
    execute_process(COMMAND "${symbol_tool}" -C "${geometry_archive}"
        RESULT_VARIABLE status OUTPUT_VARIABLE symbols ERROR_VARIABLE errors)
endif()
if(NOT status EQUAL 0)
    message(FATAL_ERROR "Cannot inspect geometry symbols: ${errors}")
endif()
# MSVC /dump /symbols lists C string-literal constants as `??_C@....  (`string')`,
# so words inside diagnostic messages and capability names land here: the
# "mesh.retopology" capability name (modifiers.cpp) and the "mesh retopology
# has no supported backend" error text both emit `string' symbols. The broad
# backend-keyword checks below would then mistake those explicit
# unsupported-operation diagnostic strings for linked backend code. Drop every
# string-literal symbol line first; a genuinely linked backend function/class
# emits a mangled C++ definition (e.g. `??_Z?remesh@autoremesher@@...`) that is
# not a `string' symbol, so it survives the filter and is still detected below.
# Non-MSVC `nm -C` output has no `string' annotation, so the filter is a no-op there.
string(REGEX REPLACE "\n[^\n]*\\(`string'\\)" "" symbols "${symbols}")
string(REGEX REPLACE "^[^\n]*\\(`string'\\)"  "" symbols "${symbols}")
if(symbols MATCHES "compute_resolved_hash|DslState|JS_Eval|vkCreate|BeginDrawing|TerrainGenerator")
    message(FATAL_ERROR "Headless archive contains a host/renderer/terrain/cache runtime symbol")
endif()
# Retopology needs process-global geogram/TBB and subprocess isolation; it must
# not ride into this archive. OFF must drop the simplify/smooth stages entirely.
if(symbols MATCHES "autoremesher|geogram|retopo|tbb::|GEO::")
    message(FATAL_ERROR "Headless archive contains a retopology backend symbol")
endif()
if(NOT modifiers AND symbols MATCHES "reproject_triex|SimplifyOptions|SmoothOptions")
    message(FATAL_ERROR "Modifier backend linked into a build that disabled it")
endif()
message(STATUS "GEOM-INDEPENDENT: neutral public includes, canonical sources, contracts-only link, no runtime backend symbols")
