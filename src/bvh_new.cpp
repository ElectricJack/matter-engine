#include "../include/precomp.h"
#include "../include/bvh_new.h"
#include <cstring>

namespace Tmpl8
{

// functions

void IntersectTri( Ray& ray, const Tri& tri, const uint instPrim )
{
	// Moeller-Trumbore ray/triangle intersection algorithm
	const float3 edge1 = tri.vertex1 - tri.vertex0;
	const float3 edge2 = tri.vertex2 - tri.vertex0;
	const float3 h = cross( ray.D, edge2 );
	const float a = dot( edge1, h );
	if (fabs( a ) < 0.00001f) return; // ray parallel to triangle
	const float f = 1 / a;
	const float3 s = ray.O - tri.vertex0;
	const float u = f * dot( s, h );
	if (u < 0 || u > 1) return;
	const float3 q = cross( s, edge1 );
	const float v = f * dot( ray.D, q );
	if (v < 0 || u + v > 1) return;
	const float t = f * dot( edge2, q );
	if (t > 0.0001f && t < ray.hit.t)
		ray.hit.t = t, ray.hit.u = u,
		ray.hit.v = v, ray.hit.instPrim = instPrim;
}

inline float IntersectAABB( const Ray& ray, const float3 bmin, const float3 bmax )
{
	// "slab test" ray/AABB intersection
	float tx1 = (bmin.x - ray.O.x) * ray.rD.x, tx2 = (bmax.x - ray.O.x) * ray.rD.x;
	float tmin = fminf( tx1, tx2 ), tmax = fmaxf( tx1, tx2 );
	float ty1 = (bmin.y - ray.O.y) * ray.rD.y, ty2 = (bmax.y - ray.O.y) * ray.rD.y;
	tmin = fmaxf( tmin, fminf( ty1, ty2 ) ), tmax = fminf( tmax, fmaxf( ty1, ty2 ) );
	float tz1 = (bmin.z - ray.O.z) * ray.rD.z, tz2 = (bmax.z - ray.O.z) * ray.rD.z;
	tmin = fmaxf( tmin, fminf( tz1, tz2 ) ), tmax = fminf( tmax, fmaxf( tz1, tz2 ) );
	if (tmax >= tmin && tmin < ray.hit.t && tmax > 0) return tmin; else return 1e30f;
}

#ifdef USE_SSE
float IntersectAABB_SSE( const Ray& ray, const __m128& bmin4, const __m128& bmax4 )
{
	// "slab test" ray/AABB intersection, using SIMD instructions
	static __m128 mask4 = _mm_cmpeq_ps( _mm_setzero_ps(), _mm_set_ps( 1, 0, 0, 0 ) );
	__m128 t1 = _mm_mul_ps( _mm_sub_ps( _mm_and_ps( bmin4, mask4 ), ray.O4 ), ray.rD4 );
	__m128 t2 = _mm_mul_ps( _mm_sub_ps( _mm_and_ps( bmax4, mask4 ), ray.O4 ), ray.rD4 );
	__m128 vmax4 = _mm_max_ps( t1, t2 ), vmin4 = _mm_min_ps( t1, t2 );
	
	// Extract components
	float vmax[4], vmin[4];
	_mm_store_ps(vmax, vmax4);
	_mm_store_ps(vmin, vmin4);
	
	float tmax = fminf( vmax[0], fminf( vmax[1], vmax[2] ) );
	float tmin = fmaxf( vmin[0], fmaxf( vmin[1], vmin[2] ) );
	if (tmax >= tmin && tmin < ray.hit.t && tmax > 0) return tmin; else return 1e30f;
}
#endif

// BvhMesh class implementation

BvhMesh::BvhMesh( const uint primCount )
{
	// basic constructor, for top-down TLAS construction
	tri = (Tri*)MALLOC64( primCount * sizeof( Tri ) );
	memset( tri, 0, primCount * sizeof( Tri ) );
	triEx = (TriEx*)MALLOC64( primCount * sizeof( TriEx ) );
	memset( triEx, 0, primCount * sizeof( TriEx ) );
	triCount = primCount;
}

// BVH class implementation

BVH::BVH( BvhMesh* triMesh )
{
	mesh = triMesh;
	bvhNode = (BVHNode*)MALLOC64( sizeof( BVHNode ) * mesh->triCount * 2 + 64 );
	triIdx = new uint[mesh->triCount];
	Build();
}

void BVH::Intersect( Ray& ray, uint instanceIdx )
{
	BVHNode* node = &bvhNode[0], * stack[64];
	uint stackPtr = 0;
	while (1)
	{
		if (node->isLeaf())
		{
			for (uint i = 0; i < node->triCount; i++)
			{
				uint instPrim = (instanceIdx << 20) + triIdx[node->leftFirst + i];
				IntersectTri( ray, mesh->tri[instPrim & 0xfffff /* 20 bits */], instPrim );
			}
			if (stackPtr == 0) break; else node = stack[--stackPtr];
			continue;
		}
		BVHNode* child1 = &bvhNode[node->leftFirst];
		BVHNode* child2 = &bvhNode[node->leftFirst + 1];
#ifdef USE_SSE
		float dist1 = IntersectAABB_SSE( ray, child1->aabbMin4, child1->aabbMax4 );
		float dist2 = IntersectAABB_SSE( ray, child2->aabbMin4, child2->aabbMax4 );
#else
		float dist1 = IntersectAABB( ray, child1->aabbMin, child1->aabbMax );
		float dist2 = IntersectAABB( ray, child2->aabbMin, child2->aabbMax );
#endif
		if (dist1 > dist2) { 
			float tmpf = dist1; dist1 = dist2; dist2 = tmpf;
			BVHNode* tmpn = child1; child1 = child2; child2 = tmpn;
		}
		if (dist1 == 1e30f)
		{
			if (stackPtr == 0) break; else node = stack[--stackPtr];
		}
		else
		{
			node = child1;
			if (dist2 != 1e30f) stack[stackPtr++] = child2;
		}
	}
}

void BVH::Build()
{
	// reset node pool
	nodesUsed = 2;
	memset( bvhNode, 0, mesh->triCount * 2 * sizeof( BVHNode ) );
	// populate triangle index array
	for (int i = 0; i < mesh->triCount; i++) triIdx[i] = i;
	// calculate triangle centroids for partitioning
	Tri* tri = mesh->tri;
	for (int i = 0; i < mesh->triCount; i++)
		mesh->tri[i].centroid = (tri[i].vertex0 + tri[i].vertex1 + tri[i].vertex2) * (1.0f/3.0f);
	// assign all triangles to root node
	BVHNode& root = bvhNode[0];
	root.leftFirst = 0, root.triCount = mesh->triCount;
	float3 centroidMin, centroidMax;
	UpdateNodeBounds( 0, centroidMin, centroidMax );
	// subdivide recursively
	buildStackPtr = 0;
	Subdivide( 0, 0, nodesUsed, centroidMin, centroidMax );
}

void BVH::UpdateNodeBounds( uint nodeIdx, float3& centroidMin, float3& centroidMax )
{
	BVHNode& node = bvhNode[nodeIdx];
	node.aabbMin = make_float3( 1e30f );
	node.aabbMax = make_float3( -1e30f );
	centroidMin = make_float3( 1e30f );
	centroidMax = make_float3( -1e30f );
	for (uint first = node.leftFirst, i = 0; i < node.triCount; i++)
	{
		uint leafTriIdx = triIdx[first + i];
		Tri& leafTri = mesh->tri[leafTriIdx];
		node.aabbMin = fminf( node.aabbMin, leafTri.vertex0 );
		node.aabbMin = fminf( node.aabbMin, leafTri.vertex1 );
		node.aabbMin = fminf( node.aabbMin, leafTri.vertex2 );
		node.aabbMax = fmaxf( node.aabbMax, leafTri.vertex0 );
		node.aabbMax = fmaxf( node.aabbMax, leafTri.vertex1 );
		node.aabbMax = fmaxf( node.aabbMax, leafTri.vertex2 );
		centroidMin = fminf( centroidMin, leafTri.centroid );
		centroidMax = fmaxf( centroidMax, leafTri.centroid );
	}
}

void BVH::Subdivide( uint nodeIdx, uint depth, uint& nodePtr, float3& centroidMin, float3& centroidMax )
{
	BVHNode& node = bvhNode[nodeIdx];
	// determine split axis using SAH
	int axis, splitPos;
	float splitCost = FindBestSplitPlane( node, axis, splitPos, centroidMin, centroidMax );
	// terminate recursion
	if (subdivToOnePrim)
	{
		if (node.triCount == 1) return;
	}
	else
	{
		float nosplitCost = node.CalculateNodeCost();
		if (splitCost >= nosplitCost) return;
	}
	
	// in-place partition
	int i = node.leftFirst;
	int j = i + node.triCount - 1;
	float scale = BINS / (centroidMax.cell[axis] - centroidMin.cell[axis]);
	while (i <= j)
	{
		// use the exact calculation we used for binning to prevent rare inaccuracies
		int binIdx = std::min( BINS - 1, (int)((mesh->tri[triIdx[i]].centroid.cell[axis] - centroidMin.cell[axis]) * scale) );
		if (binIdx < splitPos) i++; 
		else { 
			uint tmp = triIdx[i]; triIdx[i] = triIdx[j]; triIdx[j] = tmp; 
			j--; 
		}
	}
	// abort split if one of the sides is empty
	int leftCount = i - node.leftFirst;
	if (leftCount == 0 || leftCount == node.triCount) return;
	// create child nodes
	int leftChildIdx = nodePtr++;
	int rightChildIdx = nodePtr++;
	bvhNode[leftChildIdx].leftFirst = node.leftFirst;
	bvhNode[leftChildIdx].triCount = leftCount;
	bvhNode[rightChildIdx].leftFirst = i;
	bvhNode[rightChildIdx].triCount = node.triCount - leftCount;
	node.leftFirst = leftChildIdx;
	node.triCount = 0;
	// recurse
	UpdateNodeBounds( leftChildIdx, centroidMin, centroidMax );
	Subdivide( leftChildIdx, depth + 1, nodePtr, centroidMin, centroidMax );
	UpdateNodeBounds( rightChildIdx, centroidMin, centroidMax );
	Subdivide( rightChildIdx, depth + 1, nodePtr, centroidMin, centroidMax );
}

float BVH::FindBestSplitPlane( BVHNode& node, int& axis, int& splitPos, float3& centroidMin, float3& centroidMax )
{
	float bestCost = 1e30f;
	for (int a = 0; a < 3; a++)
	{
		float boundsMin = centroidMin.cell[a], boundsMax = centroidMax.cell[a];
		if (boundsMin == boundsMax) continue;
		
		// Simplified binning without SSE for now
		struct Bin { aabb bounds; int triCount = 0; } bin[BINS];
		float scale = BINS / (boundsMax - boundsMin);
		for (uint i = 0; i < node.triCount; i++)
		{
			Tri& triangle = mesh->tri[triIdx[node.leftFirst + i]];
			int binIdx = std::min( BINS - 1, (int)((triangle.centroid.cell[a] - boundsMin) * scale) );
			bin[binIdx].triCount++;
			bin[binIdx].bounds.grow( triangle.vertex0 );
			bin[binIdx].bounds.grow( triangle.vertex1 );
			bin[binIdx].bounds.grow( triangle.vertex2 );
		}
		// gather data for the 7 planes between the 8 bins
		float leftArea[BINS - 1], rightArea[BINS - 1];
		int leftCount[BINS - 1], rightCount[BINS - 1];
		aabb leftBox, rightBox;
		int leftSum = 0, rightSum = 0;
		for (int i = 0; i < BINS - 1; i++)
		{
			leftSum += bin[i].triCount;
			leftCount[i] = leftSum;
			leftBox.grow( bin[i].bounds );
			leftArea[i] = leftBox.area();
			rightSum += bin[BINS - 1 - i].triCount;
			rightCount[BINS - 2 - i] = rightSum;
			rightBox.grow( bin[BINS - 1 - i].bounds );
			rightArea[BINS - 2 - i] = rightBox.area();
		}
		// calculate SAH cost for the 7 planes
		scale = (boundsMax - boundsMin) / BINS;
		for (int i = 0; i < BINS - 1; i++)
		{
			float planeCost = leftCount[i] * leftArea[i] + rightCount[i] * rightArea[i];
			if (planeCost < bestCost)
				axis = a, splitPos = i + 1, bestCost = planeCost;
		}
	}
	return bestCost;
}

// BVHInstance implementation

void BVHInstance::SetTransform( const mat4& transform_new )
{
	transform = transform_new;
	invTransform = transform.Inverted();
	
	// Calculate world bounds by transforming local BVH bounds
	if (bvh && bvh->nodesUsed > 0) {
		// Get local AABB from BVH root node
		float3 localMin = bvh->bvhNode[0].aabbMin;
		float3 localMax = bvh->bvhNode[0].aabbMax;
		
		// Transform all 8 corners of the AABB
		float3 corners[8] = {
			{localMin.x, localMin.y, localMin.z},
			{localMax.x, localMin.y, localMin.z},
			{localMin.x, localMax.y, localMin.z},
			{localMax.x, localMax.y, localMin.z},
			{localMin.x, localMin.y, localMax.z},
			{localMax.x, localMin.y, localMax.z},
			{localMin.x, localMax.y, localMax.z},
			{localMax.x, localMax.y, localMax.z}
		};
		
		// Initialize world bounds
		bounds.bmin = make_float3(1e30f);
		bounds.bmax = make_float3(-1e30f);
		
		// Transform each corner and expand bounds
		for (int i = 0; i < 8; i++) {
			float3 worldCorner = transform.TransformPoint(corners[i]);
			bounds.bmin = fminf(bounds.bmin, worldCorner);
			bounds.bmax = fmaxf(bounds.bmax, worldCorner);
		}
	} else {
		// Fallback for empty BVH
		bounds.bmin = make_float3(0.0f);
		bounds.bmax = make_float3(0.0f);
	}
}

void BVHInstance::Intersect( Ray& ray )
{
	// Transform ray to local space
	Ray localRay;
	localRay.O = invTransform.TransformPoint( ray.O );
	localRay.D = invTransform.TransformVector( ray.D );
	localRay.rD = make_float3( 1.0f / localRay.D.x, 1.0f / localRay.D.y, 1.0f / localRay.D.z );
	localRay.hit.t = ray.hit.t;
	
	// Intersect with BVH
	if (bvh) bvh->Intersect( localRay, idx );
	
	// Transform result back
	if (localRay.hit.t < ray.hit.t)
	{
		ray.hit = localRay.hit;
		// TODO: transform intersection point and normal back to world space
	}
}

// TLAS implementation

TLAS::TLAS( BVHInstance* bvhList, int N )
{
	blas = bvhList;
	blasCount = N;
	tlasNode = (TLASNode*)MALLOC64( sizeof( TLASNode ) * N * 2 );
	nodeIdx = new uint[N];
	Build();
}

void TLAS::Build()
{
	// Initialize node indices
	for (uint i = 0; i < blasCount; i++) nodeIdx[i] = i;
	nodesUsed = 1;
	
	if (blasCount == 0) {
		// No instances
		tlasNode[0].aabbMin = make_float3(0.0f);
		tlasNode[0].aabbMax = make_float3(0.0f);
		tlasNode[0].leftRight = 0;
		tlasNode[0].BLAS = 0;
		return;
	}
	
	if (blasCount == 1) {
		// Single instance - create leaf node
		tlasNode[0].aabbMin = blas[0].bounds.bmin;
		tlasNode[0].aabbMax = blas[0].bounds.bmax;
		tlasNode[0].leftRight = 0; // leaf
		tlasNode[0].BLAS = 0; // first instance
		return;
	}
	
	// Multiple instances - build proper binary tree
	BuildRecursive(0, 0, blasCount);
}

void TLAS::BuildRecursive(uint nodeIndex, uint first, uint count)
{
	TLASNode& node = tlasNode[nodeIndex];
	
	// Calculate AABB for this node
	node.aabbMin = make_float3(1e30f);
	node.aabbMax = make_float3(-1e30f);
	
	for (uint i = first; i < first + count; i++) {
		uint blasIdx = nodeIdx[i];
		if (blas[blasIdx].bounds.bmin.x < 1e29f) { // Valid bounds check
			node.aabbMin = fminf(node.aabbMin, blas[blasIdx].bounds.bmin);
			node.aabbMax = fmaxf(node.aabbMax, blas[blasIdx].bounds.bmax);
		}
	}
	
	// If we have only one instance, make this a leaf
	if (count == 1) {
		node.leftRight = 0; // leaf
		node.BLAS = nodeIdx[first]; // instance index
		return;
	}
	
	// Split instances into two groups
	uint split = count / 2;
	uint leftFirst = first;
	uint leftCount = split;
	uint rightFirst = first + split;
	uint rightCount = count - split;
	
	// Create child nodes
	uint leftChild = nodesUsed++;
	uint rightChild = nodesUsed++;
	
	// Set interior node data
	node.leftRight = (leftChild & 0xFFFF) | ((rightChild & 0xFFFF) << 16);
	node.BLAS = 0; // Not used for interior nodes
	
	// Recursively build children
	BuildRecursive(leftChild, leftFirst, leftCount);
	BuildRecursive(rightChild, rightFirst, rightCount);
}

void TLAS::Intersect( Ray& ray )
{
	// Stack-based iterative traversal
	TLASNode* stack[64];
	uint stackPtr = 0;
	TLASNode* node = &tlasNode[0];
	
	while (true)
	{
		// Test ray against node AABB
		if (IntersectAABB(ray, node->aabbMin, node->aabbMax) == 1e30f)
		{
			// Miss - pop from stack
			if (stackPtr == 0) break;
			node = stack[--stackPtr];
			continue;
		}
		
		if (node->leftRight == 0) // Leaf node
		{
			// Intersect with instance
			uint instanceIndex = node->BLAS;
			if (instanceIndex < blasCount)
			{
				blas[instanceIndex].Intersect(ray);
			}
			
			// Pop from stack
			if (stackPtr == 0) break;
			node = stack[--stackPtr];
		}
		else // Interior node
		{
			// Extract left and right child indices
			uint leftChild = node->leftRight & 0xFFFF;
			uint rightChild = (node->leftRight >> 16) & 0xFFFF;
			
			// Test both children and order by distance
			TLASNode* leftNode = &tlasNode[leftChild];
			TLASNode* rightNode = &tlasNode[rightChild];
			
			float distLeft = IntersectAABB(ray, leftNode->aabbMin, leftNode->aabbMax);
			float distRight = IntersectAABB(ray, rightNode->aabbMin, rightNode->aabbMax);
			
			// Sort by distance - process closer child first
			if (distLeft > distRight)
			{
				float tmpDist = distLeft; distLeft = distRight; distRight = tmpDist;
				TLASNode* tmpNode = leftNode; leftNode = rightNode; rightNode = tmpNode;
			}
			
			if (distLeft == 1e30f)
			{
				// Both children missed
				if (stackPtr == 0) break;
				node = stack[--stackPtr];
			}
			else
			{
				// Process closer child first
				node = leftNode;
				// Push farther child to stack if it hit
				if (distRight != 1e30f && stackPtr < 63)
				{
					stack[stackPtr++] = rightNode;
				}
			}
		}
	}
}

} // namespace Tmpl8