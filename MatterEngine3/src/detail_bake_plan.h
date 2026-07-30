#pragma once
// detail_bake_plan.h — which detail-tileset atlases a loaded world needs baked,
// and which materials each one binds (chart-VT spec Phase 3 / plan contract C3).
//
// Before this existed the answer was implicit: "every `tileset: true` world
// root, always bound to material 16". `defineMaterial(name, { detail: '...' })`
// generalizes it — any material can declare a detail scene — so the mapping
// became an explicit, order-defined plan that LocalProvider::run_tileset_deferred
// walks.
//
// Pure and dependency-free (no provider, no GPU) so the ordering and merging
// rules are unit-testable without standing up a world.

#include "matter/world_definition.h"

#include <string>
#include <vector>

namespace tileset {

// Material bound by the deprecated `tileset: true` world root. Before
// defineMaterial() existed this was the only way to texture ground, and the
// binding was hardcoded at the slot-load call site; it survives as an alias so
// worlds authored against it (StreamMountain, Meadow, FloorDemo) keep behaving
// exactly as before.
inline constexpr int kDeprecatedTilesetRootMaterial = 16;  // DIRT

// One automated detail-tileset bake: a Tileset module plus the materials bound
// to whichever slot its `.gtex` lands in.
struct DetailBakeRequest {
    std::string      module;
    std::string      params_json = "{}";
    int              texels_per_meter = 0;   // 0 = the module's own density
    std::vector<int> materials;              // bound on a successful slot load
    bool             from_tileset_root = false;  // deprecated alias path
};

// A `tileset: true` manifest root, reduced to what the plan needs.
struct DetailBakeRoot {
    std::string module;
    std::string params_json = "{}";
};

// Build the ordered plan.
//
// Order: deprecated roots first in manifest order, then declared materials in
// declaration order. That ordering is load-bearing — it is what makes a world
// that only uses `tileset: true` land in exactly the slots it landed in before
// this file existed.
//
// Merging: requests agreeing on (module, params, density) collapse into one, so
// two materials naming the same detail scene share a single bake and a single
// slot. That sharing is the reason a small slot pool is workable at all.
inline std::vector<DetailBakeRequest> plan_detail_bakes(
    const std::vector<DetailBakeRoot>& tileset_roots,
    const std::vector<matter::WorldMaterial>& materials) {
    std::vector<DetailBakeRequest> out;

    auto add = [&out](const std::string& module, const std::string& params_json,
                      int texels_per_meter, int material, bool from_root) {
        for (DetailBakeRequest& existing : out) {
            if (existing.module != module ||
                existing.params_json != params_json ||
                existing.texels_per_meter != texels_per_meter)
                continue;
            for (const int bound : existing.materials)
                if (bound == material) return;
            existing.materials.push_back(material);
            return;
        }
        DetailBakeRequest request;
        request.module = module;
        request.params_json = params_json;
        request.texels_per_meter = texels_per_meter;
        request.materials.push_back(material);
        request.from_tileset_root = from_root;
        out.push_back(std::move(request));
    };

    for (const DetailBakeRoot& root : tileset_roots) {
        if (root.module.empty()) continue;
        add(root.module, root.params_json.empty() ? "{}" : root.params_json, 0,
            kDeprecatedTilesetRootMaterial, true);
    }
    // A detail module referenced from a material carries no authored params —
    // the tileset module's own statics are the whole input.
    for (const matter::WorldMaterial& material : materials) {
        if (material.detail_module.empty() || material.index < 0) continue;
        add(material.detail_module, "{}", material.detail_density,
            material.index, false);
    }
    return out;
}

} // namespace tileset
