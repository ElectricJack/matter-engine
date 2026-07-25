#pragma once

#include "tri.h"   // Tri, TriEx, mat4

// enable the use of SSE in the AABB intersection function
#define USE_SSE

// bin count for binned BVH building
#define BINS 8

// Forward declarations
class BvhMesh;

// minimalist AABB struct with grow functionality
struct aabb
{
	float3 bmin, bmax;
	aabb() { 
		bmin = make_float3(1e30f); 
		bmax = make_float3(-1e30f); 
	}
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
struct ALIGN(64) BVHRay
{
	BVHRay() { O4 = D4 = rD4 = _mm_set1_ps( 1 ); }
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
class ALIGN(64) BVH
{
	struct BuildJob
	{
		uint nodeIdx;
		float3 centroidMin, centroidMax;
	};
public:
	BVH() = default;
	BVH( BvhMesh* mesh );
	// Install a previously-built BVH (from disk) without rebuilding. nodes/triIdx
	// are copied; nodes_used is the live node count. mesh must outlive this BVH.
	BVH( BvhMesh* mesh, const BVHNode* nodes, uint nodes_used, const uint* tri_idx );
	void Build();
	void Refit();
	void Intersect( BVHRay& ray, uint instanceIdx );
private:
	void Subdivide( uint nodeIdx, uint depth, uint& nodePtr, float3& centroidMin, float3& centroidMax );
	void UpdateNodeBounds( uint nodeIdx, float3& centroidMin, float3& centroidMax );
	float FindBestSplitPlane( BVHNode& node, int& axis, int& splitPos, float3& centroidMin, float3& centroidMax );
	bool TryMedianSplit( uint nodeIdx, int axis, float3& centroidMin, float3& centroidMax, uint& leftCount );
	BvhMesh* mesh = 0;
public:
	uint* triIdx = 0;
	uint nodesUsed;
	BVHNode* bvhNode = 0;
	bool subdivToOnePrim = false; // for TLAS experiment
	BuildJob buildStack[64];
	int buildStackPtr;
};

// minimalist mesh class
class BvhMesh
{
public:
	BvhMesh() = default;
	BvhMesh( uint primCount );
	BvhMesh( const char* objFile, const char* texFile, const float scale = 1 );
	Tri* tri = 0;			// triangle data for intersection
	TriEx* triEx = 0;		// triangle data for shading
	int triCount = 0;
	BVH* bvh = 0;
	float3* P = 0, * N = 0;
};


// BVH instance, for TLAS
class BVHInstance
{
public:
	BVH* bvh = 0;
	mat4 transform, invTransform;
	uint idx;
	aabb bounds;
	void SetTransform( const mat4& transform );
	BVHInstance() = default;
	BVHInstance( BVH* bvh_ptr, uint instance_idx ) : bvh(bvh_ptr), idx(instance_idx) {}
	void Intersect( BVHRay& ray );
	
	// Accessor methods for compatibility
	const mat4& GetTransform() const { return transform; }
	mat4& GetTransform() { return transform; }
	const mat4& GetInvTransform() const { return invTransform; }
	mat4& GetInvTransform() { return invTransform; }
};

// Top Level Acceleration Structure
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
	bool isLeaf() const { return leftRight == 0; }
};

class ALIGN(64) TLAS
{
public:
	TLAS() = default;
	TLAS( BVHInstance* blas, int N );
	void Build();
	void Intersect( BVHRay& ray );
	
	// Public accessors for external classes
	uint GetBlasCount() const { return blasCount; }
	uint GetNodesUsed() const { return nodesUsed; }
	BVHInstance* GetBlas() const { return blas; }
	TLASNode* GetTlasNode() const { return tlasNode; }
	
private:
	void BuildRecursive( uint nodeIndex, uint first, uint count );
	int FindBestMatch( int N, int A );
	
public:
	// Made public for direct access by visualization and manager classes
	BVHInstance* blas = 0;
	uint blasCount = 0, nodesUsed = 0;
	TLASNode* tlasNode = 0;
	uint* nodeIdx = 0;
};
