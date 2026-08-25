#include "river_float_system.h"

#include "matter/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace matter::river_float {
namespace {

constexpr float kWaterDensity = 1000.0f;
constexpr float kEpsilon = 1.0e-6f;
constexpr std::uint64_t kHashOffset = 14695981039346656037ULL;
constexpr std::uint64_t kHashPrime = 1099511628211ULL;

struct RiverBindingState {
    const void* context = nullptr;
    RiverBindingAcquire acquire = nullptr;
    RiverSampleFunction test_sample{};
    std::uint64_t test_generation = 0;
    std::shared_ptr<const RiverRuntimeBinding> cached_binding;
    std::uint64_t cached_tick = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t cached_generation = 0;
    bool test_mode = false;
    RiverFloatMeasurementHook measurement{};
};

struct ProductionSampleContext {
    const RiverBindingState* state = nullptr;
    const RiverRuntimeBinding* binding = nullptr;
    std::uint64_t generation = 0;
};

bool finite(float value) { return std::isfinite(value); }
bool finite(Float3 value) {
    return finite(value.x) && finite(value.y) && finite(value.z);
}
bool finite(Quaternion value) {
    return finite(value.x) && finite(value.y) && finite(value.z) && finite(value.w);
}

Float3 add(Float3 a, Float3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Float3 sub(Float3 a, Float3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Float3 mul(Float3 value, float scalar) {
    return {value.x * scalar, value.y * scalar, value.z * scalar};
}
float dot(Float3 a, Float3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
Float3 cross(Float3 a, Float3 b) {
    return {a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z,
            a.x*b.y - a.y*b.x};
}
float length(Float3 value) { return std::sqrt(dot(value, value)); }

bool normalize_quaternion(Quaternion value, Quaternion& output) {
    const float squared = value.x*value.x + value.y*value.y +
                          value.z*value.z + value.w*value.w;
    if (!finite(squared)) return false;
    if (squared <= kEpsilon*kEpsilon) {
        output = {0, 0, 0, 1};
        return true;
    }
    const float inverse = 1.0f / std::sqrt(squared);
    output = {value.x*inverse, value.y*inverse, value.z*inverse, value.w*inverse};
    return finite(output);
}

Quaternion multiply(Quaternion a, Quaternion b) {
    return {
        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z};
}

Float3 rotate(Quaternion rotation, Float3 value) {
    const Float3 q{rotation.x, rotation.y, rotation.z};
    const Float3 t = mul(cross(q, value), 2.0f);
    return add(value, add(mul(t, rotation.w), cross(q, t)));
}

void hash_byte(std::uint64_t& hash, std::uint8_t value) {
    hash ^= value;
    hash *= kHashPrime;
}
void hash_u32(std::uint64_t& hash, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        hash_byte(hash, static_cast<std::uint8_t>(value >> shift));
}
void hash_u64(std::uint64_t& hash, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8)
        hash_byte(hash, static_cast<std::uint8_t>(value >> shift));
}
void hash_float(std::uint64_t& hash, float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float width changed");
    std::memcpy(&bits, &value, sizeof(bits));
    hash_u32(hash, bits);
}
void hash_float3(std::uint64_t& hash, Float3 value) {
    hash_float(hash, value.x); hash_float(hash, value.y); hash_float(hash, value.z);
}

float projected_area(Float3 direction, Quaternion orientation, Float3 dimensions) {
    const Float3 x = rotate(orientation, {1, 0, 0});
    const Float3 y = rotate(orientation, {0, 1, 0});
    const Float3 z = rotate(orientation, {0, 0, 1});
    return std::fabs(dot(direction, x)) * dimensions.y * dimensions.z +
           std::fabs(dot(direction, y)) * dimensions.x * dimensions.z +
           std::fabs(dot(direction, z)) * dimensions.x * dimensions.y;
}

float projected_extent(Float3 direction, Quaternion orientation,
                       Float3 dimensions) {
    const Float3 x = rotate(orientation, {1, 0, 0});
    const Float3 y = rotate(orientation, {0, 1, 0});
    const Float3 z = rotate(orientation, {0, 0, 1});
    return std::fabs(dot(direction, x)) * dimensions.x +
           std::fabs(dot(direction, y)) * dimensions.y +
           std::fabs(dot(direction, z)) * dimensions.z;
}

RiverSampleStatus production_sample(const void* opaque, Float3 position,
                                    RiverFieldSample& output) noexcept {
    const auto& context = *static_cast<const ProductionSampleContext*>(opaque);
    if (context.binding == nullptr) return RiverSampleStatus::Dry;
    if (context.binding->generation() != context.generation)
        return RiverSampleStatus::Invalid;
    if (context.binding->sample(position, output)) return RiverSampleStatus::Wet;
    if (context.state == nullptr || context.state->acquire == nullptr)
        return RiverSampleStatus::Invalid;
    const std::shared_ptr<const RiverRuntimeBinding> current =
        context.state->acquire(context.state->context);
    if (!current || current.get() != context.binding ||
        current->generation() != context.generation)
        return RiverSampleStatus::Invalid;
    return RiverSampleStatus::Dry;
}

int compare_entity_ids(flecs::entity_t first, const void*,
                       flecs::entity_t second, const void*) {
    return (first > second) - (first < second);
}

void mark_invalid(flecs::entity entity, RiverFloatState& state) {
    if (state.disabled) return;
    if (state.consecutive_invalid < 8) ++state.consecutive_invalid;
    if (state.consecutive_invalid == 8) {
        state.disabled = true;
        if (!state.diagnostic_emitted) {
            MATTER_LOGE("river-float", "disabled entity=%llu after 8 invalid ticks\n",
                        static_cast<unsigned long long>(entity.id()));
            state.diagnostic_emitted = true;
        }
    }
}

} // namespace

bool valid_river_float_body(const RiverFloatBody& value) noexcept {
    const std::uint32_t count = static_cast<std::uint32_t>(value.probes_x) *
        static_cast<std::uint32_t>(value.probes_y) *
        static_cast<std::uint32_t>(value.probes_z);
    return finite(value.effective_density_kg_m3) &&
        value.effective_density_kg_m3 > 0.0f &&
        value.effective_density_kg_m3 <= 2000.0f &&
        finite(value.displaced_volume_scale) && value.displaced_volume_scale > 0.0f &&
        value.displaced_volume_scale <= 4.0f &&
        value.probes_x >= 1 && value.probes_x <= 4 &&
        value.probes_y >= 1 && value.probes_y <= 4 &&
        value.probes_z >= 1 && value.probes_z <= 4 && count <= 64 &&
        finite(value.probe_inset) && value.probe_inset >= 0.0f &&
        value.probe_inset <= 0.49f &&
        finite(value.buoyancy_response) && value.buoyancy_response >= 0.0f &&
        finite(value.longitudinal_drag) && value.longitudinal_drag >= 0.0f &&
        finite(value.lateral_drag) && value.lateral_drag >= 0.0f &&
        finite(value.vertical_drag) && value.vertical_drag >= 0.0f &&
        finite(value.angular_damping) && value.angular_damping >= 0.0f &&
        finite(value.max_force_per_probe_n) && value.max_force_per_probe_n > 0.0f &&
        finite(value.max_total_force_n) && value.max_total_force_n > 0.0f &&
        finite(value.diagnostic_color);
}

bool compute_river_float_forces(
    const RiverFloatBody& settings,
    const physics::BoxCollider& box,
    const ecs::LocalTransform& transform,
    const physics::PhysicsVelocity& velocity,
    const RiverSampleFunction& sample,
    float gravity_mps2,
    RiverFloatForceBuffer& output,
    RiverFloatDiagnostics& diagnostics) noexcept {
    output.count = 0;
    const std::uint64_t generation = diagnostics.binding_generation;
    diagnostics = {};
    diagnostics.binding_generation = generation;
    diagnostics.sample_checksum = kHashOffset;
    diagnostics.force_checksum = kHashOffset;
    hash_u64(diagnostics.sample_checksum, generation);
    hash_u64(diagnostics.force_checksum, generation);

    const bool valid_input = valid_river_float_body(settings) &&
        finite(box.center) && finite(box.rotation) && finite(box.half_extents) &&
        box.half_extents.x > 0.0f && box.half_extents.y > 0.0f &&
        box.half_extents.z > 0.0f && finite(transform.translation) &&
        finite(transform.rotation) && finite(transform.scale) &&
        transform.scale.x != 0.0f && transform.scale.y != 0.0f &&
        transform.scale.z != 0.0f && finite(velocity.linear) &&
        finite(velocity.angular) && finite(gravity_mps2) && gravity_mps2 > 0.0f &&
        sample.callback != nullptr;
    if (!valid_input) {
        diagnostics.hard_invalid = true;
        return false;
    }

    const Float3 scaled_half{
        box.half_extents.x * std::fabs(transform.scale.x),
        box.half_extents.y * std::fabs(transform.scale.y),
        box.half_extents.z * std::fabs(transform.scale.z)};
    if (!finite(scaled_half)) {
        diagnostics.hard_invalid = true;
        return false;
    }
    Quaternion body_orientation{};
    Quaternion collider_orientation{};
    Quaternion orientation{};
    if (!normalize_quaternion(transform.rotation, body_orientation) ||
        !normalize_quaternion(box.rotation, collider_orientation) ||
        !normalize_quaternion(multiply(body_orientation, collider_orientation),
                              orientation)) {
        diagnostics.hard_invalid = true;
        return false;
    }
    const Float3 scaled_center{box.center.x * transform.scale.x,
                               box.center.y * transform.scale.y,
                               box.center.z * transform.scale.z};
    const Float3 centre = add(transform.translation,
                              rotate(body_orientation, scaled_center));
    const Float3 dimensions{2.0f * scaled_half.x / settings.probes_x,
                            2.0f * scaled_half.y / settings.probes_y,
                            2.0f * scaled_half.z / settings.probes_z};
    const float represented_volume = dimensions.x * dimensions.y * dimensions.z *
                                     settings.displaced_volume_scale;
    const float total_volume = 8.0f * scaled_half.x * scaled_half.y * scaled_half.z;
    diagnostics.reference_mass_kg = settings.effective_density_kg_m3 * total_volume *
                                    settings.displaced_volume_scale;
    diagnostics.equilibrium_submerged_fraction =
        settings.effective_density_kg_m3 / kWaterDensity;
    if (!finite(dimensions) || !finite(represented_volume) ||
        !finite(diagnostics.reference_mass_kg)) {
        diagnostics.hard_invalid = true;
        return false;
    }

    const Float3 box_dimensions{2.0f * scaled_half.x, 2.0f * scaled_half.y,
                                2.0f * scaled_half.z};
    const float inset_scale = 1.0f - 2.0f * settings.probe_inset;
    for (std::uint32_t ix = 0; ix < settings.probes_x; ++ix) {
        for (std::uint32_t iy = 0; iy < settings.probes_y; ++iy) {
            for (std::uint32_t iz = 0; iz < settings.probes_z; ++iz) {
                const Float3 fraction{
                    (static_cast<float>(ix) + 0.5f) / settings.probes_x - 0.5f,
                    (static_cast<float>(iy) + 0.5f) / settings.probes_y - 0.5f,
                    (static_cast<float>(iz) + 0.5f) / settings.probes_z - 0.5f};
                const Float3 local{fraction.x * box_dimensions.x * inset_scale,
                                   fraction.y * box_dimensions.y * inset_scale,
                                   fraction.z * box_dimensions.z * inset_scale};
                const Float3 point = add(centre, rotate(orientation, local));
                if (!finite(point)) {
                    diagnostics.hard_invalid = true;
                    output.count = 0;
                    return false;
                }

                RiverFieldSample river{};
                const RiverSampleStatus status = sample.callback(sample.context, point, river);
                hash_byte(diagnostics.sample_checksum, static_cast<std::uint8_t>(status));
                hash_float3(diagnostics.sample_checksum, point);
                if (status == RiverSampleStatus::Dry) {
                    ++diagnostics.dry_probe_count;
                    continue;
                }
                if (status != RiverSampleStatus::Wet || !river.wet_valid ||
                    !finite(river.surface_position_m) || !finite(river.surface_normal) ||
                    !finite(river.velocity_mps) || !finite(river.depth_m) ||
                    !finite(river.turbulence) || !finite(river.aeration) ||
                    !finite(river.foam_potential)) {
                    diagnostics.hard_invalid = true;
                    output.count = 0;
                    return false;
                }
                ++diagnostics.wet_probe_count;
                hash_float3(diagnostics.sample_checksum, river.surface_position_m);
                hash_float3(diagnostics.sample_checksum, river.surface_normal);
                hash_float3(diagnostics.sample_checksum, river.velocity_mps);
                hash_float(diagnostics.sample_checksum, river.depth_m);
                hash_byte(diagnostics.sample_checksum,
                          static_cast<std::uint8_t>(river.feature));

                const float probe_height = projected_extent(
                    {0, 1, 0}, orientation, dimensions);
                if (!finite(probe_height) || probe_height <= kEpsilon) {
                    diagnostics.hard_invalid = true;
                    output.count = 0;
                    return false;
                }
                const float submerged = std::max(0.0f, std::min(1.0f,
                    (river.surface_position_m.y - (point.y - 0.5f * probe_height)) /
                    probe_height));
                const Float3 radial = sub(point, centre);
                const Float3 angular_point_velocity = cross(velocity.angular, radial);
                const Float3 point_velocity = add(velocity.linear, angular_point_velocity);
                if (!finite(angular_point_velocity) || !finite(point_velocity)) {
                    diagnostics.hard_invalid = true;
                    output.count = 0;
                    return false;
                }

                Float3 flow_axis{river.velocity_mps.x, 0.0f, river.velocity_mps.z};
                const float horizontal_speed = length(flow_axis);
                if (!finite(horizontal_speed)) {
                    diagnostics.hard_invalid = true;
                    output.count = 0;
                    return false;
                }
                flow_axis = horizontal_speed > kEpsilon
                    ? mul(flow_axis, 1.0f / horizontal_speed) : Float3{1, 0, 0};
                const Float3 lateral_axis{-flow_axis.z, 0.0f, flow_axis.x};
                const Float3 up_axis{0, 1, 0};
                const Float3 relative = sub(river.velocity_mps, point_velocity);
                if (!finite(relative)) {
                    diagnostics.hard_invalid = true;
                    output.count = 0;
                    return false;
                }
                const float longitudinal_speed = dot(relative, flow_axis);
                const float lateral_speed = dot(relative, lateral_axis);
                const float vertical_speed = dot(relative, up_axis);
                const float wet_scale = submerged;

                Float3 force{};
                if (river.feature != RiverFeature::Waterfall) {
                    force.y += kWaterDensity * represented_volume * gravity_mps2 *
                               submerged * settings.buoyancy_response;
                }
                const auto drag = [&](Float3 axis, float speed, float coefficient) {
                    const float area = projected_area(axis, orientation, dimensions);
                    return mul(axis, 0.5f * kWaterDensity * coefficient * area *
                               speed * std::fabs(speed) * wet_scale);
                };
                force = add(force, drag(flow_axis, longitudinal_speed,
                                        settings.longitudinal_drag));
                force = add(force, drag(lateral_axis, lateral_speed,
                                        settings.lateral_drag));
                force = add(force, drag(up_axis, vertical_speed,
                                        settings.vertical_drag));
                const float angular_speed = length(angular_point_velocity);
                if (angular_speed > kEpsilon && settings.angular_damping > 0.0f) {
                    const float area = projected_area(
                        mul(angular_point_velocity, 1.0f / angular_speed),
                        orientation, dimensions);
                    force = add(force, mul(angular_point_velocity,
                        -0.5f * kWaterDensity * settings.angular_damping * area *
                        angular_speed * wet_scale));
                }
                if (!finite(force)) {
                    diagnostics.hard_invalid = true;
                    output.count = 0;
                    return false;
                }
                float magnitude = length(force);
                if (!finite(magnitude)) {
                    diagnostics.hard_invalid = true;
                    output.count = 0;
                    return false;
                }
                if (magnitude > settings.max_force_per_probe_n) {
                    force = mul(force, settings.max_force_per_probe_n / magnitude);
                    magnitude = settings.max_force_per_probe_n;
                }
                if (magnitude > kEpsilon) output.rows[output.count++] = {force, point};
            }
        }
    }

    float sum_magnitudes = 0.0f;
    for (std::uint32_t i = 0; i < output.count; ++i)
        sum_magnitudes += length(output.rows[i].force_n);
    if (!finite(sum_magnitudes)) {
        diagnostics.hard_invalid = true;
        output.count = 0;
        return false;
    }
    if (sum_magnitudes > settings.max_total_force_n) {
        const float scale = settings.max_total_force_n / sum_magnitudes;
        for (std::uint32_t i = 0; i < output.count; ++i)
            output.rows[i].force_n = mul(output.rows[i].force_n, scale);
    }
    hash_u32(diagnostics.force_checksum, output.count);
    for (std::uint32_t i = 0; i < output.count; ++i) {
        hash_float3(diagnostics.force_checksum, output.rows[i].world_point_m);
        hash_float3(diagnostics.force_checksum, output.rows[i].force_n);
    }
    return true;
}

void register_river_float_systems(flecs::world& world) {
    world.component<RiverFloatState>("RiverFloatState");
    world.component<RiverBindingState>("RiverBindingState");
    world.set<RiverBindingState>({});

    world.observer<RiverFloatBody>("InitializeRiverFloatState")
        .event(flecs::OnAdd)
        .each([](flecs::entity entity, RiverFloatBody&) {
            if (!entity.has<RiverFloatState>()) entity.set<RiverFloatState>({});
        });

    auto measurement_begin = world.system<const RiverBindingState>(
            "MatterRiverFloatMeasurementBegin")
        .term_at(0).src<RiverBindingState>()
        .kind<RiverFloatForces>()
        .each([](const RiverBindingState& state) {
            if (state.measurement.begin != nullptr)
                state.measurement.begin(state.measurement.context);
        });
    measurement_begin.add<ecs::FixedPipelineSystem>();

    auto system = world.system<const RiverFloatBody,
                               const ecs::LocalTransform,
                               const physics::PhysicsVelocity,
                               const physics::BoxCollider,
                               const physics::RigidBody,
                               RiverFloatState>("MatterRiverFloatForces")
        .kind<RiverFloatForces>()
        .order_by(static_cast<flecs::entity_t>(0), compare_entity_ids)
        .each([](flecs::iter& iterator, std::size_t row,
                 const RiverFloatBody& settings,
                 const ecs::LocalTransform& transform,
                 const physics::PhysicsVelocity& velocity,
                 const physics::BoxCollider& box,
                 const physics::RigidBody& body,
                 RiverFloatState& state) {
            if (body.type != physics::RigidBodyType::Dynamic) return;
            flecs::world world = iterator.world();
            RiverBindingState* binding = world.try_get_mut<RiverBindingState>();
            if (binding == nullptr) return;
            const std::uint64_t tick = world.get<ecs::AnimationFixedState>().current_tick;
            if (binding->cached_tick != tick) {
                binding->cached_tick = tick;
                if (binding->test_mode) {
                    binding->cached_generation = binding->test_generation;
                    binding->cached_binding.reset();
                } else if (binding->acquire != nullptr) {
                    binding->cached_binding = binding->acquire(binding->context);
                    binding->cached_generation = binding->cached_binding
                        ? binding->cached_binding->generation() : 0;
                } else {
                    binding->cached_binding.reset();
                    binding->cached_generation = 0;
                }
            }
            if (binding->cached_generation == 0) return;
            if (state.binding_generation != binding->cached_generation) {
                state = {};
                state.binding_generation = binding->cached_generation;
            }
            if (state.disabled) return;

            ProductionSampleContext production{binding,
                binding->cached_binding.get(), binding->cached_generation};
            const RiverSampleFunction sample = binding->test_mode
                ? binding->test_sample
                : RiverSampleFunction{&production, &production_sample};
            RiverFloatForceBuffer forces{};
            RiverFloatDiagnostics diagnostics{};
            diagnostics.binding_generation = binding->cached_generation;
            bool valid = compute_river_float_forces(
                settings, box, transform, velocity, sample,
                std::fabs(world.get<physics::PhysicsSettings>().gravity.y),
                forces, diagnostics);
            flecs::entity entity = iterator.entity(row);
            if (valid) {
                for (std::uint32_t i = 0; i < forces.count; ++i) {
                    if (!physics::physics_apply_force_at_world_point(
                            entity, forces.rows[i].force_n,
                            forces.rows[i].world_point_m)) {
                        valid = false;
                        break;
                    }
                }
            }
            state.sample_checksum = diagnostics.sample_checksum;
            state.force_checksum = diagnostics.force_checksum;
            if (!valid || diagnostics.hard_invalid) {
                mark_invalid(entity, state);
            } else {
                state.consecutive_invalid = 0;
            }
        });
    system.add<ecs::FixedPipelineSystem>();

    auto measurement_end = world.system<const RiverBindingState>(
            "MatterRiverFloatMeasurementEnd")
        .term_at(0).src<RiverBindingState>()
        .kind<RiverFloatForces>()
        .each([](const RiverBindingState& state) {
            if (state.measurement.end != nullptr)
                state.measurement.end(state.measurement.context);
        });
    measurement_end.add<ecs::FixedPipelineSystem>();
}

void install_test_binding(flecs::world& world, std::uint64_t generation,
                          RiverSampleFunction sample) noexcept {
    RiverBindingState* state = world.try_get_mut<RiverBindingState>();
    if (state == nullptr) return;
    state->context = nullptr;
    state->acquire = nullptr;
    state->test_sample = sample;
    state->test_generation = generation;
    state->cached_tick = std::numeric_limits<std::uint64_t>::max();
    state->cached_generation = 0;
    state->cached_binding.reset();
    state->test_mode = true;
}

void install_runtime_binding(flecs::world& world, const void* context,
                             RiverBindingAcquire acquire) noexcept {
    RiverBindingState* state = world.try_get_mut<RiverBindingState>();
    if (state == nullptr) return;
    state->context = context;
    state->acquire = acquire;
    state->test_sample = {};
    state->test_generation = 0;
    state->cached_tick = std::numeric_limits<std::uint64_t>::max();
    state->cached_generation = 0;
    state->cached_binding.reset();
    state->test_mode = false;
}

void clear_runtime_binding(flecs::world& world) noexcept {
    install_runtime_binding(world, nullptr, nullptr);
}

void install_measurement_hook(flecs::world& world,
                              RiverFloatMeasurementHook hook) noexcept {
    RiverBindingState* state = world.try_get_mut<RiverBindingState>();
    if (state != nullptr) state->measurement = hook;
}

std::uint64_t checksum_transform(const ecs::LocalTransform& transform) noexcept {
    std::uint64_t hash = kHashOffset;
    hash_float3(hash, transform.translation);
    hash_float(hash, transform.rotation.x);
    hash_float(hash, transform.rotation.y);
    hash_float(hash, transform.rotation.z);
    hash_float(hash, transform.rotation.w);
    hash_float3(hash, transform.scale);
    return hash;
}

} // namespace matter::river_float
