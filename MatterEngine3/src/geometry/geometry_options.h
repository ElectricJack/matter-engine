#pragma once
#include <cstdlib>
#include <cstring>

namespace geometry {
// Startup opt-in only. Scene presets and the subordinate terrain/cache/profile
// options cannot enable virtual geometry on their own.
inline bool pages_requested() {
    const char* value = std::getenv("MATTER_GEOMETRY_PAGES");
    return value && std::strcmp(value, "1") == 0;
}
} // namespace geometry
