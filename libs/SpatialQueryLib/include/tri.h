#pragma once

// Core triangle + transform types shared across the engine.
//
// These are the engine's universal geometry interchange types: meshing,
// flattening, LOD baking, part serialization and triangle emission all speak
// Tri/TriEx, and most of them never touch an acceleration structure. They used
// to live in bvh.h, which made ~13 BVH-free translation units include the whole
// BVH/TLAS surface just to name a triangle.
//
// Layout, units and conventions
// -----------------------------
// - `Tri` is exactly 64 bytes (one cache line) and `ALIGN(64)`; `TriEx` is 96
//   bytes and unaligned. Arrays of either are allocated with MALLOC64, which
//   on POSIX needs the byte count rounded up to a multiple of 64 — trivial for
//   `Tri`, but a real trap for `TriEx` at odd counts. Both `BvhMesh`'s
//   constructor and `blas_manager.cpp` do that round-up; new call sites must
//   too.
// - `Tri` and `TriEx` are parallel arrays: index i of one describes the same
//   triangle as index i of the other. `TriEx` is optional — BVH construction
//   and ray/triangle intersection touch only `Tri`.
// - No coordinate space is implied. Producers use part-local space for BLAS
//   geometry and world space for flattened geometry; the space is a property
//   of the producer, not of the type.
// - Nothing here is thread-affine, but `BVH::Build` writes `Tri::centroid`
//   back into a shared triangle array, so a build and a trace over the same
//   `Tri[]` must not overlap.
// - `mat4` here is a separate type from `libs/MathLib`'s `mm::Mat4`, though
//   the two share a memory layout (row-major float[16], translation at cells
//   3/7/11) and are copied element-wise at the boundary — see the conversion
//   loop in `libs/MatterSurfaceLib/src/tlas_manager.cpp`.

#include "precomp.h"

// minimalist triangle struct
//
// Three positions plus a cached centroid, each unioned with a `__m128` so the
// builder can load them without shuffles. Winding is whatever the producer
// emitted; nothing in this library normalises or checks it.
//
// `centroid` is build scratch, not authored data: `BVH::Build` recomputes it
// as the arithmetic mean of the three vertices for every triangle in the mesh
// before partitioning. Do not store anything else there, and do not assume it
// is populated on a `Tri` that has never been through a build.
struct ALIGN(64) Tri
{
	// union each float3 with a 16-byte __m128 for faster BVH construction
	union { float3 vertex0; __m128 v0; };
	union { float3 vertex1; __m128 v1; };
	union { float3 vertex2; __m128 v2; };
	union { float3 centroid; __m128 centroid4; }; // total size: 64 bytes
};

// Shading payload paralleling `Tri` — same index, same count, allocated
// alongside it as `BvhMesh::triEx`. Entirely unused by BVH construction and
// traversal; consumers read it only after a hit resolves to a triangle index.
//
// `uv0/1/2` are per-vertex texture coordinates, `N0/N1/N2` per-vertex shading
// normals (unit length by convention, but not normalised here — note that
// shading normals are NOT the geometric normal, and AO/visibility traces must
// use the geometric one, see docs on the VT enrich normal fix).
// `materialId` indexes the engine's material table.
//
// sizeof(TriEx) is 96 bytes, which is not a multiple of 64 — see the
// allocation note in the file header before MALLOC64-ing an array of these.
// additional triangle data, for texturing and shading
// tint is per-triangle RGBA copied from the nearest particle; a (alpha) is the
// blend strength against the material albedo. (1,1,1,0) = no tint (neutral).
struct TriEx {
    float2 uv0, uv1, uv2; float3 N0, N1, N2; int materialId; float4 tint;
    // Per-vertex baked ambient occlusion in [0,1]; 1.0 = fully unoccluded.
    // Defaulted so unbaked meshes (e.g. marching cubes before any bake) render bright.
    float ao0 = 1.0f, ao1 = 1.0f, ao2 = 1.0f;
};

// Simple matrix class for transforms
//
// 4x4 float matrix stored ROW-MAJOR in `cell[16]`: row i occupies
// cell[4i .. 4i+3], `operator()(i, j)` is cell[i*4 + j], and the translation
// lives in the last column at cells 3, 7 and 11. The convention is
// column-vector (`M * v`), which is what `TransformPoint` implements — so
// composing A then B means `B * A`.
//
// This is the same layout as `mm::Mat4` in `libs/MathLib`, which is why
// `tlas_manager.cpp` converts between them with a plain 16-element copy. It is
// the TRANSPOSE of GLSL/Vulkan's default column-major `mat4`, so it cannot be
// memcpy'd into a uniform block without transposing.
//
// Default-constructed to identity. Trivially copyable; no dynamic state.
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
	
	// Full 4x4 Gauss-Jordan inversion with partial pivoting — not free, and
	// called once per `BVHInstance::SetTransform`.
	//
	// Fails SILENTLY: when a pivot's magnitude falls below 1e-8f the matrix is
	// treated as singular and IDENTITY is returned. There is no error channel,
	// so a degenerate transform produces a plausible-looking instance whose
	// rays are never transformed. Note this threshold differs from MathLib's
	// `inverse_or_identity`, which rejects only an exactly-zero pivot; the two
	// are not interchangeable on near-singular input (see the long comment at
	// `libs/MathLib/include/matter_math.h`).
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
	
	// Apply the full transform including translation (treats `v` as a point,
	// w = 1). The perspective row is ignored — there is no w divide — so this
	// is only correct for affine matrices, which is all `BVHInstance` ever
	// stores. Use `TransformVector` below for directions and normals-as-
	// directions: it drops the translation (w = 0) but, being the same 3x3, it
	// is NOT the inverse-transpose and will skew normals under non-uniform
	// scale.
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
