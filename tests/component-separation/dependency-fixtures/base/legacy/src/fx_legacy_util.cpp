#include "../include/fx_legacy/fx_util.h"

double fx_legacy_smoothstep(double edge0, double edge1, double value) {
    const double span_width = edge1 - edge0;
    const double raw_fraction = (value - edge0) / span_width;
    const double lower_bounded = raw_fraction < 0.0 ? 0.0 : raw_fraction;
    const double unit_interval = lower_bounded > 1.0 ? 1.0 : lower_bounded;
    return unit_interval * unit_interval * (3.0 - 2.0 * unit_interval);
}
