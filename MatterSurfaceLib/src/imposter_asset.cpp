#include "../include/imposter_asset.h"
#include "../include/mesh_simplifier.hpp"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <sys/stat.h>

namespace imposter_asset {

uint64_t compute_imp_hash(const ImpGenParams& p) {
    return part_asset::fnv1a64(&p, sizeof(p)) ^ static_cast<uint64_t>(kFormatVersion);
}

std::string cache_path(uint64_t hash) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
    return std::string("imposters/") + buf + ".imp";
}

bool save(const std::string&, const ImposterAsset&, uint64_t) { return false; }
bool load(const std::string&, uint64_t, uint64_t, ImposterAsset&) { return false; }
bool build_cage(const std::vector<Tri>&, const ImpGenParams&, uint64_t, ImposterAsset&) { return false; }
bool bake_displacement_cpu(const std::vector<Tri>&, ImposterAsset&) { return false; }

} // namespace imposter_asset
