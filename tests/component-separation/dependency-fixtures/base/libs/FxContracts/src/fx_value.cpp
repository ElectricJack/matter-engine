#include <fx_contracts/fx_value.h>

#include <cmath>

namespace fx {
double fx_length(const Value& value) {
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}
}  // namespace fx
