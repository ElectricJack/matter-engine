#include <fx_graphics/fx_draw.h>

namespace fx {
unsigned fxgfx_pack_color(double red, double green, double blue) {
    const unsigned red_byte = static_cast<unsigned>(red * 255.0 + 0.5) & 0xffu;
    const unsigned green_byte = static_cast<unsigned>(green * 255.0 + 0.5) & 0xffu;
    const unsigned blue_byte = static_cast<unsigned>(blue * 255.0 + 0.5) & 0xffu;
    return (red_byte << 16) | (green_byte << 8) | blue_byte;
}

double fxgfx_luminance(const Value& color) {
    return 0.2126 * color.x + 0.7152 * color.y + 0.0722 * color.z;
}
}  // namespace fx
