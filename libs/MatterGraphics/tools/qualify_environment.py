"""Build-time source contracts formerly scanned by environment runtime tests.

These are wiring/layout checks, not substitutes for the numerical cloud,
froxel, memory and lighting regressions that still run in CTest.
"""
from pathlib import Path
import sys


def qualify(repository):
    def read(relative):
        return (repository / relative).read_text(encoding="utf-8")

    def require(condition, description):
        if not condition:
            raise ValueError(description)

    def contains(source, *fragments):
        return all(fragment in source for fragment in fragments)

    common = read("MatterEngine3/shaders_vk/vol_common.glsl")
    require(not any("const uint " + name in common for name in ("VOL_W", "VOL_H", "VOL_D"))
            and contains(common, "slice_to_depth(float slice_index, float depth_slices)",
                         "depth_to_slice_n(float depth, float depth_slices)"),
            "Volumetric slice mapping must use runtime depth dimensions")
    require(contains(common, "GpuCloudLayer in matter/cloud_layers.h (96 bytes std430)",
                     "vec4 weather_scale_influence_detail_scale_detail_erosion;",
                     "vec4 shape_bias_padding;"),
            "Cloud SSBO mirror must retain its 96-byte stride and control vec4s")

    density = read("MatterEngine3/shaders_vk/cloud_density.glsl")
    volume = read("MatterEngine3/shaders_vk/vol_density.comp")
    implementation = read("libs/MatterGraphics/src/native/vk_volumetrics.cpp")
    composite = read("MatterEngine3/shaders_vk/composite.frag")
    require("CloudDensitySample evaluate_cloud_density" in density
            and contains(volume, "ENHANCED_CLOUDS", "vol_cloud_density")
            and contains(implementation, "VK_FORMAT_R16_SFLOAT", "density_pipelines_[count][enhanced]",
                         "cloud_density_dummy_"),
            "Enhanced clouds must share density and allocate optional R16F extinction")
    cloud_debug = composite.find("Cloud density must precede every GBuffer/sky early-out")
    sky_early_out = composite.find("if (normal_length_squared <= 1e-20)")
    require(0 <= cloud_debug < sky_early_out
            and "for (int z = 0; z < depth_slices; ++z)" in composite
            and "fog_albedo * non_cloud_extinction + vec3(0.99) * cloud_extinction" in volume,
            "Cloud debug must cover sky rays; enhanced scattering must avoid double counting")
    require(contains(density,
                     "if (L.weather_scale_influence_detail_scale_detail_erosion.y > 0.0)",
                     "if (L.shape_bias_padding.x != 0.0)",
                     "result.full = profile * L.max_density * full_shape;",
                     "if (L.weather_scale_influence_detail_scale_detail_erosion.w > 0.0)"),
            "Neutral cloud density must retain authored full-octave multiplication")
    require("enhanced_clouds_requested_ =\n        matter::enhanced_cloud_lighting(vol, shadows);" in implementation,
            "Enhanced-cloud allocation must use the canonical predicate")

    engine = read("MatterEngine3/src/matter_engine.cpp")
    editor = read("MatterEditor/src/main.cpp")
    require(contains(engine, "vol_effective_xy_scale", "vol_effective_depth_slices"),
            "Frame stats must receive effective froxel enums")
    require(contains(editor, "last_rejected_froxel_generation",
                     "frame_stats.vol_resource_generation !=\n                last_rejected_froxel_generation",
                     "set_enum(b->instance(), *xy", "set_enum(b->instance(), *depth",
                     "b->set_dirty(false)", "volumetric froxel allocation rejected:"),
            "Rejected froxel generations must restore both fields, clear dirty state and log once")
    require(contains(implementation, "fail_next_bundle_descriptor_allocation_for_test_",
                     "destroy_froxel_bundle(bundle)"),
            "Descriptor failure must expose its seam and destroy partial froxel bundles")
    renderer = read("MatterEngine3/src/render/vk_scene_renderer.cpp")
    require(contains(renderer, "VkClearColorValue neutral_volume", "vol_dummy_3d_", "submit_immediate"),
            "No-RT volumetric fallback must be explicitly cleared")
    smoke = read("MatterEngine3/tests/vulkan_smoke_tests.cpp")
    require(contains(smoke, "volumetrics_integrated_view()", "(active.width + 7) / 8", "seen_frame_slots"),
            "Resize smoke must observe bundle replacement, ceil coverage and both frame slots")
    print("PASS GFX-ENVIRONMENT-SOURCES: cloud layout/density, froxel dimensions/rejection/retirement/fallback/resize")


if __name__ == "__main__":
    qualify(Path(sys.argv[1]).resolve())
