// Compatibility entrypoint; implementation belongs to MatterGraphics.
#include "../../../libs/MatterGraphics/src/native/vk_volumetrics.cpp"

#include "vk_scene_renderer.h"

void viewer::VkVolumetrics::set_lighting(const VkSceneLighting& lighting) {
    set_sun_direction(lighting.sun_direction);
}
