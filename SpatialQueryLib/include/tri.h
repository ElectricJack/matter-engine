#pragma once

// Core triangle + transform types shared across the engine.
//
// These are the engine's universal geometry interchange types: meshing,
// flattening, LOD baking, part serialization and triangle emission all speak
// Tri/TriEx, and most of them never touch an acceleration structure. They used
// to live in bvh.h, which made ~13 BVH-free translation units include the whole
// BVH/TLAS surface just to name a triangle.

#include "precomp.h"

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
// tint is per-triangle RGBA copied from the nearest particle; a (alpha) is the
// blend strength against the material albedo. (1,1,1,0) = no tint (neutral).
struct TriEx {
    float2 uv0, uv1, uv2; float3 N0, N1, N2; int materialId; float4 tint;
    // Per-vertex baked ambient occlusion in [0,1]; 1.0 = fully unoccluded.
    // Defaulted so unbaked meshes (e.g. marching cubes before any bake) render bright.
    float ao0 = 1.0f, ao1 = 1.0f, ao2 = 1.0f;
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
