#pragma once

#include "precomp.h"

// enable the use of SSE in the AABB intersection function
#define USE_SSE

// bin count for binned BVH building
#define BINS 8

namespace Tmpl8
{

// minimalist triangle struct
ALIGN(64) struct Tri
{
	// union each float3 with a 16-byte __m128 for faster BVH construction
	union { float3 vertex0; __m128 v0; };
	union { float3 vertex1; __m128 v1; };
	union { float3 vertex2; __m128 v2; };
	union { float3 centroid; __m128 centroid4; }; // total size: 64 bytes
};

// additional triangle data, for texturing and shading
struct TriEx { float2 uv0, uv1, uv2; float3 N0, N1, N2; };

// minimalist AABB struct with grow functionality
struct aabb
{
	float3 bmin = 1e30f, bmax = -1e30f;
	void grow( float3 p ) { bmin = fminf( bmin, p ); bmax = fmaxf( bmax, p ); }
	void grow( aabb& b ) { if (b.bmin.x != 1e30f) { grow( b.bmin ); grow( b.bmax ); } }
	float area()
	{
		float3 e = bmax - bmin; // box extent
		return e.x * e.y + e.y * e.z + e.z * e.x;
	}
};

// intersection record, carefully tuned to be 16 bytes in size
struct Intersection
{
	float t;		// intersection distance along ray
	float u, v;		// barycentric coordinates of the intersection
	uint instPrim;	// instance index (12 bit) and primitive index (20 bit)
};

// ray struct, prepared for SIMD AABB intersection
ALIGN(64) struct Ray
{
	Ray() { O4 = D4 = rD4 = _mm_set1_ps( 1 ); }
	union { struct { float3 O; float dummy1; }; __m128 O4; };
	union { struct { float3 D; float dummy2; }; __m128 D4; };
	union { struct { float3 rD; float dummy3; }; __m128 rD4; };
	Intersection hit; // total ray size: 64 bytes
};

// 32-byte BVH node struct
struct BVHNode
{
	union { struct { float3 aabbMin; uint leftFirst; }; __m128 aabbMin4; };
	union { struct { float3 aabbMax; uint triCount; }; __m128 aabbMax4; };
	bool isLeaf() const { return triCount > 0; } // empty BVH leaves do not exist
	float CalculateNodeCost()
	{
		float3 e = aabbMax - aabbMin; // extent of the node
		return (e.x * e.y + e.y * e.z + e.z * e.x) * triCount;
	}
};

// bounding volume hierarchy, to be used as BLAS
ALIGN(64) class BVH
{
	struct BuildJob
	{
		uint nodeIdx;
		float3 centroidMin, centroidMax;
	};
public:
	BVH() = default;
	BVH( class Mesh* mesh );
	void Build();
	void Refit();
	void Intersect( Ray& ray, uint instanceIdx );
private:
	void Subdivide( uint nodeIdx, uint depth, uint& nodePtr, float3& centroidMin, float3& centroidMax );
	void UpdateNodeBounds( uint nodeIdx, float3& centroidMin, float3& centroidMax );
	float FindBestSplitPlane( BVHNode& node, int& axis, int& splitPos, float3& centroidMin, float3& centroidMax );
	class Mesh* mesh = 0;
public:
	uint* triIdx = 0;
	uint nodesUsed;
	BVHNode* bvhNode = 0;
	bool subdivToOnePrim = false; // for TLAS experiment
	BuildJob buildStack[64];
	int buildStackPtr;
};

// minimalist mesh class
class Mesh
{
public:
	Mesh() = default;
	Mesh( uint primCount );
	Mesh( const char* objFile, const char* texFile, const float scale = 1 );
	Tri* tri = 0;			// triangle data for intersection
	TriEx* triEx = 0;		// triangle data for shading
	int triCount = 0;
	BVH* bvh = 0;
	float3* P = 0, * N = 0;
};

// Simple matrix class for transforms
class mat4
{
public:
	mat4() = default;
	float cell[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	float& operator [] ( const int idx ) { return cell[idx]; }
	float operator()( const int i, const int j ) const { return cell[i * 4 + j]; }
	float& operator()( const int i, const int j ) { return cell[i * 4 + j]; }
	
	static mat4 Identity() { return mat4{}; }
	static mat4 Translate( const float3 P ) 
	{ 
		mat4 r; 
		r.cell[3] = P.x; r.cell[7] = P.y; r.cell[11] = P.z; 
		return r; 
	}
	static mat4 Scale( const float s ) 
	{ 
		mat4 r; 
		r.cell[0] = r.cell[5] = r.cell[10] = s; 
		return r; 
	}
	
	mat4 Inverted() const
	{
		// Simplified inversion for basic transforms
		mat4 ret;
		// For now, return identity - will implement proper inversion later
		return ret;
	}
	
	float3 TransformPoint( const float3& v ) const
	{
		return make_float3(
			cell[0] * v.x + cell[1] * v.y + cell[2] * v.z + cell[3],
			cell[4] * v.x + cell[5] * v.y + cell[6] * v.z + cell[7],
			cell[8] * v.x + cell[9] * v.y + cell[10] * v.z + cell[11] );
	}
	
	float3 TransformVector( const float3& v ) const
	{
		return make_float3( 
			cell[0] * v.x + cell[1] * v.y + cell[2] * v.z,
			cell[4] * v.x + cell[5] * v.y + cell[6] * v.z,
			cell[8] * v.x + cell[9] * v.y + cell[10] * v.z );
	}
};

// instance of a BVH, with transform and world bounds
class BVHInstance
{
public:
	BVHInstance() = default;
	BVHInstance( BVH* blas, uint index ) : bvh( blas ), idx( index ) { SetTransform( mat4() ); }
	void SetTransform( const mat4& transform );
	mat4& GetTransform() { return transform; }
	void Intersect( Ray& ray );
private:
	mat4 transform;
	mat4 invTransform; // inverse transform
public:
	aabb bounds; // in world space
private:
	BVH* bvh = 0;
	uint idx;
};

// top-level BVH node
struct TLASNode
{
	union 
	{ 
		struct { float dummy1[3]; uint leftRight; }; 
		struct { float dummy3[3]; unsigned short left, right; }; 
		float3 aabbMin; 
		__m128 aabbMin4; 
	};
	union 
	{ 
		struct { float dummy2[3]; uint BLAS; }; 
		float3 aabbMax; 
		__m128 aabbMax4; 
	};
	bool isLeaf() { return leftRight == 0; }
};

// top-level BVH class (simplified without kdtree for now)
ALIGN(64) class TLAS
{
public:
	TLAS() = default;
	TLAS( BVHInstance* bvhList, int N );
	void Build();
	void Intersect( Ray& ray );
private:
	int FindBestMatch( int N, int A );
public:
	TLASNode* tlasNode = 0;
	BVHInstance* blas = 0;
	uint nodesUsed, blasCount;
	uint* nodeIdx = 0;
};

} // namespace Tmpl8