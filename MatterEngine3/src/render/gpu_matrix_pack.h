#pragma once

// MatterEngine3/src/render/gpu_matrix_pack.h
//
// The boundary between the engine's matrix layout and GLSL's.
//
// matter::Mat4f is ROW-major float[16]; a GLSL `mat4` in a uniform block, an
// SSBO or a push constant is COLUMN-major. Every matrix crossing into a shader
// goes through pack_glsl_mat4 below, so the transpose lives in exactly one
// place — vk_pipeline.cpp and vk_scene_renderer.cpp store GpuMat4, never a raw
// Mat4f, in their GPU-facing structs.
//
// Getting this wrong does not fail to compile and does not crash; it renders a
// transposed world. If a matrix reaches a shader without passing through here,
// that is the bug to look for first.

#include "matter/math_types.h"

namespace viewer {

// A GLSL `mat4` as bytes: 16 floats in column-major order, 64 bytes, which is
// both the std140 and std430 size and alignment for a mat4. vk_pipeline.cpp
// static_asserts the 64 bytes, since push-constant and buffer layouts are laid
// out against it.
struct GpuMat4 {
    float elements[16]{};
};

// Transpose a row-major Mat4f into GLSL column-major order. This is the only
// transpose on the CPU->GPU path: shaders receive the matrix as authored, so
// `world_to_clip * vec4(p, 1)` in GLSL means the same thing as
// mat4_mul/transform on the CPU side.
inline GpuMat4 pack_glsl_mat4(const matter::Mat4f& matrix) {
    GpuMat4 packed{};
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            packed.elements[column * 4 + row] = matrix.m[row * 4 + column];
        }
    }
    return packed;
}

} // namespace viewer
