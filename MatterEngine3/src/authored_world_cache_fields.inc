// Explicit scalar schema. Update schema version when adding or changing fields.
template <class A> void fields(A& a, matter::WorldFluidCollider& v) {
    a(v.shape, v.radius_m, v.half_extents_m, v.center_m);
}
template <class A> void fields(A& a, matter::WorldRoot& v) {
    a(v.id, v.module, v.params_json, v.transform, v.fluid_collider, v.expand,
      v.tileset);
}
template <class A> void fields(A& a, matter::WorldLight& v) {
    a(v.kind, v.position, v.color, v.intensity, v.range, v.source_radius,
      v.casts_shadow, v.direction, v.inner_cone_degrees, v.outer_cone_degrees);
}
template <class A> void fields(A& a, matter::FogSettings& v) {
    a(v.density, v.floor, v.falloff, v.color, v.wind, v.clouds, v.cloud_count,
      v.height_layer, v.min_height, v.max_height, v.noise_scale);
}
template <class A> void fields(A& a, matter::WorldStreamingRing& v) {
    a(v.radius, v.rung);
}
template <class A> void fields(A& a, matter::WorldCameraSettings& v) {
    a(v.authored, v.position, v.target);
}
template <class A> void fields(A& a, matter::WorldSettings& v) {
    a(v.nested_sectors, v.volumetric_sectors, v.sector_size, v.y_min, v.y_max,
      v.sun_direction, v.sun_color, v.sky_color, v.sun_angular_diameter_deg, v.fog,
      v.camera, v.streaming_rings, v.terrain_bands, v.atmosphere, v.volumetrics,
      v.cloud_shadows);
}
template <class A> void fields(A& a, matter::RawEntityRecipe& v) {
    a(v.authored_id, v.display_name, v.parent_authored_id, v.components_json);
}
template <class A> void fields(A& a, matter::WorldMaterial& v) {
    a(v.name, v.index, v.detail_module, v.detail_density);
}
template <class A> void fields(A& a, matter::WorldPropSpec& v) {
    a(v.name, v.label, v.doc, v.units, v.kind, v.number_default, v.bool_default,
      v.string_default, v.enum_labels, v.has_range, v.min, v.max, v.step);
}
template <class A> void fields(A& a, matter::CloudLayer& v) {
    a(v.enabled, v.min_height, v.max_height, v.max_density, v.falloff_min,
      v.falloff_max, v.noise_scale, v.octaves, v.lacunarity, v.gain, v.coverage, v.wind,
      v.weather_scale, v.weather_influence, v.detail_scale, v.detail_erosion,
      v.shape_bias);
}
template <class A> void fields(A& a, matter::CloudShadowSettings& v) {
    a(v.enabled, v.near_resolution, v.near_depth_slices, v.near_coverage_m,
      v.far_resolution, v.far_depth_slices, v.far_coverage_m, v.filter_scale,
      v.update_fraction);
}
template <class A> void fields(A& a, matter::AtmosphereSettings& v) {
    a(v.sea_level_y, v.rayleigh_scale, v.mie_scale, v.mie_anisotropy, v.ozone_scale,
      v.ground_albedo);
}
template <class A> void fields(A& a, matter::VulkanVolumetricsSettings& v) {
    a(v.enabled, v.temporal_blend, v.phase_g, v.vol_debug_view, v.froxel_xy_scale,
      v.froxel_depth_slices, v.local_sun_march_steps, v.local_sun_march_distance_m,
      v.multiple_scattering_orders, v.multiple_scattering_strength, v.powder_strength);
}
template <class A> void fields(A& a, MaterialDef& v) {
    a(v.albedo, v.roughness, v.metallic, v.emission, v.translucency, v.ior,
      v.flatShading, v.mergeGroup, v.meshingAlgorithm, v.groundTilesetSlot,
      v.groundMacroSlot, v.opacity, v.transmission, v.emissionColor, v.absorptionColor,
      v.absorptionDistance, v.thickness, v.subsurface, v.scatteringColor,
      v.scatteringDistance, v.anisotropy, v.clearcoat, v.clearcoatRoughness,
      v.specularStrength, v.specularTint, v.alphaCutoff, v.shadowOpacity,
      v.surfaceFlags);
}
