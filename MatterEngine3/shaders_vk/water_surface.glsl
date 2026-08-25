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

struct WaterFieldGpuRecord {
    vec4 origin_cell_size;
    uvec4 extent_generation;
    uvec2 runtime_digest;
    uvec2 presentation_digest;
    vec4 wave_bands[WATER_WAVE_BAND_COUNT];
    uvec4 appearance;
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
    uint feature;
    bool valid;
};

struct WaterDualPhase {
    vec2 age_seconds;
    vec2 weight;
};

struct WaterSurfaceState {
    vec3 shading_normal;
    float roughness;
    WaterFieldSample field;
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
    float normal_y = sqrt(max(0.0, 1.0 - dot(field_b.yz, field_b.yz)));
    result.surface_height = field_a.x;
    result.depth = field_a.y;
    result.velocity = vec3(field_a.z, field_b.x, field_a.w);
    result.base_normal = normalize(vec3(field_b.y, normal_y, field_b.z));
    result.turbulence = field_b.w;
    result.aeration = classification.r;
    result.foam_potential = classification.g;
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
    return mix(vec3(1.0) - response, vec3(1.0), driver);
}

bool water_evaluate_surface(uint slot, uint generation, uint material_id,
                            vec2 world_xz, vec3 geometric_normal,
                            float animation_time_seconds, float base_roughness,
                            out WaterSurfaceState state) {
    state.shading_normal = normalize(geometric_normal);
    state.roughness = clamp(base_roughness, 0.0, 1.0);
    state.field = water_invalid_sample();
    state.animated = false;
    if (!water_sample_field(slot, generation, material_id, world_xz,
                            state.field))
        return false;
    WaterFieldGpuRecord record = water_field_records[slot];
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
    state.shading_normal = animated;
    state.roughness = clamp(
        base_roughness + 0.05 * min(slope_length, 1.0) +
        0.08 * clamp(state.field.turbulence, 0.0, 1.0), 0.0, 1.0);
    state.animated = true;
    return true;
}

#endif
