#pragma once
#include "material_registry.h"
#include "matter/world_definition.h"
#include <string>
#include <vector>
namespace authored_world_cache {
struct Material {
    std::string name;
    int index = -1;
    MaterialDef definition{};
};
struct Snapshot {
    matter::WorldDefinition world;
    std::vector<Material> materials;
};
// Pure decoding never mutates the registry; replay occurs only after validation.
bool capture(const matter::WorldDefinition&, std::string& blob);
bool decode(const std::string& blob, Snapshot& out);
bool replay_materials(const Snapshot& snapshot);
} // namespace authored_world_cache
