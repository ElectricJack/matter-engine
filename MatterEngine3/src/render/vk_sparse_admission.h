#pragma once
#include "vk_sparse_voxel.h"
#include "scene_admission.h"

namespace viewer {
// Internal boundary: preflight BOTH halves before preparing either GPU scene.
bool preflight_shared_forest(matter::VulkanDevice&,
    const std::vector<SparseSharedObject>&,const std::vector<SparseShadowObject>&,
    std::string& error);
}
