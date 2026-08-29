#ifndef MATTER_WATER_SURFACE_GLSL
#define MATTER_WATER_SURFACE_GLSL

#extension GL_EXT_nonuniform_qualifier : require

#ifndef WATER_SET
#error WATER_SET must name the raster or RT descriptor set
#endif
#ifndef WATER_A_BINDING
#error WATER_A_BINDING must name the continuous field-A binding
#endif
#ifndef WATER_B_BINDING
#error WATER_B_BINDING must name the continuous field-B binding
#endif
#ifndef WATER_C_BINDING
#error WATER_C_BINDING must name the nearest classification binding
#endif
#ifndef WATER_D_BINDING
#error WATER_D_BINDING must name the continuous local-override binding
#endif
#ifndef WATER_RECORD_BINDING
#error WATER_RECORD_BINDING must name the immutable record binding
#endif

const uint WATER_FIELD_SLOT_COUNT = 8u;
const uint WATER_INVALID_SLOT = 0xffffffffu;
const uint WATER_SURFACE_MATERIAL_FLAG = 1u << 4;
const int WATER_BACKTRACE_STEPS = 3;
const int WATER_WAVE_BAND_COUNT = 3;
const int WATER_PHASE_COUNT = 2;
const float WATER_PI = 3.14159265358979323846;
const float WATER_TWO_PI = 6.28318530717958647692;
const float WATER_MAX_SHADING_SPEED = 20.0;
const float WATER_MAX_SLOPE = 4.0;
const float WATER_MIN_HEMISPHERE_DOT = 0.05;

layout(set = WATER_SET, binding = WATER_A_BINDING)
    uniform sampler2D water_field_a[WATER_FIELD_SLOT_COUNT];
layout(set = WATER_SET, binding = WATER_B_BINDING)
    uniform sampler2D water_field_b[WATER_FIELD_SLOT_COUNT];
layout(set = WATER_SET, binding = WATER_C_BINDING)
    uniform sampler2D water_field_c[WATER_FIELD_SLOT_COUNT];
layout(set = WATER_SET, binding = WATER_D_BINDING)
    uniform sampler2D water_field_d[WATER_FIELD_SLOT_COUNT];

struct WaterFieldGpuRecord {
    vec4 origin_cell_size;
    uvec4 extent_generation;
    uvec2 runtime_digest;
    uvec2 presentation_digest;
    vec4 wave_bands[WATER_WAVE_BAND_COUNT];
    uvec4 appearance;
    vec4 optics_shallow;
    vec4 optics_deep;
    vec4 optics_scattering;
    vec4 optics_misc;
    vec4 foam_controls;
    vec4 foam_response;
};

layout(set = WATER_SET, binding = WATER_RECORD_BINDING, std430)
    readonly buffer WaterFieldRecords {
        WaterFieldGpuRecord water_field_records[];
    };

struct WaterFieldSample {
    float surface_height;
    float depth;
    vec3 velocity;
    vec3 base_normal;
    float turbulence;
    float aeration;
    float foam_potential;
    float local_foam_multiplier;
    float local_threshold_offset;
    float local_wave_multiplier;
    uint feature;
    bool valid;
};

struct WaterOpticalState {
    vec3 transmittance;
    vec3 scattering_color;
    float bottom_visibility;
    float reflection_weight;
    float coherent_transmission_weight;
    float diffuse_scattering_weight;
};

struct WaterFoamState {
    float macro_mask;
    float breakup_detail;
    float coverage;
    float local_multiplier;
    float threshold_offset;
    float wave_multiplier;
};

struct WaterDualPhase {
    vec2 age_seconds;
    vec2 weight;
};

struct WaterSurfaceState {
    vec3 shading_normal;
    float roughness;
    WaterFieldSample field;
    WaterOpticalState optics;
    WaterFoamState foam;
    float reactivity;
    bool animated;
};

WaterFieldSample water_invalid_sample() {
    WaterFieldSample result;
    result.surface_height = 0.0;
    result.depth = 0.0;
    result.velocity = vec3(0.0);
    result.base_normal = vec3(0.0, 1.0, 0.0);
    result.turbulence = 0.0;
    result.aeration = 0.0;
    result.foam_potential = 0.0;
    result.local_foam_multiplier = 1.0;
    result.local_threshold_offset = 0.0;
    result.local_wave_multiplier = 1.0;
    result.feature = 0u;
    result.valid = false;
    return result;
}

vec2 water_clamp_length(vec2 value, float maximum) {
    float magnitude = length(value);
    return magnitude > maximum && magnitude > 0.0
        ? value * (maximum / magnitude) : value;
}

bool water_sample_field(uint slot, uint generation, uint material_id,
                        vec2 world_xz, out WaterFieldSample result) {
    result = water_invalid_sample();
    if (slot >= WATER_FIELD_SLOT_COUNT || generation == 0u ||
        slot >= water_field_records.length())
        return false;
    WaterFieldGpuRecord record = water_field_records[slot];
    if (record.extent_generation.w == 0u ||
        record.extent_generation.z != generation ||
        record.appearance.w == 0u || record.appearance.x != material_id ||
        record.extent_generation.x == 0u || record.extent_generation.y == 0u ||
        !(record.origin_cell_size.z > 0.0))
        return false;
    vec2 relative =
        (world_xz - record.origin_cell_size.xy) / record.origin_cell_size.z;
    vec2 extent = vec2(record.extent_generation.xy);
    if (any(lessThan(relative, vec2(0.0))) ||
        any(greaterThanEqual(relative, extent)))
        return false;
    vec2 uv = relative / extent;
    uint descriptor_slot = nonuniformEXT(slot);
    vec4 classification =
        textureLod(water_field_c[descriptor_slot], uv, 0.0);
    if (classification.b < 0.5) return false;
    vec4 field_a = textureLod(water_field_a[descriptor_slot], uv, 0.0);
    vec4 field_b = textureLod(water_field_b[descriptor_slot], uv, 0.0);
    vec4 field_d = textureLod(water_field_d[descriptor_slot], uv, 0.0);
    float normal_y = sqrt(max(0.0, 1.0 - dot(field_b.yz, field_b.yz)));
    result.surface_height = field_a.x;
    result.depth = field_a.y;
    result.velocity = vec3(field_a.z, field_b.x, field_a.w);
    result.base_normal = normalize(vec3(field_b.y, normal_y, field_b.z));
    result.turbulence = field_b.w;
    result.aeration = classification.r;
    result.foam_potential = classification.g;
    result.local_foam_multiplier = field_d.r;
    result.local_threshold_offset = field_d.g;
    result.local_wave_multiplier = field_d.b;
    result.feature = uint(floor(classification.a * 6.0 + 0.5));
    result.valid = true;
    return true;
}

vec2 water_backtrace_rk2(uint slot, uint generation, uint material_id,
                         vec2 xz, float age_s) {
    float dt = age_s / float(WATER_BACKTRACE_STEPS);
    for (int step = 0; step < WATER_BACKTRACE_STEPS; ++step) {
        WaterFieldSample at_current;
        if (!water_sample_field(slot, generation, material_id, xz,
                                at_current))
            break;
        vec2 v0 = water_clamp_length(at_current.velocity.xz,
                                     WATER_MAX_SHADING_SPEED);
        vec2 midpoint = xz - 0.5 * dt * v0;
        WaterFieldSample at_midpoint;
        if (!water_sample_field(slot, generation, material_id, midpoint,
                                at_midpoint))
            break;
        vec2 vm = water_clamp_length(at_midpoint.velocity.xz,
                                     WATER_MAX_SHADING_SPEED);
        vec2 next = xz - dt * vm;
        WaterFieldSample at_next;
        if (!water_sample_field(slot, generation, material_id, next, at_next))
            break;
        xz = next;
    }
    return xz;
}

WaterDualPhase water_wrapped_phase(float time_seconds, float period_seconds) {
    WaterDualPhase phase;
    phase.age_seconds = mod(
        mod(vec2(time_seconds, time_seconds + 0.5 * period_seconds),
            vec2(period_seconds)) + vec2(period_seconds),
        vec2(period_seconds));
    vec2 sine = sin(WATER_PI * phase.age_seconds / period_seconds);
    phase.weight = sine * sine;
    phase.weight /= max(phase.weight.x + phase.weight.y, 1.0e-8);
    return phase;
}

float water_feature_response(uint feature) {
    if (feature >= 2u && feature <= 5u) return 1.0;
    return feature == 1u ? 0.35 : 0.0;
}

vec3 water_band_responses(WaterFieldGpuRecord record,
                          WaterFieldSample field) {
    float speed = length(field.velocity.xz);
    float slope = length(field.base_normal.xz);
    float feature = water_feature_response(field.feature);
    float broad_driver = clamp(
        0.55 * field.depth / (field.depth + 1.5) +
        0.35 * speed / (speed + 2.0) + 0.10 * feature, 0.0, 1.0);
    float chop_driver = clamp(
        0.35 * speed / (speed + 2.0) + 0.20 * slope +
        0.35 * field.turbulence + 0.10 * feature, 0.0, 1.0);
    float capillary_driver = clamp(
        (0.45 * speed / (speed + 1.0) + 0.45 * field.turbulence +
         0.10 * feature) *
        (1.0 - 0.75 * clamp(field.foam_potential, 0.0, 1.0)), 0.0, 1.0);
    vec3 driver = vec3(broad_driver, chop_driver, capillary_driver);
    vec3 response = vec3(record.wave_bands[0].w,
                         record.wave_bands[1].w,
                         record.wave_bands[2].w);
    return mix(vec3(1.0) - response, vec3(1.0), driver) *
           max(field.local_wave_multiplier, 0.0);
}

float water_feature_foam_bias(uint feature) {
    if (feature == 2u) return 0.10;
    if (feature == 3u) return 0.30;
    if (feature == 4u) return 0.35;
    if (feature == 5u) return 0.25;
    return feature == 1u ? 0.02 : 0.0;
}

float water_foam_driver(WaterFieldSample field) {
    float primary = clamp(field.foam_potential, 0.0, 1.0);
    float turbulence_support =
        0.10 * clamp(field.turbulence, 0.0, 1.0);
    float aeration_support = 0.20 * clamp(field.aeration, 0.0, 1.0);
    float feature_support =
        min(water_feature_foam_bias(field.feature), 0.15);
    return clamp(primary + turbulence_support + aeration_support +
                     feature_support,
                 0.0, 1.0);
}

float water_breakup_noise(vec2 position, float scale_m) {
    float scale = max(scale_m, 0.001);
    float phase = position.x * (2.17 / scale) +
                  position.y * (3.11 / scale);
    float secondary = position.x * (5.03 / scale) -
                      position.y * (1.73 / scale);
    return clamp(0.5 + 0.32 * sin(phase) +
                 0.18 * sin(secondary + sin(phase)), 0.0, 1.0);
}

WaterOpticalState water_evaluate_optics(
    WaterFieldGpuRecord record, float optical_distance_m,
    float foam_coverage) {
    WaterOpticalState optics;
    float optical_distance = max(optical_distance_m, 0.0);
    float bounded_foam_coverage = clamp(foam_coverage, 0.0, 1.0);
    float depth_blend = smoothstep(1.5, 4.0, optical_distance);
    vec3 absorption = mix(record.optics_shallow.rgb,
                          record.optics_deep.rgb, depth_blend);
    float absorption_distance = max(
        0.001, mix(record.optics_shallow.a, record.optics_deep.a,
                   depth_blend));
    float optical_depth = optical_distance / absorption_distance;
    optics.transmittance = exp(-absorption * optical_depth);
    optics.bottom_visibility = clamp(
        dot(optics.transmittance, vec3(0.2126, 0.7152, 0.0722)),
        0.0, 1.0);
    float ior = clamp(record.optics_misc.y, 1.0, 2.5);
    float f0 = (ior - 1.0) / (ior + 1.0);
    optics.reflection_weight = f0 * f0;
    optics.coherent_transmission_weight = clamp(
        (1.0 - optics.reflection_weight) * optics.bottom_visibility *
        (1.0 - bounded_foam_coverage *
                   clamp(record.foam_response.z, 0.0, 1.0)),
        0.0, 1.0);
    float optical_remaining = max(
        0.0, 1.0 - optics.reflection_weight -
             optics.coherent_transmission_weight);
    float depth_scattering = 1.0 - exp(
        -optical_distance / max(record.optics_scattering.a, 0.001));
    optics.diffuse_scattering_weight = min(
        optical_remaining,
        depth_scattering + bounded_foam_coverage *
                               max(record.foam_response.y, 0.0));
    optics.scattering_color = mix(
        record.optics_scattering.rgb, vec3(0.92, 0.97, 1.0),
        bounded_foam_coverage);
    return optics;
}

bool water_evaluate_surface(uint slot, uint generation, uint material_id,
                            vec2 world_xz, vec3 geometric_normal,
                            float animation_time_seconds, float base_roughness,
                            out WaterSurfaceState state) {
    state.shading_normal = normalize(geometric_normal);
    state.roughness = clamp(base_roughness, 0.0, 1.0);
    state.field = water_invalid_sample();
    state.optics.transmittance = vec3(1.0);
    state.optics.scattering_color = vec3(0.0);
    state.optics.bottom_visibility = 1.0;
    state.optics.reflection_weight = 0.0;
    state.optics.coherent_transmission_weight = 1.0;
    state.optics.diffuse_scattering_weight = 0.0;
    state.foam.macro_mask = 0.0;
    state.foam.breakup_detail = 0.0;
    state.foam.coverage = 0.0;
    state.foam.local_multiplier = 1.0;
    state.foam.threshold_offset = 0.0;
    state.foam.wave_multiplier = 1.0;
    state.reactivity = 0.0;
    state.animated = false;
    if (!water_sample_field(slot, generation, material_id, world_xz,
                            state.field))
        return false;
    WaterFieldGpuRecord record = water_field_records[slot];
    state.foam.local_multiplier = max(state.field.local_foam_multiplier, 0.0);
    state.foam.threshold_offset = state.field.local_threshold_offset;
    state.foam.wave_multiplier = max(state.field.local_wave_multiplier, 0.0);
    float foam_driver = water_foam_driver(state.field);
    state.foam.macro_mask = clamp(
        (foam_driver - record.foam_controls.x -
         state.foam.threshold_offset) * record.foam_controls.y,
        0.0, 1.0);
    vec2 foam_position = world_xz;
    if (record.foam_controls.z > 0.0) {
        float foam_age = mod(max(animation_time_seconds, 0.0),
                             max(record.foam_controls.z, 0.001));
        foam_position = water_backtrace_rk2(
            slot, generation, material_id, world_xz, foam_age);
    }
    state.foam.breakup_detail =
        water_breakup_noise(foam_position, record.foam_controls.w);
    state.foam.coverage = clamp(
        state.foam.macro_mask * state.foam.local_multiplier *
        mix(0.85, 1.0, state.foam.breakup_detail), 0.0, 1.0);

    state.optics = water_evaluate_optics(
        record, max(state.field.depth, 0.0), state.foam.coverage);
    state.reactivity = clamp(max(
        state.foam.coverage,
        0.35 * clamp(state.field.turbulence, 0.0, 1.0) +
        0.15 * clamp(state.field.aeration, 0.0, 1.0)), 0.0, 1.0);
    vec2 flow = state.field.velocity.xz;
    float flow_length = length(flow);
    flow = flow_length > 0.05 ? flow / flow_length : vec2(1.0, 0.0);
    vec3 responses = water_band_responses(record, state.field);
    vec2 slope = vec2(0.0);
    for (int band = 0; band < WATER_WAVE_BAND_COUNT; ++band) {
        vec4 wave = record.wave_bands[band];
        if (!(wave.x > 0.0) || !(wave.y >= 0.0) || !(wave.z > 0.0))
            continue;
        float angle = band == 0 ? 0.0 : (band == 1 ? 0.52 : -0.83);
        float ca = cos(angle);
        float sa = sin(angle);
        vec2 direction = vec2(flow.x * ca - flow.y * sa,
                              flow.x * sa + flow.y * ca);
        float period = max(0.25, wave.x / wave.z);
        WaterDualPhase phases =
            water_wrapped_phase(animation_time_seconds, period);
        float blended_gradient = 0.0;
        for (int phase_index = 0; phase_index < WATER_PHASE_COUNT;
             ++phase_index) {
            vec2 backtraced = water_backtrace_rk2(
                slot, generation, material_id, world_xz,
                phases.age_seconds[phase_index]);
            float theta = WATER_TWO_PI * dot(backtraced, direction) / wave.x +
                          WATER_TWO_PI * phases.age_seconds[phase_index] /
                              period;
            blended_gradient +=
                phases.weight[phase_index] * cos(theta);
        }
        slope += direction * min(wave.y, 1.0) * responses[band] *
                 blended_gradient;
    }
    float slope_length = length(slope);
    if (slope_length > WATER_MAX_SLOPE)
        slope *= WATER_MAX_SLOPE / slope_length;
    vec3 base = state.shading_normal;
    vec3 slope_world = vec3(slope.x, 0.0, slope.y);
    slope_world -= base * dot(slope_world, base);
    vec3 animated = normalize(base - slope_world);
    float hemisphere = dot(animated, base);
    if (hemisphere < WATER_MIN_HEMISPHERE_DOT) {
        vec3 tangent = animated - base * hemisphere;
        float tangent_length = length(tangent);
        animated = tangent_length > 1.0e-8
            ? base * WATER_MIN_HEMISPHERE_DOT +
                  tangent * (sqrt(1.0 - WATER_MIN_HEMISPHERE_DOT *
                                          WATER_MIN_HEMISPHERE_DOT) /
                             tangent_length)
            : base;
    }
    float normal_softening = state.foam.coverage *
                             clamp(record.foam_response.w, 0.0, 1.0);
    state.shading_normal = normalize(mix(animated, base, normal_softening));
    state.roughness = clamp(
        base_roughness + 0.05 * min(slope_length, 1.0) +
        0.08 * clamp(state.field.turbulence, 0.0, 1.0) +
        state.foam.coverage * clamp(record.foam_response.x, 0.0, 1.0),
        0.0, 1.0);
    state.animated = true;
    return true;
}

// Primary ray-generation pixels retain material identity but not the mesh
// draw's water slot. Resolve the one immutable field whose authored material
// and world bounds own this point. Eight bounded probes keep this identical
// to explicit-slot evaluation without adding another G-buffer attachment.
bool water_evaluate_surface_for_material(
    uint material_id, vec2 world_xz, vec3 geometric_normal,
    float animation_time_seconds, float base_roughness,
    out WaterSurfaceState state) {
    for (uint slot = 0u; slot < WATER_FIELD_SLOT_COUNT; ++slot) {
        if (slot >= water_field_records.length()) break;
        WaterFieldGpuRecord record = water_field_records[slot];
        if (record.extent_generation.w == 0u ||
            record.appearance.w == 0u || record.appearance.x != material_id)
            continue;
        if (water_evaluate_surface(
                slot, record.extent_generation.z, material_id, world_xz,
                geometric_normal, animation_time_seconds, base_roughness,
                state))
            return true;
    }
    state.shading_normal = normalize(geometric_normal);
    state.roughness = clamp(base_roughness, 0.0, 1.0);
    state.field = water_invalid_sample();
    state.animated = false;
    return false;
}

#endif
