# Binding-owned private adapters: reuse canonical scalar math/declarations at
# configure time. No fake SSE types, SIMD emulation or checked-in algorithm copy.
set(_scalar_headers "${CMAKE_CURRENT_BINARY_DIR}/scalar-headers")
file(MAKE_DIRECTORY "${_scalar_headers}")
get_filename_component(_repo "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
function(read_scalar_source relative output)
    set(path "${_repo}/${relative}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    file(READ "${path}" source)
    set(${output} "${source}" PARENT_SCOPE)
endfunction()
read_scalar_source("libs/SpatialQueryLib/include/precomp.h" _precomp)
string(FIND "${_precomp}" "#include <immintrin.h>" _intrinsics)
if(_intrinsics LESS 0)
    message(FATAL_ERROR "Scalar precomp seam changed; audit the header")
endif()
string(REPLACE "#include <immintrin.h>" "" _precomp "${_precomp}")
string(REPLACE "#include <thread>" "" _precomp "${_precomp}")
string(REPLACE "#include <fstream>" "" _precomp "${_precomp}")
file(WRITE "${_scalar_headers}/precomp.h" "${_precomp}")
read_scalar_source("libs/SpatialQueryLib/include/tri.h" _tri)
string(FIND "${_tri}" "struct MATTER_ALIGN(64) Tri\n{" _tri_begin)
string(FIND "${_tri}" "}; // total size: 64 bytes\n};" _tri_end)
if(_tri_begin LESS 0 OR _tri_end LESS_EQUAL _tri_begin)
    message(FATAL_ERROR "Scalar Tri seam changed; audit the POD layout")
endif()
string(LENGTH "}; // total size: 64 bytes\n};" _tail_length)
math(EXPR _tri_end "${_tri_end}+${_tail_length}")
string(SUBSTRING "${_tri}" 0 ${_tri_begin} _prefix)
string(SUBSTRING "${_tri}" ${_tri_end} -1 _suffix)
file(WRITE "${_scalar_headers}/tri.h" "${_prefix}
// Scalar CPU interchange; SIMD aliases deliberately do not exist.
struct MATTER_ALIGN(64) Tri {
    float3 vertex0; float pad0;
    float3 vertex1; float pad1;
    float3 vertex2; float pad2;
    float3 centroid; float pad3;
};
static_assert(sizeof(Tri)==64 && alignof(Tri)==64);
${_suffix}
#include <cstddef>
static_assert(sizeof(TriEx)==96 && sizeof(float3)==12);
static_assert(offsetof(Tri,vertex1)==16 && offsetof(Tri,vertex2)==32 && offsetof(Tri,centroid)==48);
")
target_include_directories(matter_geometry BEFORE PRIVATE "${_scalar_headers}")
