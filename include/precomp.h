#pragma once

// Template, IGAD version 2 - adapted for MatterEngine2
// IGAD/NHTV/UU - Jacco Bikker - 2006-2021

// add your includes to this file instead of to individual .cpp files
// to enjoy the benefits of precompiled headers:
// - fast compilation
// - solve issues with the order of header files once (here)
// do not include headers in header files (ever).

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

// "leak" common namespaces to all compilation units. This is not standard
// C++ practice but a simplification for template projects.
using namespace std;

// aligned memory allocations
#ifdef _MSC_VER
#define ALIGN( x ) __declspec( align( x ) )
#define MALLOC64( x ) ( ( x ) == 0 ? 0 : _aligned_malloc( ( x ), 64 ) )
#define FREE64( x ) _aligned_free( x )
#else
#define ALIGN( x ) __attribute__( ( aligned( x ) ) )
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

// Math constants for Windows compatibility
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// basic types
typedef unsigned char uchar;
typedef unsigned int uint;
typedef unsigned short ushort;

// vector type placeholders, carefully matching OpenCL's layout and alignment
struct ALIGN( 8 ) int2
{
	int2() = default;
	int2( const int a, const int b ) : x( a ), y( b ) {}
	int2( const int a ) : x( a ), y( a ) {}
	union { struct { int x, y; }; int cell[2]; };
	int& operator [] ( const int n ) { return cell[n]; }
};

struct ALIGN( 8 ) uint2
{
	uint2() = default;
	uint2( const int a, const int b ) : x( a ), y( b ) {}
	uint2( const uint a ) : x( a ), y( a ) {}
	union { struct { uint x, y; }; uint cell[2]; };
	uint& operator [] ( const int n ) { return cell[n]; }
};

struct ALIGN( 8 ) float2
{
	float2() = default;
	float2( const float a, const float b ) : x( a ), y( b ) {}
	float2( const float a ) : x( a ), y( a ) {}
	union { struct { float x, y; }; float cell[2]; };
	float& operator [] ( const int n ) { return cell[n]; }
};

struct float3
{
	union { struct { float x, y, z; }; float cell[3]; };
	float& operator [] ( const int n ) { return cell[n]; }
};

struct ALIGN( 16 ) float4
{
	float4() = default;
	float4( const float a, const float b, const float c, const float d ) : x( a ), y( b ), z( c ), w( d ) {}
	float4( const float a ) : x( a ), y( a ), z( a ), w( a ) {}
	float4( const float3 & a, const float d ) : x( a.x ), y( a.y ), z( a.z ), w( d ) {}
	float4( const float3 & a ) : x( a.x ), y( a.y ), z( a.z ), w( 1.0f ) {}
	union { struct { float x, y, z, w; }; float cell[4]; };
	float& operator [] ( const int n ) { return cell[n]; }
};

// math functions
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
#include <immintrin.h>