#pragma once

#include "precomp.h"

// enable the use of SSE in the AABB intersection function
#define USE_SSE

// bin count for binned BVH building
#define BINS 8

// Forward declarations
class BvhMesh;

// minimalist triangle struct
struct ALIGN(64) Tri
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
	~BVH() { FREE64( bvhNode ); delete[] triIdx; }
	// Delete copy operations to prevent double-free
	BVH(const BVH&) = delete;
	BVH& operator=(const BVH&) = delete;
	void Build();
	void Refit();
	void Intersect( BVHRay& ray, uint instanceIdx );
private:
	void Subdivide( uint nodeIdx, uint depth, uint& nodePtr, float3& centroidMin, float3& centroidMax );
	void UpdateNodeBounds( uint nodeIdx, float3& centroidMin, float3& centroidMax );
	float FindBestSplitPlane( BVHNode& node, int& axis, int& splitPos, float3& centroidMin, float3& centroidMax );
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
	~BvhMesh() { FREE64( tri ); FREE64( triEx ); }
	// Delete copy operations to prevent double-free
	BvhMesh(const BvhMesh&) = delete;
	BvhMesh& operator=(const BvhMesh&) = delete;
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
		// General 4x4 matrix inversion using Gauss-Jordan elimination
		mat4 inv;
		float m[16], invOut[16];
		
		// Copy to working array
		for (int i = 0; i < 16; i++) m[i] = cell[i];
		
		// Initialize as identity
		for (int i = 0; i < 16; i++) invOut[i] = 0.0f;
		invOut[0] = invOut[5] = invOut[10] = invOut[15] = 1.0f;
		
		// Perform Gauss-Jordan elimination
		for (int i = 0; i < 4; i++) {
			// Find pivot
			int pivot = i;
			for (int j = i + 1; j < 4; j++) {
				if (fabs(m[j * 4 + i]) > fabs(m[pivot * 4 + i])) {
					pivot = j;
				}
			}
			
			// Swap rows if needed
			if (pivot != i) {
				for (int k = 0; k < 4; k++) {
					float tmp = m[i * 4 + k];
					m[i * 4 + k] = m[pivot * 4 + k];
					m[pivot * 4 + k] = tmp;
					
					tmp = invOut[i * 4 + k];
					invOut[i * 4 + k] = invOut[pivot * 4 + k];
					invOut[pivot * 4 + k] = tmp;
				}
			}
			
			// Check for singular matrix
			if (fabs(m[i * 4 + i]) < 1e-8f) {
				// Return identity for singular matrices
				return mat4::Identity();
			}
			
			// Scale pivot row
			float scale = 1.0f / m[i * 4 + i];
			for (int k = 0; k < 4; k++) {
				m[i * 4 + k] *= scale;
				invOut[i * 4 + k] *= scale;
			}
			
			// Eliminate column
			for (int j = 0; j < 4; j++) {
				if (j != i) {
					float factor = m[j * 4 + i];
					for (int k = 0; k < 4; k++) {
						m[j * 4 + k] -= factor * m[i * 4 + k];
						invOut[j * 4 + k] -= factor * invOut[i * 4 + k];
					}
				}
			}
		}
		
		// Copy result
		for (int i = 0; i < 16; i++) inv.cell[i] = invOut[i];
		return inv;
	}
	
	float3 TransformPoint( const float3& v ) const
	{
		return make_float3( 
			cell[0] * v.x + cell[1] * v.y + cell[2] * v.z + cell[3],
			cell[4] * v.x + cell[5] * v.y + cell[6] * v.z + cell[7],
			cell[8] * v.x + cell[9] * v.y + cell[10] * v.z + cell[11]
		);
	}
	
	float3 TransformVector( const float3& v ) const
	{
		return make_float3( 
			cell[0] * v.x + cell[1] * v.y + cell[2] * v.z,
			cell[4] * v.x + cell[5] * v.y + cell[6] * v.z,
			cell[8] * v.x + cell[9] * v.y + cell[10] * v.z
		);
	}
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
	~TLAS() { FREE64( tlasNode ); delete[] nodeIdx; }
	// Delete copy operations to prevent double-free
	TLAS(const TLAS&) = delete;
	TLAS& operator=(const TLAS&) = delete;
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
