#pragma once

#include <cstdint>
#include <string>

#include "matter/camera.h"

namespace viewer {

struct FrameMatrices {
    matter::Mat4f world_to_view;
    matter::Mat4f view_to_clip;
    matter::Mat4f world_to_clip;
    matter::Mat4f clip_to_world;
    // Sub-pixel projection offset in internal-render pixels. Unjittered
    // matrices keep this at zero; temporal candidates fill it explicitly.
    float jitter_pixels[2]{};
    float frustum_planes[6][4]{};
};

bool build_frame_matrices(const matter::CameraDesc& camera, std::uint32_t width,
                          std::uint32_t height, FrameMatrices& frame,
                          std::string& error);

} // namespace viewer
