#include <fx_geometry/fx_mesh.h>

#include <cmath>

namespace fx {
double fxgeo_triangle_area(const Value& a, const Value& b, const Value& c) {
    const double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
    const double vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
    const double cross_x = uy * vz - uz * vy;
    const double cross_y = uz * vx - ux * vz;
    const double cross_z = ux * vy - uy * vx;
    const double twice_area = std::sqrt(cross_x * cross_x + cross_y * cross_y + cross_z * cross_z);
    return 0.5 * twice_area;
}
}  // namespace fx
