#pragma once
#include "internal.h"
#include "fat_primitive.h"
#include <map>

namespace matter::geometry::detail {
// Private, owned postorder field program. No caller or legacy scene pointers.
struct FieldInstruction {
    FatPrim primitive{};
    bool leaf = false, grouped = false;
    float smoothing = 0;
    std::vector<size_t> inputs;
    std::vector<int> operations; // 0 union, 1 subtract, 2 intersect
};
struct CsgField {
    std::vector<FieldInstruction> program;
    data::Bounds bounds{};
    float spacing = 0.1f;
    float isovalue = 0;
    uint64_t storage = 0;
};
CsgField lower_csg(const data::RecipeNode &,
    const std::map<std::string,const data::RecipeNode *> &, CsgMeshingSettings,
    jobs::JobMonitor &, jobs::ResourceUsage &);
float sample_csg(const CsgField &, data::Vec3, std::vector<float> &, std::vector<float> &,
    jobs::JobMonitor &, jobs::ResourceUsage &);
// Empty boolean results have no mesh, rather than an invalid empty prototype.
std::optional<data::MeshPrototype> mesh_csg(const CsgField &, const data::RecipeNode &,
    jobs::JobMonitor &, jobs::ResourceUsage &);
} // namespace matter::geometry::detail
