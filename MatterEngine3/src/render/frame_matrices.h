#pragma once

// MatterEngine3/src/render/frame_matrices.h
//
// The per-frame camera matrix set used by the Vulkan renderer, and the single
// validated way to build one from a matter::CameraDesc.
//
// Everything downstream of the camera reads a FrameMatrices rather than
// rebuilding matrices of its own: vk_scene_renderer uploads world_to_clip for
// drawing, the GPU culler and shaders_vk/cull.comp consume frustum_planes, and
// vk_temporal.h keeps four of these per frame (current/previous, jittered and
// unjittered) for the temporal resolve.
//
// Conventions:
//   - Matrices are matter::Mat4f in the engine's row-major float[16] layout;
//     multiply with mat4_mul, and pack for GLSL with pack_glsl_mat4
//     (gpu_matrix_pack.h), which transposes.
//   - Projection is right-handed, zero-to-one depth, REVERSED-Z: the near plane
//     maps to 1 and the far plane to 0. Depth comparisons, depth clears and the
//     near/far recovery arithmetic downstream all assume this.
//   - Distances are world units; the vertical FOV in CameraDesc is radians.

#include <cstdint>
#include <string>

#include "matter/camera.h"

namespace viewer {

// One frame's camera transforms plus the derived culling frustum. Plain value
// type: copyable, holds no GPU resources, and cheap enough that vk_temporal
// keeps several copies. Build it with build_frame_matrices() rather than
// filling the members by hand — the four matrices must stay mutually consistent
// (world_to_clip == view_to_clip * world_to_view, clip_to_world its inverse,
// frustum_planes extracted from world_to_clip), and only that function checks
// the camera is well formed.
struct FrameMatrices {
    matter::Mat4f world_to_view;
    matter::Mat4f view_to_clip;
    matter::Mat4f world_to_clip;
    matter::Mat4f clip_to_world;
    // Sub-pixel projection offset in internal-render pixels. Unjittered
    // matrices keep this at zero; temporal candidates fill it explicitly.
    float jitter_pixels[2]{};
    // Six world-space half-space planes as (nx, ny, nz, d); a point is inside
    // when dot(plane, (p, 1)) >= 0. Order is the extractor's: left, right,
    // bottom, top, then slots 4 and 5, whose NEAR/FAR LABELS ARE SWAPPED under
    // reversed-Z (slot 4 is the far test, slot 5 the near test — see the long
    // note in frame_matrices.cpp). Every consumer today tests all six
    // uniformly; anything that wants one specific plane by index must account
    // for that swap.
    float frustum_planes[6][4]{};
};

// Build the frame's matrices and frustum from a camera and the framebuffer
// extent (which supplies the aspect ratio; both must be non-zero). Returns
// false with `error` set — and `frame` LEFT UNTOUCHED — when the camera is
// unusable: degenerate extent, near/far not satisfying 0 < near < far or not
// representable, an up vector parallel to the view direction, a singular
// world_to_clip, or degenerate planes. On success `frame` is fully overwritten,
// including jitter_pixels, which is zeroed here; the temporal path fills it
// afterwards (vk_temporal.cpp::jitter_frame).
bool build_frame_matrices(const matter::CameraDesc& camera, std::uint32_t width,
                          std::uint32_t height, FrameMatrices& frame,
                          std::string& error);

} // namespace viewer
