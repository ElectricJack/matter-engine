// libs/SpatialQueryLib/src/bvh.cpp
//
// Implementation of `include/bvh.h`. That header owns the type, ownership,
// lifetime, threading and bit-packing documentation; this file documents the
// ALGORITHMS and their numeric policy. Read the header first.
//
// SpatialQueryLib is misnamed — it owns the engine's core geometry types, not
// just queries — and this file is the largest piece of that. Nothing here
// touches the GPU: the Vulkan ray-tracing path builds its own device
// structures, and these are the CPU mirrors used for baking, picking and
// offline traces.
//
// The BLAS build, end to end
// --------------------------
//   Build()              resets the node pool to nodesUsed = 2 (index 1 is
//                        skipped so sibling pairs stay cache-line aligned),
//                        fills triIdx with the identity permutation,
//                        recomputes every Tri::centroid IN THE SHARED MESH,
//                        bounds the root, then recurses.
//   UpdateNodeBounds()   computes a node's AABB and, as a side effect,
//                        OVERWRITES the caller's centroidMin/centroidMax with
//                        that node's centroid bounds. Those out-params are
//                        scratch threaded through the recursion, not inputs.
//   FindBestSplitPlane() 8 bins per axis (BINS), prefix/suffix sweep for
//                        per-side area and count, then a cost that is NOT
//                        textbook SAH -- see below.
//   Subdivide()          applies the split, or keeps the node as a leaf.
//   TryMedianSplit()     rescue path when the binned partition degenerates.
//
// Split policy, and why the trees look the way they do
// ----------------------------------------------------
// The cost function blends SAH with a QUADRATIC penalty on imbalance
// (BALANCE_WEIGHT 0.7 against IDEAL_BALANCE 0.5), then candidates are ordered
// by balance first and cost only as a tie-break within a 0.05 balance band.
// Subdivide additionally force-splits any node above 4 triangles and otherwise
// accepts a split costing up to 1.5x the un-split node. The result is
// deliberately flatter and more balanced than a pure-SAH build, at some cost in
// traversal quality for non-uniform geometry. Any of those five constants
// changes every tree in the engine, so they are effectively a tuning contract.
//
// Depth budget -- an invariant that keeps traversal memory-safe
// -------------------------------------------------------------
// `BVH::Intersect` walks a FIXED 64-entry stack with NO overflow check. What
// makes that safe is MAX_DEPTH = 40 in `Subdivide`: the tree can never be deep
// enough to overflow the stack. Raising MAX_DEPTH past 64 would silently
// corrupt traversal. (`TLAS::Intersect` does bounds-check its stack, and
// silently drops the far child instead -- the two traversals are not
// consistent about this.)
//
// Node-pool sizing: the pool is 2*triCount nodes plus padding, the build starts
// at 2 and adds 2 per split with at most triCount-1 splits, so it is exactly
// sized and `nodePtr` cannot run past it.
//
// Epsilons here are ABSOLUTE, not relative to geometry scale: 1e-5 for the
// ray/triangle parallel test, 1e-4 for the near-hit rejection, 1e30 as the
// universal "miss" sentinel and as the empty-AABB marker. Geometry authored at
// a very different scale from the rest of the engine will interact with them.
#include "../include/precomp.h"
#include "../include/bvh.h"
#include <cstring>
#include <cstdio>
#include <algorithm>  // std::nth_element for TLAS centroid-axis split

// functions

// Moeller-Trumbore, unculled: `fabs(a)` accepts back faces, so a ray hits a
// triangle from either side. Records into `ray.hit` only when the distance
// beats the incoming `ray.hit.t`, which is why the caller must seed it.
//
// The 1e-4 near-hit floor is what stops a secondary ray re-hitting the surface
// it started on; it is an absolute distance in the BVH's own units, so at very
// small geometry scales it can reject legitimate hits. `instPrim` is stored
// verbatim -- packing it is the caller's job (see `BVH::Intersect`).
void IntersectTri( BVHRay& ray, const Tri& tri, const uint instPrim )
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

// Slab test. Returns the entry distance `tmin`, or 1e30f for "no useful hit" --
// which covers three distinct cases the caller cannot tell apart and does not
// need to: a genuine miss, a box entirely behind the ray, and a box farther
// than the current best hit. Traversal treats all three as "do not descend".
//
// Reads `ray.rD` (the precomputed reciprocal direction) and never derives it;
// the fminf/fmaxf pairing is what makes infinities from a zero direction
// component resolve correctly instead of producing NaNs.
inline float IntersectAABB( const BVHRay& ray, const float3 bmin, const float3 bmax )
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
// SIMD slab test, and the one place the .w-lane overlay described in bvh.h is
// actively defended: `mask4` is all-ones in x/y/z and zero in w, so the `and`
// clears whatever packed integer (`leftFirst` / `triCount`) is sitting in the
// fourth float of the node's AABB corner before any arithmetic touches it.
// Without that mask those integers would be reinterpreted as floats and would
// poison the min/max reduction.
//
// The w lane is computed and then ignored -- only vmin/vmax [0..2] feed the
// result. Same return convention as the scalar version: 1e30f for miss,
// behind-the-ray, or beyond the current best `hit.t`.
float IntersectAABB_SSE( const BVHRay& ray, const __m128& bmin4, const __m128& bmax4 )
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

// Allocate and zero both parallel arrays for `primCount` triangles. `Tri` is
// exactly 64 bytes so its byte count is always a valid MALLOC64 size; `TriEx`
// is 96, which is why only that one needs the explicit round-up below. Any new
// TriEx allocation site has to repeat it.
//
// Note the memsets zero `primCount * sizeof(TriEx)` while the allocation may be
// larger after the round-up; the padding tail is left uninitialized and is
// never indexed.
BvhMesh::BvhMesh( const uint primCount )
{
	// basic constructor, for top-down TLAS construction
	tri = (Tri*)MALLOC64( primCount * sizeof( Tri ) );
	memset( tri, 0, primCount * sizeof( Tri ) );
	// Round up to a multiple of 64 so aligned_alloc (MALLOC64) gets a valid size.
	// sizeof(TriEx)==96 is not a power-of-two multiple of 64, so odd primCounts
	// would produce a misaligned size without this guard.
	size_t triex_bytes = ((primCount * sizeof( TriEx ) + 63) & ~size_t(63));
	triEx = (TriEx*)MALLOC64( triex_bytes );
	memset( triEx, 0, primCount * sizeof( TriEx ) );
	triCount = primCount;
}

// BVH class implementation

// Destructors for the raw buffers these classes allocate. Their absence meant
// every BvhMesh/BVH/TLAS ever built was leaked: nothing in the tree called
// FREE64 at all. BLASManager erases entries on release and destroys a whole
// scratch manager per part decode plus a staging manager per staged load, so a
// forever-streaming world leaked roughly a decoded sector twice per sector.
//
// Only genuinely-owned pointers are freed. BvhMesh::bvh and BVH::mesh are
// back-pointers whose lifetime belongs to BLASEntry's unique_ptrs; freeing them
// here would double-free. BvhMesh::P/N are never allocated by any constructor
// in this file and stay null.
BvhMesh::~BvhMesh()
{
	FREE64( tri );
	FREE64( triEx );
}

BVH::~BVH()
{
	FREE64( bvhNode );
	delete[] triIdx;
}

TLAS::~TLAS()
{
	FREE64( tlasNode );
	delete[] nodeIdx;
}

BVH::BVH( BvhMesh* triMesh, bool subdiv_to_one_prim )
{
	mesh = triMesh;
	bvhNode = (BVHNode*)MALLOC64( sizeof( BVHNode ) * mesh->triCount * 2 + 64 );
	triIdx = new uint[mesh->triCount];
	// Applied before the build so the caller that wants single-triangle leaves
	// gets one build rather than a default build followed by a full rebuild.
	subdivToOnePrim = subdiv_to_one_prim;
	Build();
}

uint BVH::TriangleCount() const
{
	return mesh ? mesh->triCount : 0;
}

// Install a previously-serialised tree: allocates the same pool shape as the
// building constructor (so every consumer can assume one layout) and memcpys
// the caller's nodes and permutation in. It does NOT call Build(), does not
// touch `mesh->tri[i].centroid`, and does not validate anything.
//
// Preconditions the caller must uphold: `nodes` holds at least `nodes_used`
// entries, `tri_idx` holds exactly `mesh->triCount` entries, and every index in
// both is in range for this mesh. A mismatched pair produces out-of-bounds
// reads during traversal, not a diagnostic.
BVH::BVH( BvhMesh* triMesh, const BVHNode* nodes, uint nodes_used, const uint* tri_idx )
{
	mesh = triMesh;
	// Same allocation shape as the building constructor so all consumers agree.
	bvhNode = (BVHNode*)MALLOC64( sizeof( BVHNode ) * mesh->triCount * 2 + 64 );
	triIdx = new uint[mesh->triCount];
	nodesUsed = nodes_used;
	memcpy( bvhNode, nodes, sizeof( BVHNode ) * nodes_used );
	memcpy( triIdx, tri_idx, sizeof( uint ) * mesh->triCount );
}

// Iterative front-to-back traversal: test both children, descend into the
// nearer, push the farther only if it was hit at all. Because the near child is
// visited first and `IntersectAABB` rejects anything beyond the current
// `hit.t`, a pushed node is often already dead by the time it is popped -- that
// early-out is where the ordering pays for itself.
//
// The stack is 64 entries with NO overflow guard; see the file header on why
// MAX_DEPTH = 40 is what keeps that safe.
//
// The leaf loop packs `(instanceIdx << 20) + triIdx[...]` into `instPrim` and
// then masks the low 20 bits back off to index the mesh. So `triIdx` values at
// or above 2^20 do not merely truncate -- they carry into the instance field
// and mis-attribute the hit (the ceilings are documented on `Intersection` in
// bvh.h).
void BVH::Intersect( BVHRay& ray, uint instanceIdx )
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

// Full rebuild from `mesh`. Note the second step: it writes `centroid` back
// into every `Tri` OF THE SHARED MESH, so this mutates state other objects can
// see. That is the reason a build must not overlap a trace or another build
// over the same mesh (bvh.h states the rule; this is where it comes from).
//
// `nodesUsed` restarts at 2 -- index 1 is intentionally skipped so that every
// sibling pair (2k, 2k+1) lands in one cache line. Anything reading the pool
// must therefore never assume node 1 is meaningful.
//
// `buildStackPtr` is zeroed here and never read again; the recursion is plain
// call recursion, and the BuildJob stack is vestigial (see bvh.h).
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

// Two jobs in one pass, and the second is easy to miss: it sets the node's AABB
// from its triangles' VERTICES, and it OVERWRITES the caller's
// centroidMin/centroidMax with the bounds of the same triangles' CENTROIDS.
// The split search bins against centroid bounds while the tree stores vertex
// bounds, so both are needed and they are not the same box.
//
// Must be called for a node before `Subdivide` looks at it -- `Build` does so
// for the root and `Subdivide` does so for each child it creates.
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

// Recursively split one node, or leave it as a leaf. Termination, in order:
// depth >= 40 (also the traversal-stack invariant, see the file header),
// triCount <= 1, no valid split plane, or -- unless the node is force-split for
// holding more than 4 triangles -- a best split costing >= 1.5x the un-split
// node. `subdivToOnePrim` replaces the cost test with "stop at one triangle".
//
// The partition deliberately RE-DERIVES each triangle's bin index with the
// same expression `FindBestSplitPlane` used, rather than comparing against a
// reconstructed plane position. Computing the plane and comparing coordinates
// would disagree with the binning in the last float ulp for triangles sitting
// exactly on a bin edge, and put them on the other side of the split from the
// one the cost was computed for.
//
// KNOWN DEFECT in the degenerate-partition rescue. When the binned partition
// leaves one side empty, `TryMedianSplit` re-partitions the range and reports a
// new `leftCount` -- but the local `i`, which is the partition boundary the
// right child's `leftFirst` is taken from, is NOT recomputed. It still holds
// the degenerate boundary (the start or the end of the parent's range), so the
// two children's triangle ranges end up overlapping or running past the parent.
// This path is believed unreachable today: `FindBestSplitPlane` only proposes
// candidates whose bin counts are non-empty on both sides, and the partition
// re-derives the identical bin index, so a degenerate result should not occur.
// It is recorded here because the rescue is silent -- if it ever does fire, the
// symptom is duplicated and dropped triangles, not a crash.
void BVH::Subdivide( uint nodeIdx, uint depth, uint& nodePtr, float3& centroidMin, float3& centroidMax )
{
	BVHNode& node = bvhNode[nodeIdx];
	
	// Improved termination criteria for balanced trees
	const uint MAX_DEPTH = 40;  // Allow deeper trees for better balance
	const uint MIN_TRIS_PER_LEAF = 1;  // Minimum triangles per leaf
	const uint MAX_TRIS_PER_LEAF = 4;  // Maximum triangles per leaf before forced split (reduced from 8)
	
	// Early termination conditions
	if (depth >= MAX_DEPTH || node.triCount <= MIN_TRIS_PER_LEAF) {
		return; // Too deep or too few triangles
	}
	
	// Force split for nodes with many triangles to maintain balance
	bool forceSplit = (node.triCount > MAX_TRIS_PER_LEAF);
	
	// determine split axis using balanced SAH
	int axis, splitPos;
	float splitCost = FindBestSplitPlane( node, axis, splitPos, centroidMin, centroidMax );
	if (splitCost == 1e30f) return; // no valid split (all centroids identical) - keep as leaf

	// terminate recursion based on improved criteria
	if (subdivToOnePrim)
	{
		if (node.triCount == 1) return;
	}
	else
	{
		float nosplitCost = node.CalculateNodeCost();
		// Be much more aggressive about splitting for balance - allow up to 50% cost increase
		if (!forceSplit && splitCost >= nosplitCost * 1.5f) {
			return;
		}
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
	uint leftCount = i - node.leftFirst;
	if (leftCount == 0 || leftCount == node.triCount) {
		// Fallback: try spatial median split for better balance
		if (TryMedianSplit(nodeIdx, axis, centroidMin, centroidMax, leftCount)) {
			// Median split succeeded, continue with subdivision
		} else {
			return; // Cannot split this node
		}
	}
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

// Search all three axes for the best of the 7 planes between 8 bins, returning
// the blended cost and writing the winning `axis`/`splitPos`; 1e30f means no
// valid split exists on any axis (all centroids coincident along every axis
// with extent) and the caller keeps the node as a leaf.
//
// Mechanics per axis: bin by centroid, accumulate per-bin AABB and count, then
// one prefix and one suffix sweep give left/right area and count for every
// plane in O(BINS) instead of O(BINS^2). Axes whose centroid bounds are
// degenerate are skipped entirely -- which is also what guarantees the caller's
// `scale = BINS / (max - min)` cannot divide by zero for the chosen axis.
//
// The cost is NOT plain SAH. `sahCost` is scaled by (1 + 4*(leftRatio - 0.5)^2)
// and mixed 30/70 with itself, i.e. an imbalanced split is penalised
// quadratically; candidates are then ordered by balance first, with cost only
// breaking ties inside a 0.05 balance band. See the file header on what that
// does to tree shape.
//
// Implementation notes: the candidate ordering is an O(k^2) selection-style
// pass over at most 7 entries -- cheap here, but it is a full sort where only
// the minimum is used. `bin`, `candidates` and the four sweep arrays are all
// fixed-size locals derived from BINS, so changing BINS changes this function's
// stack frame.
float BVH::FindBestSplitPlane( BVHNode& node, int& axis, int& splitPos, float3& centroidMin, float3& centroidMax )
{
	axis = 0; splitPos = 0; // defensible defaults; cost stays 1e30f when no valid split exists
	float bestCost = 1e30f;

	// Parameters for balanced tree construction
	const float BALANCE_WEIGHT = 0.7f;  // Weight balance vs pure SAH (increased from 0.3)
	const float IDEAL_BALANCE = 0.5f;   // Ideal left/right split ratio
	
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
		
		// Enhanced splitting: try multiple approaches and pick the most balanced
		struct SplitCandidate {
			int axis, splitPos;
			float cost, balance;
			bool valid;
		};
		
		SplitCandidate candidates[BINS - 1];
		int numCandidates = 0;
		
		// calculate balanced SAH cost for the 7 planes
		scale = (boundsMax - boundsMin) / BINS;
		for (int i = 0; i < BINS - 1; i++)
		{
			// Skip invalid splits
			if (leftCount[i] == 0 || rightCount[i] == 0) continue;
			
			// Standard SAH cost
			float sahCost = leftCount[i] * leftArea[i] + rightCount[i] * rightArea[i];
			
			// Balance metric - how close to 50/50 split
			float leftRatio = (float)leftCount[i] / (float)node.triCount;
			float balanceScore = fabsf(leftRatio - IDEAL_BALANCE);
			
			// Exponential penalty for unbalanced splits
			float balancePenalty = balanceScore * balanceScore * 4.0f; // Quadratic penalty
			
			// Combined cost: heavily favor balance
			float combinedCost = (1.0f - BALANCE_WEIGHT) * sahCost + BALANCE_WEIGHT * (sahCost * (1.0f + balancePenalty));
			
			// Store candidate
			candidates[numCandidates] = {a, i + 1, combinedCost, balanceScore, true};
			numCandidates++;
		}
		
		// Sort candidates by balance first, then by cost
		for (int i = 0; i < numCandidates - 1; i++) {
			for (int j = i + 1; j < numCandidates; j++) {
				bool shouldSwap = false;
				
				// Primary criterion: balance (lower is better)
				if (candidates[j].balance < candidates[i].balance - 0.05f) {
					shouldSwap = true;
				} else if (fabsf(candidates[j].balance - candidates[i].balance) <= 0.05f) {
					// Similar balance, prefer lower cost
					if (candidates[j].cost < candidates[i].cost) {
						shouldSwap = true;
					}
				}
				
				if (shouldSwap) {
					SplitCandidate temp = candidates[i];
					candidates[i] = candidates[j];
					candidates[j] = temp;
				}
			}
		}
		
		// Select the best candidate
		if (numCandidates > 0 && candidates[0].cost < bestCost) {
			axis = candidates[0].axis;
			splitPos = candidates[0].splitPos;
			bestCost = candidates[0].cost;
		}
	}
	return bestCost;
}

// Rescue path for a degenerate binned partition. Two attempts: partition about
// the SPATIAL median of the centroid bounds and accept if each side keeps at
// least 10% of the triangles; failing that, insertion-sort the whole range by
// centroid along `axis` and halve it by COUNT. Writes the resulting left-side
// size to `leftCount` and returns false only when even the count split is
// impossible.
//
// The insertion sort is O(n^2) in the node's triangle count, which is only
// tolerable because this fires on nodes the binner could not separate --
// typically small ones. On a large node it would be the dominant cost of the
// whole build.
//
// It reports a SIZE but not a partition INDEX, and the caller does not
// recompute one -- see the defect note on `Subdivide`.
bool BVH::TryMedianSplit( uint nodeIdx, int axis, float3& centroidMin, float3& centroidMax, uint& leftCount )
{
	BVHNode& node = bvhNode[nodeIdx];
	
	// Calculate spatial median along the specified axis
	float medianPos = (centroidMin.cell[axis] + centroidMax.cell[axis]) * 0.5f;
	
	// Partition triangles around the median
	int i = node.leftFirst;
	int j = i + node.triCount - 1;
	
	while (i <= j)
	{
		if (mesh->tri[triIdx[i]].centroid.cell[axis] < medianPos) {
			i++;
		} else {
			uint tmp = triIdx[i]; 
			triIdx[i] = triIdx[j]; 
			triIdx[j] = tmp;
			j--;
		}
	}
	
	// Check if we got a reasonable split
	leftCount = i - node.leftFirst;
	float leftRatio = (float)leftCount / (float)node.triCount;
	
	// Accept split if it's reasonably balanced (at least 10% on each side)
	if (leftRatio > 0.1f && leftRatio < 0.9f) {
		return true;
	}
	
	// If still too one-sided, try splitting exactly in half by count
	// Sort triangles by centroid position along the axis
	uint first = node.leftFirst;
	uint count = node.triCount;
	
	// Simple insertion sort for small arrays (good enough for typical leaf sizes)
	for (uint i = first + 1; i < first + count; i++) {
		uint key = triIdx[i];
		float keyPos = mesh->tri[key].centroid.cell[axis];
		int j = i - 1;
		
		while (j >= (int)first && mesh->tri[triIdx[j]].centroid.cell[axis] > keyPos) {
			triIdx[j + 1] = triIdx[j];
			j--;
		}
		triIdx[j + 1] = key;
	}
	
	// Split exactly in half by count
	leftCount = count / 2;
	if (leftCount > 0 && leftCount < count) {
		return true;
	}
	
	return false; // Cannot create a reasonable split
}

// BVHInstance implementation

// World bounds are computed by transforming all EIGHT corners of the BLAS root
// AABB and taking their extent. That is correct but conservative: under
// rotation the result is the AABB of the rotated box, which is larger than the
// AABB of the geometry inside it, so a rotated instance costs extra TLAS
// traversal it does not strictly need.
//
// Reads the root node directly, so it requires an already-BUILT `bvh`; with a
// null or unbuilt one it collapses `bounds` to a degenerate box at the origin
// rather than leaving the empty sentinel, which is why such an instance still
// participates in TLAS bounds (as a point at the origin) instead of being
// skipped.
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

// Trace one world-space ray against this instance by pulling the ray into the
// BLAS's local space: origin as a point, direction as a VECTOR (not
// renormalised), and 1/D recomputed for the local direction. Because the
// direction is transformed rather than normalised, the ray parameter `t` means
// the same thing in both spaces, which is what lets `hit.t` be carried in and
// out unchanged.
//
// Only `hit.t` is seeded on the local ray; the rest of its `hit` is garbage
// until something writes it, and the copy-back is gated on a strictly closer
// hit, so garbage can never reach the caller. `idx` is passed down and lands in
// the top 12 bits of `hit.instPrim`.
//
// The TODO below is real: barycentrics and the primitive id come back
// unchanged (correct -- they are space-independent), but nothing here
// transforms a position or normal back to world space. Callers that need a
// world-space hit point derive it from `ray.O + ray.D * hit.t` themselves.
void BVHInstance::Intersect( BVHRay& ray )
{
	// Transform ray to local space
	BVHRay localRay;
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
	// Always allocate room for at least one node and one index. Build() writes
	// tlasNode[0] even in the zero-instance case (the empty-box sentinel), and
	// MALLOC64(0) returns null -- so sizing straight off N made TLAS(list, 0)
	// dereference null instead of producing a valid empty tree.
	const int nodeCapacity = N > 0 ? N * 2 : 1;
	const int idxCapacity  = N > 0 ? N     : 1;
	tlasNode = (TLASNode*)MALLOC64( sizeof( TLASNode ) * nodeCapacity );
	nodeIdx = new uint[idxCapacity];
	Build();
}

// Build the top-level tree over the instances `blas` points at. Two degenerate
// cases short-circuit before the recursion, both leaving `nodesUsed == 1`: zero
// instances writes a zero-sized box at the origin, and one instance writes a
// single leaf. The constructor floors its allocation at one node so the
// zero-instance write lands in real storage.
//
// Otherwise `nodesUsed` starts at 1 and `BuildRecursive` bumps it by 2 per
// interior node, giving 2*blasCount-1 nodes for a full binary tree -- inside
// the 2N the constructor allocated.
//
// Preconditions: every instance must already have had `SetTransform` called, or
// its `bounds` is the empty sentinel and it is skipped when accumulating node
// AABBs (it still occupies a leaf, just an unbounded-looking one).
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

// Top-down median build over `nodeIdx[first .. first+count)`, which it permutes
// in place. Per node: accumulate the AABB over the instances in range (skipping
// the empty-bounds sentinel), leaf out at count == 1, otherwise pick the
// longest axis of that AABB, `nth_element` the range about its middle by
// instance centroid, and recurse into two halves. O(N log N) overall.
//
// Splitting by count rather than by SAH means the tree is always perfectly
// balanced in instance count and never adapts to clustering -- fine for the
// roughly uniform sector/part placements this is used for, weaker for a scene
// with one dense island.
//
// `node` is a reference into `tlasNode`, held across the recursive calls. That
// is safe only because the pool is preallocated and never reallocates.
//
// The 16-bit guard is a real ceiling, not a formality: `leftRight` packs two
// 16-bit child indices, so a TLAS needing more than 65535 nodes cannot address
// its own children. Rather than wrap silently it logs to stderr and degrades
// the node to a leaf holding ONE instance -- the other `count - 1` are dropped
// from the tree and will never be hit by a trace.
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

	// Perf: sort along the longest centroid axis before splitting at the median.
	// The original count/2 split in insertion order produces degenerate trees when
	// instances happen to be ordered along one axis. Sorting by centroid on the
	// widest dimension gives near-SAH quality for uniform distributions at O(N log N).
	float3 extent = node.aabbMax - node.aabbMin;
	int axis = 0;
	if (extent.y > extent.x) axis = 1;
	if (axis == 0 && extent.z > extent.x) axis = 2;
	else if (axis == 1 && extent.z > extent.y) axis = 2;

	// Helper: extract centroid along the chosen axis (float3 has no [] operator).
	auto centroid_on_axis = [this, axis](uint idx) -> float {
		const float3& bmin = blas[idx].bounds.bmin;
		const float3& bmax = blas[idx].bounds.bmax;
		if (axis == 1) return (bmin.y + bmax.y) * 0.5f;
		if (axis == 2) return (bmin.z + bmax.z) * 0.5f;
		return (bmin.x + bmax.x) * 0.5f;
	};

	// Partial sort: use std::nth_element on nodeIdx[first..first+count) to place
	// the median at split, with smaller centroids to the left and larger to the right.
	uint split = count / 2;
	uint* base = nodeIdx + first;
	std::nth_element(base, base + split, base + count,
		[&centroid_on_axis](uint a, uint b) {
			return centroid_on_axis(a) < centroid_on_axis(b);
		});

	uint leftFirst = first;
	uint leftCount = split;
	uint rightFirst = first + split;
	uint rightCount = count - split;

	// T4/16-bit packing guard (NDEBUG-safe): leftRight packs two 16-bit child
	// indices; a child index beyond 65535 would wrap at pack time and corrupt
	// traversal. Fail loudly and degrade this node to a leaf instead.
	if (nodesUsed + 1 > 0xFFFFu) {
		fprintf(stderr, "[ERROR] TLAS::BuildRecursive: node count exceeds 16-bit child index field "
		        "(nodesUsed=%u); node %u degraded to leaf, %u instance(s) dropped.\n",
		        nodesUsed, nodeIndex, count - 1);
		node.leftRight = 0; // leaf sentinel
		node.BLAS = nodeIdx[first];
		return;
	}

	// Create child nodes
	uint leftChild = nodesUsed++;
	uint rightChild = nodesUsed++;

	// Set interior node data
	node.leftRight = leftChild | (rightChild << 16);
	node.BLAS = 0; // Not used for interior nodes

	// Recursively build children
	BuildRecursive(leftChild, leftFirst, leftCount);
	BuildRecursive(rightChild, rightFirst, rightCount);
}

// Iterative front-to-back traversal, same near-child-first ordering as
// `BVH::Intersect`, recursing into `BVHInstance::Intersect` at each leaf (which
// transforms the ray into that BLAS's local space).
//
// Unlike `BVH::Intersect` this one DOES bounds-check its 64-entry stack -- and
// on overflow it silently drops the far child, which loses hits rather than
// corrupting memory. The median build keeps depth at ~log2(instances), so 64 is
// far more room than it needs; the two traversals are simply inconsistent about
// how they handle the same fixed stack.
//
// The root AABB is tested at the top of the loop like any other node, so a ray
// missing the whole scene exits after one slab test.
void TLAS::Intersect( BVHRay& ray )
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
				if (distRight != 1e30f && stackPtr < 64)
				{
					stack[stackPtr++] = rightNode;
				}
			}
		}
	}
} 