#include "check.h"
#include "memory_fixture_io.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include "matter/vulkan_device.h"
#include "render/gpu_matrix_pack.h"
#include "render/lod_distance.h"
#include "render/matrix_math.h"
#include "render/raster_mesh.h"
#include "render/streamline_bridge.h"
#include "render/vk_temporal.h"
#include "render/vk_gi_math.h"
#include "render/vk_device_internal.h"
#include "render/vk_instance_cache.h"
#include "render/vk_lighting_controls.h"
#include "render/vk_scene_renderer.h"
#include "render/vk_volumetrics.h"
#include "matter/atmosphere_lighting.h"
#include "ui.h"
#include "viewer_commands.h"
#include "provider/sector_resolver.h"
// Last: this reaches the legacy precompiled-header namespace imports.
#include "impostor_bake.h"

namespace {
#include "vulkan_smoke_cpu_cases.inc"
}

int main() {
    run_vulkan_cpu_contract_tests();
    return check_summary();
}
