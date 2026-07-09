#include "particle_flow.h"
#include <cmath>

namespace pf {

// Task 2 implements Bias (steer+force) and Drag. Task 3 extends this file
// with Curl/Adhere/Separate and implements Sim::attract_dir/surface_normal.

V3 field_steer_dir(const Sim& s, const FieldConfig& f, uint32_t slot) {
    (void)s; (void)slot;
    switch (f.type) {
        case FieldType::Bias: return normalize(f.dir);
        default:              return {0, 0, 0};
    }
}

V3 field_force(const Sim& s, const FieldConfig& f, uint32_t slot) {
    switch (f.type) {
        case FieldType::Bias: return normalize(f.dir);
        case FieldType::Drag: {
            const float* v = const_cast<Sim&>(s).vel_data();
            return V3{v[3*slot], v[3*slot+1], v[3*slot+2]} * (-f.k);
        }
        default: return {0, 0, 0};
    }
}

// Stubs completed in Task 3.
V3 Sim::attract_dir(uint32_t, V3) { return {0, 0, 0}; }
V3 Sim::surface_normal(V3, float, bool* ok) const { if (ok) *ok = false; return {0,0,0}; }

} // namespace pf
