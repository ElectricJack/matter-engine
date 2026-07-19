#include "world_lights.h"
#include "part_asset.h"   // fnv1a64 (MatterSurfaceLib/include, via -I../../MatterSurfaceLib/include)

#include <vector>

namespace world_lights {

uint64_t lights_fingerprint(const WorldLights& l) {
    std::vector<float> data;
    data.reserve(6 + l.spots.size() * 12);

    // sun_dir (3) + sun_color (3) + sky_color (3)
    data.push_back(l.sun_dir[0]);
    data.push_back(l.sun_dir[1]);
    data.push_back(l.sun_dir[2]);
    data.push_back(l.sun_color[0]);
    data.push_back(l.sun_color[1]);
    data.push_back(l.sun_color[2]);
    data.push_back(l.sky_color[0]);
    data.push_back(l.sky_color[1]);
    data.push_back(l.sky_color[2]);

    // Each spot: pos(3) + dir(3) + color(3) + range(1) + cos_inner(1) + cos_outer(1) = 12
    for (const auto& s : l.spots) {
        data.push_back(s.pos[0]);
        data.push_back(s.pos[1]);
        data.push_back(s.pos[2]);
        data.push_back(s.dir[0]);
        data.push_back(s.dir[1]);
        data.push_back(s.dir[2]);
        data.push_back(s.color[0]);
        data.push_back(s.color[1]);
        data.push_back(s.color[2]);
        data.push_back(s.range);
        data.push_back(s.cos_inner);
        data.push_back(s.cos_outer);
    }

    return part_asset::fnv1a64(data.data(), data.size() * sizeof(float));
}

} // namespace world_lights
