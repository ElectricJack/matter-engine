#include "world_lights.h"
#include "part_asset.h"   // fnv1a64 (MatterSurfaceLib/include, via -I../../MatterSurfaceLib/include)

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace world_lights {

// Normalize a 3-float vector in-place. No-op if near-zero.
static void normalize3(float v[3]) {
    float len = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (len > 1e-9f) { v[0] /= len; v[1] /= len; v[2] /= len; }
}

bool parse_lights(const std::string& manifest_path, WorldLights& out, std::string& err) {
    out = WorldLights{};   // reset to defaults first
    err.clear();

    std::ifstream in(manifest_path);
    if (!in) {
        // Missing file: return defaults and true (per spec).
        return true;
    }

    std::string line;
    while (std::getline(in, line)) {
        // Trim leading/trailing whitespace.
        size_t b = line.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) continue;
        size_t e = line.find_last_not_of(" \t\r\n");
        std::string trimmed = line.substr(b, e - b + 1);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        // Check first token.
        std::istringstream ss(trimmed);
        std::string first;
        ss >> first;
        if (first != "light") continue;  // non-light lines are ignored

        std::string kind;
        if (!(ss >> kind)) {
            err = "light: bad line (no kind): " + trimmed;
            return false;
        }

        if (kind == "sun") {
            // light sun <dx> <dy> <dz>  <r> <g> <b>
            float dx, dy, dz, r, g, b;
            if (std::sscanf(trimmed.c_str(), "light sun %f %f %f %f %f %f",
                            &dx, &dy, &dz, &r, &g, &b) != 6) {
                err = "light: bad sun line: " + trimmed;
                return false;
            }
            out.sun_dir[0] = dx; out.sun_dir[1] = dy; out.sun_dir[2] = dz;
            normalize3(out.sun_dir);
            out.sun_color[0] = r; out.sun_color[1] = g; out.sun_color[2] = b;
        } else if (kind == "sky") {
            // light sky <r> <g> <b>
            float r, g, b;
            if (std::sscanf(trimmed.c_str(), "light sky %f %f %f", &r, &g, &b) != 3) {
                err = "light: bad sky line: " + trimmed;
                return false;
            }
            out.sky_color[0] = r; out.sky_color[1] = g; out.sky_color[2] = b;
        } else if (kind == "spot") {
            // light spot <px> <py> <pz>  <dx> <dy> <dz>  <r> <g> <b>  <range> <inner_deg> <outer_deg>
            float px, py, pz, dx, dy, dz, r, g, b, range, inner_deg, outer_deg;
            if (std::sscanf(trimmed.c_str(),
                            "light spot %f %f %f %f %f %f %f %f %f %f %f %f",
                            &px, &py, &pz, &dx, &dy, &dz, &r, &g, &b,
                            &range, &inner_deg, &outer_deg) != 12) {
                err = "light: bad spot line: " + trimmed;
                return false;
            }
            SpotLight sp;
            sp.pos[0] = px; sp.pos[1] = py; sp.pos[2] = pz;
            sp.dir[0] = dx; sp.dir[1] = dy; sp.dir[2] = dz;
            normalize3(sp.dir);
            sp.color[0] = r; sp.color[1] = g; sp.color[2] = b;
            sp.range = range;
            constexpr float kPiOver180 = 3.14159265358979323846f / 180.0f;
            sp.cos_inner = std::cos(inner_deg * kPiOver180);
            sp.cos_outer = std::cos(outer_deg * kPiOver180);
            out.spots.push_back(sp);
        } else {
            err = "light: bad " + kind + " line: " + trimmed;
            return false;
        }
    }
    return true;
}

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
