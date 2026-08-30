#pragma once

// Template, IGAD version 2 - adapted for MatterEngine2
// IGAD/NHTV/UU - Jacco Bikker - 2006-2021

// add your includes to this file instead of to individual .cpp files
// to enjoy the benefits of precompiled headers:
// - fast compilation
// - solve issues with the order of header files once (here)
// do not include headers in header files (ever).

// ---------------------------------------------------------------------------
// libs/SpatialQueryLib/include/precomp.h — what this is, in MatterEngine2
// ---------------------------------------------------------------------------
//
// Despite the name and the template preamble above, this is NOT used as a
// compiler precompiled header here. It is an ordinary header pulled in by
// `tri.h` (and therefore by `bvh.h` and by the ~15 engine translation units
// that name a `Tri`), so treat everything it defines as leaking repo-wide.
//
// What it provides:
//   - `ALIGN(x)` — the portable alignment attribute used by `Tri`, `BVHRay`,
//     `BVH` and `TLAS`.
//   - `MALLOC64`/`FREE64` — 64-byte-aligned alloc/free. Note `MALLOC64(0)`
//     returns null rather than a valid empty allocation, and on POSIX the
//     backing `aligned_alloc` requires the size to be a multiple of 64, which
//     is why callers round `n * sizeof(TriEx)` (96 bytes each) up before
//     allocating. Buffers from MALLOC64 must be released with FREE64.
//   - The `float2`/`float3`/`float4` POD vector types and their operators.
//
// Relationship to MathLib: `libs/MathLib` (`mm::Vec3`, `mm::Mat4`, ...) is the
// engine's canonical everyday math library. The types here are the separate,
// older SIMD/BVH interchange format — they exist because the BVH node and ray
// layouts depend on their exact sizes. Do not "unify" them; convert at the
// boundary instead.
//
// These types carry no coordinate-space meaning of their own. Whatever space a
// caller stores in a `float3` is the caller's convention.

// C++ headers
#include <chrono>
#include <fstream>
#include <vector>
#include <list>
#include <string>
#include <thread>
#include <math.h>
#include <algorithm>
#include <assert.h>

#include "matter/compiler.h"

// Keep standard-library names qualified. A using-directive in this public
// header makes the Windows SDK's global ::byte ambiguous with C++17
// std::byte whenever a consumer includes Win32 headers afterward.

// aligned memory allocations
//
// MATTER_MALLOC64_HOOK is a TEST-ONLY seam (defined by the partstore race
// harness's build flavor, never by production Makefiles): it routes MALLOC64 /
// FREE64 through a pair of extern hooks so a stress test can put guard pages
// under the BVH/mesh buffers. With the macro undefined this header is
// byte-identical in behavior to what it always was.
#ifdef MATTER_MALLOC64_HOOK
extern "C" void* matter_malloc64_hook( size_t bytes );
extern "C" void  matter_free64_hook( void* p );
#define MALLOC64( x ) ( ( x ) == 0 ? 0 : matter_malloc64_hook( ( x ) ) )
#define FREE64( x ) matter_free64_hook( x )
#endif
#ifdef _MSC_VER
#ifndef MATTER_MALLOC64_HOOK
#define MALLOC64( x ) ( ( x ) == 0 ? 0 : _aligned_malloc( ( x ), 64 ) )
#define FREE64( x ) _aligned_free( x )
#endif
#else
#ifndef MATTER_MALLOC64_HOOK
#ifdef _WIN32
    #define MALLOC64( x ) ( ( x ) == 0 ? 0 : _aligned_malloc( ( x ), 64 ) )
#else
    #define MALLOC64( x ) ( ( x ) == 0 ? 0 : aligned_alloc( 64, ( x ) ) )
#endif
#ifdef _WIN32
    #define FREE64( x ) _aligned_free( x )
#else
    #define FREE64( x ) free( x )
#endif
#endif
#endif

// Math constants for Windows compatibility
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// basic types
typedef unsigned char uchar;
typedef unsigned int uint;
typedef unsigned short ushort;

// These vector views intentionally use anonymous structs so named components
// and indexed storage share bytes. MSVC diagnoses
// that ABI extension as C4201; keep the exception local to these declarations.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4201)
#endif

// The vector types below are PODs with no default member initialisers: a bare
// `float3 v;` is uninitialised. They are also deliberately trivial (no user
// destructor, no virtuals) because `Tri`, `BVHNode`, `BVHRay` and `TLASNode`
// all place them inside anonymous unions alongside `__m128`; giving any of
// them a non-trivial member would break those unions.
//
// Note the asymmetry: int2/uint2/float2 are MATTER_ALIGN(8) and float4 is MATTER_ALIGN(16),
// but `float3` carries NO alignment attribute and is exactly 12 bytes. That is
// load-bearing, not an oversight — `BVHNode` and `TLASNode` rely on a
// `float3` leaving the fourth 4-byte slot of a 16-byte union free for a packed
// integer. Adding MATTER_ALIGN(16) to float3 would silently corrupt both layouts.
// OpenCL-STYLE vector types. int2/uint2/float2/float4 do match OpenCL's layout
// and alignment; `float3` deliberately does NOT — OpenCL's cl_float3 is a
// 16-byte, 16-byte-aligned type, and this one is 12 bytes with no alignment
// attribute, exactly so the BVH/TLAS node unions can reuse the fourth slot.
struct MATTER_ALIGN( 8 ) int2
{
	int2() = default;
	int2( const int a, const int b ) : x( a ), y( b ) {}
	int2( const int a ) : x( a ), y( a ) {}
	union { struct { int x, y; }; int cell[2]; };
	int& operator [] ( const int n ) { return cell[n]; }
};

struct MATTER_ALIGN( 8 ) uint2
{
	uint2() = default;
	uint2( const int a, const int b ) : x( a ), y( b ) {}
	uint2( const uint a ) : x( a ), y( a ) {}
	union { struct { uint x, y; }; uint cell[2]; };
	uint& operator [] ( const int n ) { return cell[n]; }
};

struct MATTER_ALIGN( 8 ) float2
{
	float2() = default;
	float2( const float a, const float b ) : x( a ), y( b ) {}
	float2( const float a ) : x( a ), y( a ) {}
	union { struct { float x, y; }; float cell[2]; };
	float& operator [] ( const int n ) { return cell[n]; }
};

// 12 bytes, unaligned, no constructors — see the note above the vector types.
// Construct with `make_float3(...)` or brace-init (`{x, y, z}`); the
// `cell[3]` overlay is how the BVH builder indexes by split axis.
//
// `operator[]` is non-const only, so it is unavailable on a `const float3&`.
// `TLAS::BuildRecursive` in `src/bvh.cpp` works around this with an explicit
// per-axis branch rather than an index.
struct float3
{
	union { struct { float x, y, z; }; float cell[3]; };
	float& operator [] ( const int n ) { return cell[n]; }
};

struct MATTER_ALIGN( 16 ) float4
{
	float4() = default;
	float4( const float a, const float b, const float c, const float d ) : x( a ), y( b ), z( c ), w( d ) {}
	float4( const float a ) : x( a ), y( a ), z( a ), w( a ) {}
	float4( const float3 & a, const float d ) : x( a.x ), y( a.y ), z( a.z ), w( d ) {}
	float4( const float3 & a ) : x( a.x ), y( a.y ), z( a.z ), w( 1.0f ) {}
	union { struct { float x, y, z, w; }; float cell[4]; };
	float& operator [] ( const int n ) { return cell[n]; }
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

// math functions
//
// `fminf`/`fmaxf` here are two-argument float overloads that sit alongside the
// `<math.h>` functions of the same name pulled in above; the `float3` overloads
// further down resolve to them componentwise. They are not NaN-propagating the
// way the C library versions are — `a < b ? a : b` returns `b` whenever either
// operand is NaN. The BVH bounds maths relies on plain ordered comparisons, so
// feeding NaN vertices into a build produces silently wrong AABBs rather than
// a detectable result.
inline float fminf( float a, float b ) { return a < b ? a : b; }
inline float fmaxf( float a, float b ) { return a > b ? a : b; }
inline float rsqrtf( float x ) { return 1.0f / sqrtf( x ); }
inline float sqrf( float x ) { return x * x; }

inline float2 make_float2( const float a, float b ) { float2 f2; f2.x = a, f2.y = b; return f2; }
inline float2 make_float2( const float s ) { return make_float2( s, s ); }
inline float3 make_float3( const float& a, const float& b, const float& c ) { float3 f3; f3.x = a; f3.y = b; f3.z = c; return f3; }
inline float3 make_float3( const float& s ) { return make_float3( s, s, s ); }
inline float3 make_float3( const float4& a ) { float3 f3; f3.x = a.x; f3.y = a.y; f3.z = a.z; return f3; }
inline float4 make_float4( const float a, const float b, const float c, const float d ) { float4 f4; f4.x = a, f4.y = b, f4.z = c, f4.w = d; return f4; }

inline float3 operator+( const float3& a, const float3& b ) { return make_float3( a.x + b.x, a.y + b.y, a.z + b.z ); }
inline void operator+=( float3& a, const float3& b ) { a.x += b.x;	a.y += b.y;	a.z += b.z; }
inline float3 operator+( const float3& a, float b ) { return make_float3( a.x + b, a.y + b, a.z + b ); }
inline float3 operator+( float b, const float3& a ) { return make_float3( a.x + b, a.y + b, a.z + b ); }

inline float3 operator-( const float3& a, const float3& b ) { return make_float3( a.x - b.x, a.y - b.y, a.z - b.z ); }
inline float3 operator-( const float3& a, float b ) { return make_float3( a.x - b, a.y - b, a.z - b ); }
inline float3 operator-( float b, const float3& a ) { return make_float3( b - a.x, b - a.y, b - a.z ); }

inline float3 operator*( const float3& a, const float3& b ) { return make_float3( a.x * b.x, a.y * b.y, a.z * b.z ); }
inline float3 operator*( const float3& a, float b ) { return make_float3( a.x * b, a.y * b, a.z * b ); }
inline float3 operator*( float b, const float3& a ) { return make_float3( b * a.x, b * a.y, b * a.z ); }

inline float3 operator/( const float3& a, const float3& b ) { return make_float3( a.x / b.x, a.y / b.y, a.z / b.z ); }
inline float3 operator/( const float3& a, float b ) { return make_float3( a.x / b, a.y / b, a.z / b ); }

inline float3 fminf( const float3& a, const float3& b ) { return make_float3( fminf( a.x, b.x ), fminf( a.y, b.y ), fminf( a.z, b.z ) ); }
inline float3 fmaxf( const float3& a, const float3& b ) { return make_float3( fmaxf( a.x, b.x ), fmaxf( a.y, b.y ), fmaxf( a.z, b.z ) ); }

inline float dot( const float3& a, const float3& b ) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float sqrLength( const float3& v ) { return dot( v, v ); }
inline float length( const float3& v ) { return sqrtf( dot( v, v ) ); }
inline float3 normalize( const float3& v ) { float invLen = rsqrtf( dot( v, v ) ); return v * invLen; }
inline float3 cross( const float3& a, const float3& b ) { return make_float3( a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x ); }

// header for SSE intrinsics
//
// Deliberately last: nothing above needs `__m128`, but every consumer of this
// header does (`bvh.h`'s unions, `tri.h`'s `Tri`). Because it is at the bottom,
// including precomp.h is sufficient to name `__m128` — do not add a second
// include of it, and do not move this line up without checking the macro
// blocks above still see what they need.
#include <immintrin.h>
