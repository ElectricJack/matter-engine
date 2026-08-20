#pragma once

// libs/SpatialQueryLib/include/bvh.h
//
// CPU bounding-volume-hierarchy types: the BLAS (`BVH` over a `BvhMesh` of
// `Tri`), the instance wrapper (`BVHInstance`) and the two-level TLAS
// (`TLAS`/`TLASNode`), plus the ray and hit records they traverse with.
// Originally derived from Jacco Bikker's IGAD BVH template (see the
// attribution in `precomp.h`) and grown in place.
//
// Where it fits
// -------------
// SpatialQueryLib is misnamed: it owns the engine's core geometry types, not
// just queries. This header sits directly above `tri.h` (Tri/TriEx/mat4) and
// `precomp.h` (float3/float4/ALIGN/MALLOC64). Above it:
//   - `libs/MatterSurfaceLib/src/blas_manager.cpp` builds and owns one
//     `BvhMesh` + `BVH` pair per registered triangle batch, both held through
//     `unique_ptr` inside a `BLASEntry`.
//   - `libs/MatterSurfaceLib/src/tlas_manager.cpp` copies `BVHInstance`s into
//     contiguous storage and constructs a `TLAS` over it.
//   - `MatterEngine3/src/world_tracer.cpp` traces `BVHRay`s straight against a
//     part's per-slice `BVH` (see the call sequence below).
//   - `bvh_analyzer.h` walks these structures read-only for quality metrics.
// Nothing here touches the GPU. The Vulkan ray-tracing path builds its own
// device acceleration structures; these are the CPU mirrors used for baking,
// picking and offline traces.
//
// Typical use
// -----------
//   BvhMesh mesh(triCount);        // allocates tri/triEx via MALLOC64
//   ... fill mesh.tri / mesh.triEx ...
//   BVH bvh(&mesh);                // ctor allocates AND calls Build()
//   BVHRay ray;                    // ctor does NOT initialise ray.hit
//   ray.O = ...; ray.D = ...; ray.rD = 1/D;   // caller fills all three
//   ray.hit.t = current_best_t;    // miss sentinel — must be set
//   bvh.Intersect(ray, 0);
// `world_tracer.cpp` is the reference call site for that sequence.
//
// Conventions and gotchas
// -----------------------
// - Units/space: whatever the producer used. `blas_manager` registers
//   part-local triangles; `BVHInstance::transform` lifts them to world.
// - `USE_SSE` below is unconditionally defined, so the SSE slab test is
//   always the one compiled in. `BINS` is the binned-SAH bin count used by
//   `BVH::FindBestSplitPlane`; several fixed-size arrays there are sized
//   `BINS - 1`, so changing it changes that function's stack layout.
// - Threading: `Intersect` on a built `BVH`/`TLAS` only reads, so concurrent
//   traces of the same structure are fine. `Build()` is NOT: it writes
//   `mesh->tri[i].centroid` back into the shared `BvhMesh`, so no two BVHs
//   may build over one mesh concurrently, and no trace may run against a
//   mesh that is being rebuilt.
// - Ownership: `BVH`/`BvhMesh`/`TLAS` are non-copyable and free their own
//   MALLOC64/new[] buffers. Their cross-pointers (`BVH::mesh`,
//   `BvhMesh::bvh`, `TLAS::blas`) are non-owning back-pointers; see the
//   destructor comments below and `src/bvh.cpp` for the leak this fixed.
// - Capacity limits: a hit packs instance and primitive into one 32-bit word
//   (12 + 20 bits), and TLAS child links pack two 16-bit node indices. Both
//   ceilings are documented at the types below.

#include "tri.h"   // Tri, TriEx, mat4

// enable the use of SSE in the AABB intersection function
#define USE_SSE

// bin count for binned BVH building
#define BINS 8

// Forward declarations
class BvhMesh;

// minimalist AABB struct with grow functionality
//
// Build-time scratch box, used by the SAH binning in
// `BVH::FindBestSplitPlane` and as `BVHInstance::bounds`. The default state is
// the *empty* sentinel bmin=+1e30, bmax=-1e30 (an inverted box), which is what
// `grow(aabb&)` tests for before merging — merging an untouched box would
// otherwise poison the result.
//
// `area()` returns the sum of the three distinct face areas, i.e. HALF the
// true surface area. The SAH only ever compares these values against each
// other, so the missing factor of two cancels. On an empty box the extent is
// negative and the product overflows, so never call `area()` before at least
// one `grow()`.
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
//
// Has no default member initialisers and `BVHRay`'s constructor does not touch
// it, so a freshly declared `BVHRay` carries a garbage `hit`. The caller must
// seed `hit.t` with the current best distance (which doubles as the miss
// sentinel — traversal only records hits closer than it) and, if it inspects
// `instPrim` afterwards, seed that too. See `world_tracer.cpp`, which sets
// `hit.t = best_t` and `hit.instPrim = 0xFFFFFFFF` before every trace.
//
// `instPrim` unpacks as `instPrim >> 20` = instance index and
// `instPrim & 0xFFFFF` = index into `BvhMesh::tri`. That caps a single mesh at
// 2^20 triangles and a TLAS at 2^12 instances; beyond either, the fields wrap
// silently. `world_tracer.cpp` range-checks the primitive index for exactly
// this reason.
struct Intersection
{
	float t;		// intersection distance along ray
	float u, v;		// barycentric coordinates of the intersection
	uint instPrim;	// instance index (12 bit) and primitive index (20 bit)
};

// ray struct, prepared for SIMD AABB intersection
//
// One cache line. Each of O/D/rD is unioned with a `__m128` so the SSE slab
// test can load them without a shuffle; the trailing `dummy` float is the .w
// lane those loads pull in and is otherwise unused.
//
// The constructor only splats 1.0f into O4/D4/rD4 — it is a "make the SIMD
// lanes finite" guard, not an initialisation. The caller is responsible for
// all four of:
//   - `O`  ray origin, in the space the BVH was built in
//   - `D`  ray direction (need not be normalised; `hit.t` is then in units of
//          |D| rather than distance)
//   - `rD` componentwise 1/D — traversal reads it and never derives it
//   - `hit.t` see `Intersection` above
// `BVHInstance::Intersect` is the one place that fills `rD` for you, on the
// local-space ray it builds internally.
struct ALIGN(64) BVHRay
{
	BVHRay() { O4 = D4 = rD4 = _mm_set1_ps( 1 ); }
	union { struct { float3 O; float dummy1; }; __m128 O4; };
	union { struct { float3 D; float dummy2; }; __m128 D4; };
	union { struct { float3 rD; float dummy3; }; __m128 rD4; };
	Intersection hit; // total ray size: 64 bytes
};

// 32-byte BVH node struct
//
// Two 16-byte halves, each an AABB corner (12 bytes) plus one packed uint in
// the .w slot that the corner's `float3` does not occupy — `float3` is
// deliberately 12 bytes and unaligned in `precomp.h` precisely so this overlay
// works. Assigning through `aabbMin`/`aabbMax` writes 12 bytes and leaves
// `leftFirst`/`triCount` intact; assigning through `aabbMin4`/`aabbMax4`
// writes all 16 and destroys them.
//
// Field meanings depend on `triCount`:
//   - leaf   (triCount > 0): `leftFirst` is the first index into `BVH::triIdx`
//     and `triCount` is how many consecutive entries belong to this node.
//   - interior (triCount == 0): `leftFirst` is the index of the left child in
//     `BVH::bvhNode`; the right child is always `leftFirst + 1`.
// Empty leaves are never produced, which is why `isLeaf()` can be a bare
// `triCount > 0` test.
//
// The SSE slab test masks the .w lane off before comparing, so the packed
// integers never leak into the intersection maths.
//
// `CalculateNodeCost()` is the un-split SAH cost of this node (half-area times
// triangle count), compared against the best split cost in `Subdivide`.
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
//
// A binned-SAH BVH over one `BvhMesh`. `bvhNode` is a flat node pool (root at
// 0, index 1 skipped so sibling pairs stay cache-line aligned, hence
// `nodesUsed` starting at 2) and `triIdx` is the permutation of triangle
// indices that leaves slice into.
//
// Lifetime and ownership
//   - Constructed either from a mesh (builds immediately) or from
//     previously-serialised nodes (installs without building).
//   - Owns `bvhNode` (MALLOC64) and `triIdx` (new[]) and frees both.
//   - `mesh` is a NON-owning back-pointer and must outlive the BVH. In
//     production both live in a `BLASEntry` (MatterSurfaceLib) behind
//     separate `unique_ptr`s.
//   - Non-copyable and non-movable by omission — pass by pointer/reference.
//
// Threading
//   - `Intersect` is read-only and safe to call concurrently on one instance.
//   - `Build` mutates the shared `BvhMesh` (it recomputes every
//     `Tri::centroid`) as well as this object, so it must not overlap any
//     trace or any other build over the same mesh.
//
// Split policy is not textbook SAH: `FindBestSplitPlane` blends the SAH cost
// with a quadratic penalty on imbalance (BALANCE_WEIGHT in `src/bvh.cpp`) and
// `Subdivide` force-splits leaves above 4 triangles and accepts up to a 50%
// cost regression. `TryMedianSplit` is the fallback when binning degenerates.
// Expect trees that are flatter and deeper than a pure-SAH build.
class ALIGN(64) BVH
{
public:
	BVH() = default;
	// Allocates the node pool and builds immediately. Pass
	// subdiv_to_one_prim = true to split down to single-triangle leaves; it is
	// applied BEFORE the build, so the tree is built once. (Setting the member
	// afterwards and calling Build() again works too, but pays for two builds.)
	BVH( BvhMesh* mesh, bool subdiv_to_one_prim = false );
	// Install a previously-built BVH (from disk) without rebuilding. nodes/triIdx
	// are copied; nodes_used is the live node count. mesh must outlive this BVH.
	BVH( BvhMesh* mesh, const BVHNode* nodes, uint nodes_used, const uint* tri_idx );
	// Owns bvhNode (MALLOC64) and triIdx (new[]); `mesh` is a back-pointer and is
	// NOT owned. Without this every BVH ever built was leaked -- BLASManager
	// erases entries on release and destroys whole scratch/staging managers per
	// part load, so a streaming world leaked roughly a decoded sector twice per
	// sector. Copying is deleted rather than implemented: these are held through
	// unique_ptr and a value copy would double-free.
	~BVH();
	BVH( const BVH& ) = delete;
	BVH& operator=( const BVH& ) = delete;
	// Rebuild the whole tree from `mesh` in place. Called for you by the
	// mesh-taking constructor; call it again only after mutating triangles or
	// after flipping `subdivToOnePrim`. Recomputes every `Tri::centroid` in
	// the shared mesh, so it is not safe against a concurrent trace.
	void Build();
	// Trace one ray against this BLAS. Reads `ray.O/D/rD` and treats
	// `ray.hit.t` as the incoming best distance, overwriting `ray.hit` only on
	// a closer hit; a miss leaves `ray.hit` untouched. `instanceIdx` is packed
	// into the top 12 bits of `hit.instPrim` and is otherwise unused, so a
	// direct (non-TLAS) caller passes 0.
	void Intersect( BVHRay& ray, uint instanceIdx );
	// Triangle count of the mesh this BLAS indexes, or 0 when no mesh is
	// attached (a default-constructed BVH). Read-only window onto the private
	// back-pointer, for diagnostics that have a BVH but not its BvhMesh --
	// `bvh_analyzer` needs it to report per-instance triangle totals. Defined
	// out of line because BvhMesh is only forward-declared in this header.
	uint TriangleCount() const;
private:
	// Build internals. `centroidMin`/`centroidMax` are in/out scratch threaded
	// through the recursion rather than recomputed: `UpdateNodeBounds` writes
	// the node's AABB *and* overwrites both with that node's centroid bounds,
	// which the following `Subdivide`/`FindBestSplitPlane` then bin against.
	// `nodePtr` is the shared node-pool bump allocator (`nodesUsed`).
	//
	// `FindBestSplitPlane` returns 1e30f when no axis yields a non-degenerate
	// split (all centroids coincident); the caller keeps the node as a leaf.
	// `TryMedianSplit` is the rescue path when binning puts every triangle on
	// one side; it partitions about the spatial median and, failing that,
	// insertion-sorts the range and halves it by count — O(n^2) in the node's
	// triangle count, so it is only viable because it runs on small leaves.
	void Subdivide( uint nodeIdx, uint depth, uint& nodePtr, float3& centroidMin, float3& centroidMax );
	void UpdateNodeBounds( uint nodeIdx, float3& centroidMin, float3& centroidMax );
	float FindBestSplitPlane( BVHNode& node, int& axis, int& splitPos, float3& centroidMin, float3& centroidMax );
	bool TryMedianSplit( uint nodeIdx, int axis, float3& centroidMin, float3& centroidMax, uint& leftCount );
	// Non-owning back-pointer to the triangle payload; must outlive this BVH.
	BvhMesh* mesh = 0;
	// Built state, public because `blas_manager`, `bvh_analyzer` and the part
	// serializer read and write it directly.
	//   triIdx     owned new[mesh->triCount]; the permutation leaf ranges
	//              index into. triIdx[k] is an index into mesh->tri.
	//   nodesUsed  live node count in bvhNode. Starts at 2 after a build
	//              (index 1 is intentionally skipped for alignment) and is NOT
	//              initialised by the default constructor.
	//   bvhNode    owned MALLOC64 pool, allocated for 2*triCount+padding
	//              nodes regardless of how many the build actually uses.
public:
	uint* triIdx = 0;
	uint nodesUsed;
	BVHNode* bvhNode = 0;
	// When set, `Subdivide` splits all the way down to single-triangle leaves
	// instead of stopping on the SAH cost test. Set it *before* calling
	// `Build()`; prefer the constructor's `subdiv_to_one_prim` argument, which
	// does exactly that and avoids building the tree twice.
	bool subdivToOnePrim = false; // for TLAS experiment
};

// minimalist mesh class
//
// The triangle payload a `BVH` indexes: `tri` (positions + centroid, consumed
// by build and intersection) and the parallel `triEx` (UVs, shading normals,
// material id, tint, baked AO — consumed only by shading). Both are indexed
// identically, both are MALLOC64 buffers owned here, and `triCount` counts
// entries in each.
//
// Sizing gotcha: `sizeof(TriEx)` is 96, not a multiple of 64, so the byte
// count handed to MALLOC64 must be rounded up to a multiple of 64 or
// `aligned_alloc` rejects it for odd triangle counts. Both the constructor in
// `src/bvh.cpp` and `blas_manager.cpp` do that round-up explicitly; any new
// allocation site has to as well.
//
// Non-copyable. `bvh` is a NON-owning back-pointer (the owning `unique_ptr`
// lives in `BLASEntry`), and is left null by every constructor in this
// library — only external code fills it in.
class BvhMesh
{
public:
	BvhMesh() = default;
	BvhMesh( uint primCount );
	// Owns tri and triEx (both MALLOC64). `bvh` is a back-pointer and is NOT
	// owned -- BLASEntry holds the BVH through its own unique_ptr, so freeing it
	// here would double-free. See ~BVH for why these destructors exist.
	~BvhMesh();
	BvhMesh( const BvhMesh& ) = delete;
	BvhMesh& operator=( const BvhMesh& ) = delete;
	Tri* tri = 0;			// triangle data for intersection
	TriEx* triEx = 0;		// triangle data for shading
	// Entry count for BOTH tri and triEx.
	int triCount = 0;
	// Non-owning back-pointer, never set by this library; freeing it here
	// would double-free the BLASEntry's unique_ptr.
	BVH* bvh = 0;
	// Legacy vertex-array slots from the upstream template. No constructor in
	// this repo allocates them and nothing reads them; they stay null.
	float3* P = 0, * N = 0;
};


// BVH instance, for TLAS
//
// One placement of a BLAS in world space: a non-owning `bvh` pointer, the
// object->world `transform` with its cached inverse, the world-space `bounds`
// the TLAS builds over, and `idx`, the instance id packed into the top 12 bits
// of `Intersection::instPrim` on a hit.
//
// Trivially copyable by design — `tlas_manager` copies instances into a
// contiguous vector and hands `TLAS` a pointer into it. The `bvh` it points at
// is owned elsewhere and must outlive both the instance and the TLAS built
// over it.
//
// `bounds` is only valid after `SetTransform`; the default-constructed state
// leaves it as the empty sentinel. `transform`/`invTransform` are `mat4` from
// `tri.h`: row-major float[16], column-vector convention, translation at
// cells 3/7/11.
class BVHInstance
{
public:
	BVH* bvh = 0;
	mat4 transform, invTransform;
	uint idx;
	aabb bounds;
	// Set the object->world transform. Also caches `invTransform` (a full
	// Gauss-Jordan inversion — not free, and it silently yields identity for a
	// singular input, see `mat4::Inverted` in tri.h) and recomputes `bounds`
	// by transforming all eight corners of the BVH root AABB. Call this before
	// building a TLAS over the instance; with a null or unbuilt `bvh` it
	// collapses `bounds` to a degenerate box at the origin.
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
//
// 32-byte node, same .w-slot overlay trick as `BVHNode`: the AABB corners are
// 12-byte `float3`s and the packed integers live in the fourth float of each
// half. Writing `aabbMin`/`aabbMax` preserves them; writing `aabbMin4`/
// `aabbMax4` does not.
//
//   leftRight  interior nodes only: `left | (right << 16)`, two 16-bit indices
//              into `TLAS::tlasNode`. The `left`/`right` ushort overlay is the
//              unpacked view of the same word. Zero is the leaf sentinel —
//              which is safe because node 0 is the root and can never be a
//              child. Capping child indices at 65535 is what the explicit
//              guard in `TLAS::BuildRecursive` enforces.
//   BLAS       leaf nodes only: index into `TLAS::blas`.
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

// Top-level acceleration structure over an array of `BVHInstance`.
//
// Construction builds immediately: the `(blas, N)` constructor allocates the
// node pool and calls `Build()` itself, so an extra `Build()` afterwards is a
// redundant second build, not a required step.
//
// Ownership: owns `tlasNode` (MALLOC64, sized for 2N nodes) and `nodeIdx`
// (new[N], the instance permutation the median splits reorder). `blas` is a
// NON-owning pointer into caller storage — `tlas_manager` points it at a
// `std::vector<BVHInstance>` and must therefore drop the TLAS before mutating
// that vector. Non-copyable.
//
// N == 0 is valid: the constructor floors its allocation at one node and one
// index, so an empty TLAS is a real empty tree (`nodesUsed == 1`, a zero-sized
// box at the origin) rather than a null dereference. Every instance must have
// had `SetTransform` called so its `bounds` are valid; instances with the
// empty-bounds sentinel are skipped when accumulating a node AABB.
//
// `Intersect` is read-only and recurses into `BVHInstance::Intersect`, which
// transforms the ray into each BLAS's local space. Its traversal stack is 64
// deep and silently drops the far child if it would overflow.
class ALIGN(64) TLAS
{
public:
	TLAS() = default;
	TLAS( BVHInstance* blas, int N );
	// Owns tlasNode (MALLOC64) and nodeIdx (new[]); `blas` is caller-owned.
	~TLAS();
	TLAS( const TLAS& ) = delete;
	TLAS& operator=( const TLAS& ) = delete;
	void Build();
	void Intersect( BVHRay& ray );
	
	// Public accessors for external classes
	uint GetBlasCount() const { return blasCount; }
	uint GetNodesUsed() const { return nodesUsed; }
	BVHInstance* GetBlas() const { return blas; }
	TLASNode* GetTlasNode() const { return tlasNode; }
	
private:
	// Top-down median build: picks the longest axis of the node AABB,
	// `std::nth_element`s `nodeIdx[first..first+count)` about the middle by
	// instance centroid, and recurses. O(N log N) overall.
	void BuildRecursive( uint nodeIndex, uint first, uint count );

public:
	// Made public for direct access by visualization and manager classes
	//   blas       non-owning pointer to `blasCount` contiguous instances
	//   nodesUsed  live nodes in `tlasNode`; 1 for the degenerate 0- and
	//              1-instance cases, otherwise 2*blasCount-1
	//   tlasNode   owned MALLOC64 pool, root at index 0
	//   nodeIdx    owned new[blasCount]; instance permutation, reordered
	//              in place by the median splits during Build
	BVHInstance* blas = 0;
	uint blasCount = 0, nodesUsed = 0;
	TLASNode* tlasNode = 0;
	uint* nodeIdx = 0;
};
