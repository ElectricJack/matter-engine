#include "hydrology/physx_runtime.h"

#include "cuda.h"
#include "gpu_fill_sensor.h"
#include "hydrology/fill_sensor.h"
#include "hydrology/fluid_emission.h"
#include "physx_raii.h"

#include "PxPhysicsAPI.h"
#include "cudamanager/PxCudaContext.h"
#include "extensions/PxDefaultCpuDispatcher.h"
#include "extensions/PxRigidActorExt.h"
#include "extensions/PxRigidBodyExt.h"
#include "gpu/PxGpu.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

static_assert(PX_PHYSICS_VERSION == 0x05060100,
              "Matter requires PhysX SDK headers 5.6.1");

class MatterPhysxErrorCallback final : public physx::PxErrorCallback {
public:
    void reportError(physx::PxErrorCode::Enum code, const char* message,
                     const char* file, int line) override {
        if (code == physx::PxErrorCode::eDEBUG_INFO) return;
        if (severity_rank(code) < severity_rank(last_code)) return;
        last_code = code;
        try {
            last_message = "[PhysX ";
            last_message += severity_name(code);
            last_message += "] ";
            last_message +=
                message ? message : "PhysX reported an unknown error";
            if (file) {
                last_message += " (";
                last_message += file;
                last_message += ':';
                last_message += std::to_string(line);
                last_message += ')';
            }
        } catch (...) {
            message_capture_failed = true;
        }
    }

    void clear() noexcept {
        last_code = physx::PxErrorCode::eNO_ERROR;
        last_message.clear();
        message_capture_failed = false;
    }

    hydrology::FluidBakeCode translated_code(
        hydrology::FluidBakeCode fallback) const noexcept {
        return last_code == physx::PxErrorCode::eOUT_OF_MEMORY
                   ? hydrology::FluidBakeCode::CapacityExceeded
                   : fallback;
    }

    std::string diagnostic(const char* fallback) const {
        if (!last_message.empty()) return last_message;
        if (message_capture_failed) {
            return "PhysX error callback could not retain its diagnostic";
        }
        return fallback;
    }

    bool has_issue() const noexcept {
        return last_code != physx::PxErrorCode::eNO_ERROR;
    }

    bool has_failure() const noexcept {
        return severity_rank(last_code) >= 2;
    }

private:
    static int severity_rank(physx::PxErrorCode::Enum code) noexcept {
        switch (code) {
            case physx::PxErrorCode::eABORT:
                return 5;
            case physx::PxErrorCode::eINTERNAL_ERROR:
                return 4;
            case physx::PxErrorCode::eOUT_OF_MEMORY:
                return 3;
            case physx::PxErrorCode::eINVALID_PARAMETER:
            case physx::PxErrorCode::eINVALID_OPERATION:
                return 2;
            case physx::PxErrorCode::eDEBUG_WARNING:
            case physx::PxErrorCode::ePERF_WARNING:
                return 1;
            default:
                return 0;
        }
    }

    static const char* severity_name(physx::PxErrorCode::Enum code) noexcept {
        switch (code) {
            case physx::PxErrorCode::eDEBUG_WARNING:
                return "warning";
            case physx::PxErrorCode::eINVALID_PARAMETER:
                return "invalid parameter";
            case physx::PxErrorCode::eINVALID_OPERATION:
                return "invalid operation";
            case physx::PxErrorCode::eOUT_OF_MEMORY:
                return "out of memory";
            case physx::PxErrorCode::eINTERNAL_ERROR:
                return "internal error";
            case physx::PxErrorCode::eABORT:
                return "abort";
            case physx::PxErrorCode::ePERF_WARNING:
                return "performance warning";
            default:
                return "error";
        }
    }

    physx::PxErrorCode::Enum last_code = physx::PxErrorCode::eNO_ERROR;
    std::string last_message;
    bool message_capture_failed = false;
};

class MatterGpuLoadHook final : public PxGpuLoadHook {
public:
    explicit MatterGpuLoadHook(std::string runtime_path)
        : path(std::move(runtime_path)) {}

    const char* getPhysXGpuDllName() const override {
        return path.c_str();
    }

    std::string path;
};

std::mutex g_gpu_load_mutex;
MatterGpuLoadHook g_default_gpu_load_hook("PhysXGpu_64.dll");

class RestoreDefaultGpuLoadHook final {
public:
    ~RestoreDefaultGpuLoadHook() {
        PxSetPhysXGpuLoadHook(&g_default_gpu_load_hook);
    }
};

hydrology::FluidBackendProbe probe_failure(
    hydrology::FluidBakeCode code, const std::string& message) {
    hydrology::FluidBackendProbe result{};
    result.backend_name = "NVIDIA PhysX PBD";
    result.sdk_version = "5.6.1";
    result.sdk_version_hex = PX_PHYSICS_VERSION;
    result.code = code;
    result.message = message;
    return result;
}

bool finite(matter::Float3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool valid_bounds(const matter::Aabb& bounds) {
    return finite(bounds.minimum) && finite(bounds.maximum) &&
           bounds.minimum.x <= bounds.maximum.x &&
           bounds.minimum.y <= bounds.maximum.y &&
           bounds.minimum.z <= bounds.maximum.z;
}

bool valid_strict_bounds(const matter::Aabb& bounds) {
    return finite(bounds.minimum) && finite(bounds.maximum) &&
           bounds.minimum.x < bounds.maximum.x &&
           bounds.minimum.y < bounds.maximum.y &&
           bounds.minimum.z < bounds.maximum.z;
}

bool inside(const matter::Aabb& bounds, matter::Float3 point) {
    return point.x >= bounds.minimum.x && point.x <= bounds.maximum.x &&
           point.y >= bounds.minimum.y && point.y <= bounds.maximum.y &&
           point.z >= bounds.minimum.z && point.z <= bounds.maximum.z;
}

matter::Float3 from_px(physx::PxVec3 value) {
    return {value.x, value.y, value.z};
}

physx::PxVec3 to_px(matter::Float3 value) {
    return {value.x, value.y, value.z};
}

hydrology::FluidBakeCode cuda_failure_code(
    std::uint32_t cuda_result,
    const physx::PxCudaContextManager& cuda) {
    if (!cuda.contextIsValid()) return hydrology::FluidBakeCode::DeviceLost;
    switch (static_cast<CUresult>(cuda_result)) {
        case CUDA_ERROR_OUT_OF_MEMORY:
            return hydrology::FluidBakeCode::CapacityExceeded;
        case CUDA_ERROR_DEINITIALIZED:
        case CUDA_ERROR_DEVICE_UNAVAILABLE:
        case CUDA_ERROR_NO_DEVICE:
        case CUDA_ERROR_INVALID_CONTEXT:
        case CUDA_ERROR_ECC_UNCORRECTABLE:
        case CUDA_ERROR_ILLEGAL_ADDRESS:
        case CUDA_ERROR_LAUNCH_TIMEOUT:
        case CUDA_ERROR_CONTEXT_IS_DESTROYED:
        case CUDA_ERROR_ASSERT:
        case CUDA_ERROR_HARDWARE_STACK_ERROR:
        case CUDA_ERROR_LAUNCH_FAILED:
        case CUDA_ERROR_UNKNOWN:
            return hydrology::FluidBakeCode::DeviceLost;
        default:
            return hydrology::FluidBakeCode::BackendFailure;
    }
}

matter::Float3 add(matter::Float3 left, matter::Float3 right) {
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

matter::Float3 scale(matter::Float3 value, float factor) {
    return {value.x * factor, value.y * factor, value.z * factor};
}

matter::Float3 cross(matter::Float3 left, matter::Float3 right) {
    return {left.y * right.z - left.z * right.y,
            left.z * right.x - left.x * right.z,
            left.x * right.y - left.y * right.x};
}

matter::Float3 normalized(matter::Float3 value) {
    const float length = std::sqrt(value.x * value.x + value.y * value.y +
                                   value.z * value.z);
    return scale(value, 1.0f / length);
}

struct EmitterPlacement {
    matter::Float3 position_m{};
    matter::Float3 direction{};
    std::vector<matter::Float3> disk_offsets;
};

EmitterPlacement make_emitter_placement(
    const hydrology::FluidEmitter& emitter,
    float particle_spacing_m,
    std::uint32_t maximum_offsets) {
    EmitterPlacement placement{};
    placement.position_m = emitter.position_m;
    placement.direction = normalized(emitter.direction);
    const matter::Float3 reference =
        std::fabs(placement.direction.y) < 0.9f
            ? matter::Float3{0.0f, 1.0f, 0.0f}
            : matter::Float3{1.0f, 0.0f, 0.0f};
    const matter::Float3 tangent =
        normalized(cross(placement.direction, reference));
    const matter::Float3 bitangent = cross(placement.direction, tangent);

    const double radius_cells = std::floor(
        static_cast<double>(emitter.radius_m) /
        static_cast<double>(particle_spacing_m));
    const double ring_cap = (std::min)(
        static_cast<double>(maximum_offsets),
        static_cast<double>((std::numeric_limits<int>::max)()));
    const auto maximum_ring = static_cast<std::uint32_t>((std::min)(
        radius_cells, ring_cap));
    for (std::uint32_t ring = 0u;
         ring <= maximum_ring &&
         placement.disk_offsets.size() < maximum_offsets;
         ++ring) {
        const int signed_ring = static_cast<int>(ring);
        for (int z = -signed_ring;
             z <= signed_ring &&
             placement.disk_offsets.size() < maximum_offsets; ++z) {
            for (int x = -signed_ring;
                 x <= signed_ring &&
                 placement.disk_offsets.size() < maximum_offsets; ++x) {
                if ((std::max)(std::abs(x), std::abs(z)) != signed_ring) {
                    continue;
                }
                const float offset_x =
                    static_cast<float>(x) * particle_spacing_m;
                const float offset_z =
                    static_cast<float>(z) * particle_spacing_m;
                if (offset_x * offset_x + offset_z * offset_z <=
                    emitter.radius_m * emitter.radius_m + 1.0e-6f) {
                    placement.disk_offsets.push_back(
                        add(scale(tangent, offset_x),
                            scale(bitangent, offset_z)));
                }
            }
        }
    }
    if (placement.disk_offsets.empty()) placement.disk_offsets.push_back({});
    return placement;
}

matter::Float3 emitter_activation_position(
    const EmitterPlacement& placement,
    std::uint64_t emitted_sequence,
    std::uint32_t ordinal_this_step,
    float particle_spacing_m) {
    const std::size_t slot = static_cast<std::size_t>(
        emitted_sequence % placement.disk_offsets.size());
    const std::uint32_t layer = ordinal_this_step /
        static_cast<std::uint32_t>(placement.disk_offsets.size());
    return add(add(placement.position_m, placement.disk_offsets[slot]),
               scale(placement.direction,
                     -static_cast<float>(layer) * particle_spacing_m));
}

class ParticleBufferAttachment final {
public:
    ParticleBufferAttachment(physx::PxPBDParticleSystem& system,
                             physx::PxParticleBuffer& buffer)
        : system_(&system), buffer_(&buffer) {
        system_->addParticleBuffer(buffer_);
    }

    ~ParticleBufferAttachment() {
        if (system_ && buffer_) system_->removeParticleBuffer(buffer_);
    }

    ParticleBufferAttachment(const ParticleBufferAttachment&) = delete;
    ParticleBufferAttachment& operator=(const ParticleBufferAttachment&) =
        delete;

private:
    physx::PxPBDParticleSystem* system_ = nullptr;
    physx::PxParticleBuffer* buffer_ = nullptr;
};

physx::PxFilterFlags probe_filter_shader(
    physx::PxFilterObjectAttributes attributes0,
    physx::PxFilterData filter_data0,
    physx::PxFilterObjectAttributes attributes1,
    physx::PxFilterData filter_data1,
    physx::PxPairFlags& pair_flags,
    const void* constant_block,
    physx::PxU32 constant_block_size) {
    (void)attributes0;
    (void)filter_data0;
    (void)attributes1;
    (void)filter_data1;
    (void)constant_block;
    (void)constant_block_size;
    pair_flags = physx::PxPairFlag::eCONTACT_DEFAULT |
                 physx::PxPairFlag::eNOTIFY_TOUCH_FOUND |
                 physx::PxPairFlag::eNOTIFY_TOUCH_PERSISTS;
    return physx::PxFilterFlag::eDEFAULT;
}

class ProbeContactCallback final : public physx::PxSimulationEventCallback {
public:
    void onConstraintBreak(physx::PxConstraintInfo*, physx::PxU32) override {}
    void onWake(physx::PxActor**, physx::PxU32) override {}
    void onSleep(physx::PxActor**, physx::PxU32) override {}
    void onTrigger(physx::PxTriggerPair*, physx::PxU32) override {}
    void onAdvance(const physx::PxRigidBody* const*, const physx::PxTransform*,
                   const physx::PxU32) override {}

    void onContact(const physx::PxContactPairHeader&,
                   const physx::PxContactPair* pairs,
                   physx::PxU32 pair_count) override {
        for (physx::PxU32 index = 0; index < pair_count; ++index) {
            if (pairs[index].events &
                (physx::PxPairFlag::eNOTIFY_TOUCH_FOUND |
                 physx::PxPairFlag::eNOTIFY_TOUCH_PERSISTS)) {
                ++contact_events;
            }
        }
    }

    std::uint32_t contact_events = 0;
};

bool validate_probe_input(const hydrology::FluidCollisionProbeInput& input,
                          hydrology::FluidBakeError& error) {
    if (input.collision.vertices.empty() ||
        input.collision.indices.empty() ||
        input.collision.indices.size() % 3u != 0u ||
        input.initial_positions_m.empty()) {
        error = {hydrology::FluidBakeCode::InvalidInput,
                 "PhysX collision probe requires triangles and start positions"};
        return false;
    }
    if (!valid_bounds(input.dry_collar_bounds_m) ||
        !finite(input.gravity_mps2) ||
        !std::isfinite(input.probe_radius_m) ||
        !(input.probe_radius_m > 0.0f) ||
        !std::isfinite(input.fixed_step_seconds) ||
        !(input.fixed_step_seconds > 0.0f) || input.max_steps == 0u) {
        error = {hydrology::FluidBakeCode::InvalidInput,
                 "PhysX collision probe settings are invalid"};
        return false;
    }
    for (matter::Float3 vertex : input.collision.vertices) {
        if (!finite(vertex)) {
            error = {hydrology::FluidBakeCode::InvalidInput,
                     "PhysX collision probe mesh contains a non-finite vertex"};
            return false;
        }
    }
    for (std::uint32_t index : input.collision.indices) {
        if (index >= input.collision.vertices.size()) {
            error = {hydrology::FluidBakeCode::InvalidInput,
                     "PhysX collision probe mesh contains an out-of-range index"};
            return false;
        }
    }
    for (matter::Float3 position : input.initial_positions_m) {
        if (!finite(position) || !inside(input.dry_collar_bounds_m, position)) {
            error = {hydrology::FluidBakeCode::InvalidInput,
                     "PhysX collision probe start is non-finite or outside the dry collar"};
            return false;
        }
    }
    return true;
}

bool validate_pbd_run_input(const hydrology::FluidBakeInput& input,
                            hydrology::FluidBakeError& error) {
    const auto& settings = input.settings;
    if (input.collision.vertices.empty() ||
        input.collision.indices.empty() ||
        input.collision.indices.size() % 3u != 0u ||
        input.emitters.empty() ||
        !std::isfinite(settings.particle_spacing_m) ||
        !(settings.particle_spacing_m > 0.0f) ||
        !std::isfinite(settings.rest_density_kg_m3) ||
        !(settings.rest_density_kg_m3 > 0.0f) ||
        !std::isfinite(settings.fixed_step_seconds) ||
        !(settings.fixed_step_seconds > 0.0f) ||
        settings.solver_iterations == 0u || settings.max_neighbors == 0u ||
        settings.batch_steps == 0u || settings.max_steps == 0u ||
        settings.batch_steps > settings.max_steps ||
        settings.max_particles == 0u ||
        !valid_strict_bounds(input.sensor.bounds_m) ||
        input.sensor.resolution.x == 0u ||
        input.sensor.resolution.y == 0u ||
        input.sensor.resolution.z == 0u ||
        !std::isfinite(input.sensor.required_wet_fraction) ||
        !(input.sensor.required_wet_fraction > 0.0f) ||
        input.sensor.required_wet_fraction > 1.0f ||
        input.sensor.stable_steps == 0u ||
        input.sensor.minimum_particles_per_cell == 0u ||
        !valid_strict_bounds(input.dry_collar_bounds_m)) {
        error = {hydrology::FluidBakeCode::InvalidInput,
                 "PhysX PBD run input is incomplete or invalid"};
        return false;
    }
    for (matter::Float3 vertex : input.collision.vertices) {
        if (!finite(vertex)) {
            error = {hydrology::FluidBakeCode::InvalidInput,
                     "PhysX PBD collision contains a non-finite vertex"};
            return false;
        }
    }
    for (std::uint32_t index : input.collision.indices) {
        if (index >= input.collision.vertices.size()) {
            error = {hydrology::FluidBakeCode::InvalidInput,
                     "PhysX PBD collision contains an out-of-range index"};
            return false;
        }
    }
    std::vector<std::uint32_t> emitter_ids;
    emitter_ids.reserve(input.emitters.size());
    for (const hydrology::FluidEmitter& emitter : input.emitters) {
        const float direction_length_squared =
            emitter.direction.x * emitter.direction.x +
            emitter.direction.y * emitter.direction.y +
            emitter.direction.z * emitter.direction.z;
        if (!finite(emitter.position_m) || !finite(emitter.direction) ||
            !finite(emitter.initial_velocity_mps) ||
            !std::isfinite(direction_length_squared) ||
            !(direction_length_squared > 0.0f) ||
            !std::isfinite(emitter.flow_m3s) ||
            !(emitter.flow_m3s > 0.0f) ||
            !std::isfinite(emitter.radius_m) ||
            !(emitter.radius_m > 0.0f) ||
            emitter.start_step >= emitter.stop_step ||
            emitter.stop_step > settings.max_steps ||
            std::find(emitter_ids.begin(), emitter_ids.end(), emitter.id) !=
                emitter_ids.end()) {
            error = {hydrology::FluidBakeCode::InvalidInput,
                     "PhysX PBD emitter is invalid"};
            return false;
        }
        emitter_ids.push_back(emitter.id);
    }
    return true;
}

bool preflight_emission_capacity(const hydrology::FluidBakeInput& input,
                                 hydrology::FluidBakeError& error) {
    hydrology::FluidEmissionState state{};
    std::vector<hydrology::FluidParticleActivation> activations;
    std::uint32_t active_particles = 0u;
    std::uint32_t last_emission_step = 0u;
    for (const hydrology::FluidEmitter& emitter : input.emitters) {
        last_emission_step = (std::max)(last_emission_step,
                                        emitter.stop_step);
    }
    for (std::uint32_t step = 0u; step < last_emission_step; ++step) {
        if (!hydrology::schedule_fluid_emission_step(
                input.emitters, input.settings, step, active_particles, state,
                activations, error)) {
            return false;
        }
        active_particles +=
            static_cast<std::uint32_t>(activations.size());
    }
    return true;
}

}  // namespace

namespace hydrology {

struct PhysxRuntime::Impl {
    explicit Impl(PhysxRuntimeOptions runtime_options)
        : options(std::move(runtime_options)) {}

    ~Impl() { release(); }

    void release() noexcept {
        physics.reset();
        cuda.reset();
        foundation.reset();
    }

    FluidBackendProbe fail(FluidBakeCode code, const std::string& message) {
        attempted = true;
        cached_probe = probe_failure(code, message);
        release();
        return cached_probe;
    }

    FluidBackendProbe probe() {
        if (attempted) return cached_probe;
        attempted = true;
        errors.clear();

        if (!options.gpu_runtime_path.empty()) {
            std::error_code filesystem_error;
            const bool is_file = std::filesystem::is_regular_file(
                options.gpu_runtime_path, filesystem_error);
            if (!is_file || filesystem_error) {
                return fail(
                    FluidBakeCode::BackendUnavailable,
                    "PhysX GPU runtime path is missing or unreadable: " +
                        options.gpu_runtime_path);
            }
        }
        if (options.initialization_hook) options.initialization_hook();

        foundation.reset(PxCreateFoundation(
            PX_PHYSICS_VERSION, allocator, errors));
        if (!foundation) {
            return fail(errors.translated_code(FluidBakeCode::BackendFailure),
                        errors.diagnostic(
                            "PhysX foundation creation failed"));
        }
        physics.reset(PxCreatePhysics(PX_PHYSICS_VERSION, *foundation,
                                      physx::PxTolerancesScale(), false,
                                      nullptr));
        if (!physics) {
            return fail(
                errors.translated_code(FluidBakeCode::BackendUnavailable),
                errors.diagnostic(
                    "PhysX runtime/header version check failed"));
        }

        physx::PxCudaContextManagerDesc cuda_desc;
        cuda_desc.deviceOrdinal = options.device_ordinal;
        {
            std::lock_guard<std::mutex> lock(g_gpu_load_mutex);
            if (!options.gpu_runtime_path.empty()) {
                MatterGpuLoadHook custom_hook(options.gpu_runtime_path);
                PxSetPhysXGpuLoadHook(&custom_hook);
                const RestoreDefaultGpuLoadHook restore_default;
                cuda.reset(PxCreateCudaContextManager(
                    *foundation, cuda_desc, PxGetProfilerCallback(), false));
            } else {
                PxSetPhysXGpuLoadHook(&g_default_gpu_load_hook);
                cuda.reset(PxCreateCudaContextManager(
                    *foundation, cuda_desc, PxGetProfilerCallback(), false));
            }
        }
        if (!cuda || !cuda->contextIsValid()) {
            return fail(
                errors.translated_code(FluidBakeCode::BackendUnavailable),
                errors.diagnostic(
                    "PhysX GPU runtime or CUDA context is unavailable"));
        }

        cached_probe.available = true;
        cached_probe.backend_name = "NVIDIA PhysX PBD";
        cached_probe.sdk_version = "5.6.1";
        cached_probe.device_name = cuda->getDeviceName()
                                       ? cuda->getDeviceName()
                                       : "unknown CUDA device";
        cached_probe.code = FluidBakeCode::Ready;
        cached_probe.sdk_version_hex = PX_PHYSICS_VERSION;
        cached_probe.cuda_context_valid = true;
        cached_probe.device_ordinal = static_cast<int>(cuda->getDevice());
        cached_probe.cuda_driver_version = cuda->getDriverVersion();
        cached_probe.device_total_memory_bytes =
            static_cast<std::uint64_t>(cuda->getDeviceTotalMemBytes());
        return cached_probe;
    }

    bool run_collision_probe(const FluidCollisionProbeInput& input,
                             FluidCollisionProbeOutput& output,
                             FluidBakeError& error) {
        output = {};
        error = {};
        if (!validate_probe_input(input, error)) return false;
        const FluidBackendProbe state = probe();
        if (!state.available) {
            error = {state.code, state.message};
            return false;
        }

        matter_physx::PxOwner<physx::PxDefaultCpuDispatcher> dispatcher(
            physx::PxDefaultCpuDispatcherCreate(2u));
        if (!dispatcher) {
            error = {FluidBakeCode::BackendFailure,
                     "PhysX collision probe CPU dispatcher creation failed"};
            return false;
        }

        ProbeContactCallback contact_callback;
        physx::PxSceneDesc scene_desc(physics->getTolerancesScale());
        scene_desc.gravity = to_px(input.gravity_mps2);
        scene_desc.cpuDispatcher = dispatcher.get();
        scene_desc.filterShader = probe_filter_shader;
        scene_desc.simulationEventCallback = &contact_callback;
        scene_desc.cudaContextManager = cuda.get();
        scene_desc.staticStructure = physx::PxPruningStructureType::eDYNAMIC_AABB_TREE;
        scene_desc.flags |= physx::PxSceneFlag::eENABLE_PCM;
        scene_desc.flags |= physx::PxSceneFlag::eENABLE_GPU_DYNAMICS;
        scene_desc.broadPhaseType = physx::PxBroadPhaseType::eGPU;
        scene_desc.solverType = physx::PxSolverType::eTGS;
        if (!scene_desc.isValid()) {
            error = {FluidBakeCode::BackendFailure,
                     "PhysX GPU collision scene description is invalid"};
            return false;
        }
        matter_physx::PxOwner<physx::PxScene> scene(
            physics->createScene(scene_desc));
        if (!scene) {
            error = {errors.translated_code(FluidBakeCode::BackendFailure),
                     errors.diagnostic(
                         "PhysX GPU collision scene creation failed")};
            return false;
        }

        std::vector<physx::PxVec3> vertices;
        vertices.reserve(input.collision.vertices.size());
        for (matter::Float3 vertex : input.collision.vertices) {
            vertices.push_back(to_px(vertex));
        }
        physx::PxTriangleMeshDesc mesh_desc;
        mesh_desc.points.count = static_cast<physx::PxU32>(vertices.size());
        mesh_desc.points.stride = sizeof(physx::PxVec3);
        mesh_desc.points.data = vertices.data();
        mesh_desc.triangles.count = static_cast<physx::PxU32>(
            input.collision.indices.size() / 3u);
        mesh_desc.triangles.stride = 3u * sizeof(std::uint32_t);
        mesh_desc.triangles.data = input.collision.indices.data();

        physx::PxCookingParams cooking_params(physics->getTolerancesScale());
        cooking_params.meshEdgeLengthMaxLimit = 0.0f;
        errors.clear();
        physx::PxTriangleMeshCookingResult::Enum cooking_result =
            physx::PxTriangleMeshCookingResult::eFAILURE;
        matter_physx::PxOwner<physx::PxTriangleMesh> triangle_mesh(
            PxCreateTriangleMesh(cooking_params, mesh_desc,
                                 physics->getPhysicsInsertionCallback(),
                                 &cooking_result));
        if (!triangle_mesh ||
            cooking_result != physx::PxTriangleMeshCookingResult::eSUCCESS ||
            errors.has_issue()) {
            error = {
                errors.translated_code(FluidBakeCode::BackendFailure),
                errors.diagnostic(
                    "PhysX triangle collision cooking failed or warned")};
            return false;
        }

        matter_physx::PxOwner<physx::PxMaterial> material(
            physics->createMaterial(0.35f, 0.35f, 0.0f));
        matter_physx::PxOwner<physx::PxRigidStatic> terrain(
            physics->createRigidStatic(physx::PxTransform(physx::PxIdentity)));
        if (!material || !terrain ||
            !physx::PxRigidActorExt::createExclusiveShape(
                *terrain, physx::PxTriangleMeshGeometry(triangle_mesh.get()),
                *material)) {
            error = {FluidBakeCode::BackendFailure,
                     "PhysX triangle collision actor creation failed"};
            return false;
        }
        scene->addActor(*terrain);

        std::vector<matter_physx::PxOwner<physx::PxRigidDynamic>> probes;
        probes.reserve(input.initial_positions_m.size());
        for (matter::Float3 start : input.initial_positions_m) {
            matter_physx::PxOwner<physx::PxRigidDynamic> actor(
                physics->createRigidDynamic(physx::PxTransform(to_px(start))));
            if (!actor || !physx::PxRigidActorExt::createExclusiveShape(
                              *actor,
                              physx::PxSphereGeometry(input.probe_radius_m),
                              *material) ||
                !physx::PxRigidBodyExt::setMassAndUpdateInertia(*actor, 1.0f)) {
                error = {FluidBakeCode::BackendFailure,
                         "PhysX collision probe body creation failed"};
                return false;
            }
            actor->setLinearDamping(0.05f);
            actor->setAngularDamping(0.05f);
            scene->addActor(*actor);
            probes.push_back(std::move(actor));
        }

        output.final_positions_m.resize(probes.size());
        output.final_velocities_mps.resize(probes.size());
        std::vector<bool> escaped(probes.size(), false);
        bool found_non_finite = false;
        for (std::uint32_t step = 0; step < input.max_steps; ++step) {
            scene->simulate(input.fixed_step_seconds);
            if (!scene->fetchResults(true)) {
                error = {FluidBakeCode::BackendFailure,
                         "PhysX collision probe fetchResults failed"};
                return false;
            }
            output.simulated_steps = step + 1u;
            for (std::size_t probe_index = 0;
                 probe_index < probes.size(); ++probe_index) {
                const matter::Float3 position = from_px(
                    probes[probe_index]->getGlobalPose().p);
                const matter::Float3 velocity = from_px(
                    probes[probe_index]->getLinearVelocity());
                output.final_positions_m[probe_index] = position;
                output.final_velocities_mps[probe_index] = velocity;
                if (!finite(position) || !finite(velocity)) {
                    found_non_finite = true;
                } else if (!escaped[probe_index] &&
                           !inside(input.dry_collar_bounds_m, position)) {
                    escaped[probe_index] = true;
                    ++output.escaped_probes;
                }
            }
            if (found_non_finite || output.escaped_probes != 0u) break;
        }
        output.contact_events = contact_callback.contact_events;
        if (found_non_finite) {
            error = {FluidBakeCode::NonFinite,
                     "PhysX collision probe produced non-finite state"};
            return false;
        }
        if (output.escaped_probes != 0u) {
            error = {FluidBakeCode::Escaped,
                     "PhysX collision probe crossed the dry collar"};
            return false;
        }
        return true;
    }

    bool run(const FluidBakeInput& input,
             const FluidBakeCallbacks& callbacks,
             FluidBakeOutput& output,
             FluidBakeError& error) {
        output = {};
        error = {};
        const auto wall_start = std::chrono::steady_clock::now();
        auto fail_run = [&](FluidBakeCode code,
                            const std::string& message) -> bool {
            output.stats.wall_seconds =
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - wall_start)
                    .count();
            error = {code, message};
            return false;
        };
        auto notify = [&](PhysxRuntimeEvent event, std::uint32_t step,
                          std::uint32_t value) {
            if (options.execution_hook) {
                options.execution_hook(event, step, value,
                                       options.execution_hook_user_data);
            }
        };

        const FluidBackendProbe state = probe();
        if (!state.available) return fail_run(state.code, state.message);
        FluidBakeError validation_error{};
        if (!validate_pbd_run_input(input, validation_error)) {
            return fail_run(validation_error.code, validation_error.message);
        }
        if (!preflight_emission_capacity(input, validation_error)) {
            return fail_run(validation_error.code, validation_error.message);
        }

        matter_physx::PxOwner<physx::PxDefaultCpuDispatcher> dispatcher(
            physx::PxDefaultCpuDispatcherCreate(2u));
        if (!dispatcher) {
            return fail_run(FluidBakeCode::BackendFailure,
                            "PhysX PBD CPU dispatcher creation failed");
        }
        physx::PxSceneDesc scene_desc(physics->getTolerancesScale());
        scene_desc.gravity = physx::PxVec3(0.0f, -9.81f, 0.0f);
        scene_desc.cpuDispatcher = dispatcher.get();
        scene_desc.filterShader = physx::PxDefaultSimulationFilterShader;
        scene_desc.cudaContextManager = cuda.get();
        scene_desc.staticStructure =
            physx::PxPruningStructureType::eDYNAMIC_AABB_TREE;
        scene_desc.flags |= physx::PxSceneFlag::eENABLE_PCM;
        scene_desc.flags |= physx::PxSceneFlag::eENABLE_GPU_DYNAMICS;
        scene_desc.broadPhaseType = physx::PxBroadPhaseType::eGPU;
        scene_desc.solverType = physx::PxSolverType::eTGS;
        if (!scene_desc.isValid()) {
            return fail_run(FluidBakeCode::BackendFailure,
                            "PhysX PBD scene description is invalid");
        }
        matter_physx::PxOwner<physx::PxScene> scene(
            physics->createScene(scene_desc));
        if (!scene) {
            return fail_run(
                errors.translated_code(FluidBakeCode::BackendFailure),
                errors.diagnostic("PhysX PBD scene creation failed"));
        }

        std::vector<physx::PxVec3> vertices;
        vertices.reserve(input.collision.vertices.size());
        for (matter::Float3 vertex : input.collision.vertices) {
            vertices.push_back(to_px(vertex));
        }
        physx::PxTriangleMeshDesc mesh_desc;
        mesh_desc.points.count = static_cast<physx::PxU32>(vertices.size());
        mesh_desc.points.stride = sizeof(physx::PxVec3);
        mesh_desc.points.data = vertices.data();
        mesh_desc.triangles.count = static_cast<physx::PxU32>(
            input.collision.indices.size() / 3u);
        mesh_desc.triangles.stride = 3u * sizeof(std::uint32_t);
        mesh_desc.triangles.data = input.collision.indices.data();
        physx::PxCookingParams cooking_params(physics->getTolerancesScale());
        cooking_params.meshEdgeLengthMaxLimit = 0.0f;
        cooking_params.buildGPUData = true;
        errors.clear();
        physx::PxTriangleMeshCookingResult::Enum cooking_result =
            physx::PxTriangleMeshCookingResult::eFAILURE;
        matter_physx::PxOwner<physx::PxTriangleMesh> triangle_mesh(
            PxCreateTriangleMesh(cooking_params, mesh_desc,
                                 physics->getPhysicsInsertionCallback(),
                                 &cooking_result));
        if (!triangle_mesh ||
            cooking_result != physx::PxTriangleMeshCookingResult::eSUCCESS ||
            errors.has_issue()) {
            return fail_run(
                errors.translated_code(FluidBakeCode::BackendFailure),
                errors.diagnostic("PhysX PBD collision cooking failed"));
        }
        matter_physx::PxOwner<physx::PxMaterial> collision_material(
            physics->createMaterial(0.35f, 0.35f, 0.0f));
        matter_physx::PxOwner<physx::PxRigidStatic> terrain(
            physics->createRigidStatic(physx::PxTransform(physx::PxIdentity)));
        if (!collision_material || !terrain ||
            !physx::PxRigidActorExt::createExclusiveShape(
                *terrain, physx::PxTriangleMeshGeometry(triangle_mesh.get()),
                *collision_material)) {
            return fail_run(FluidBakeCode::BackendFailure,
                            "PhysX PBD collision actor creation failed");
        }
        scene->addActor(*terrain);

        const float spacing = input.settings.particle_spacing_m;
        const float rest_offset = 0.5f * spacing / 0.6f;
        const float solid_rest_offset = rest_offset;
        const float fluid_rest_offset = rest_offset * 0.6f;
        const float particle_mass = input.settings.rest_density_kg_m3 *
            physx_particle_volume_m3(spacing);
        matter_physx::PxOwner<physx::PxPBDMaterial> pbd_material(
            physics->createPBDMaterial(0.05f, 0.05f, 0.0f, 0.001f, 0.5f,
                                       0.005f, 0.01f, 0.0f, 0.0f));
        matter_physx::PxOwner<physx::PxPBDParticleSystem> particle_system(
            physics->createPBDParticleSystem(*cuda,
                                             input.settings.max_neighbors));
        if (!pbd_material || !particle_system ||
            !std::isfinite(particle_mass) || !(particle_mass > 0.0f)) {
            return fail_run(FluidBakeCode::BackendFailure,
                            "PhysX PBD material or system creation failed");
        }
        pbd_material->setViscosity(0.001f);
        pbd_material->setSurfaceTension(0.00704f);
        pbd_material->setCohesion(0.0704f);
        pbd_material->setVorticityConfinement(10.0f);
        particle_system->setRestOffset(rest_offset);
        particle_system->setContactOffset(rest_offset + 0.01f);
        particle_system->setParticleContactOffset(fluid_rest_offset / 0.6f);
        particle_system->setSolidRestOffset(solid_rest_offset);
        particle_system->setFluidRestOffset(fluid_rest_offset);
        particle_system->setParticleFlag(
            physx::PxParticleFlag::eENABLE_SPECULATIVE_CCD, false);
        particle_system->setMaxVelocity(solid_rest_offset * 100.0f);
        particle_system->setSolverIterationCounts(
            input.settings.solver_iterations, 1u);
        const physx::PxU32 phase = particle_system->createPhase(
            pbd_material.get(),
            physx::PxParticlePhaseFlags(
                physx::PxParticlePhaseFlag::eParticlePhaseFluid |
                physx::PxParticlePhaseFlag::eParticlePhaseSelfCollide));
        if (phase == PX_INVALID_U32) {
            return fail_run(FluidBakeCode::BackendFailure,
                            "PhysX PBD fluid phase creation failed");
        }
        scene->addActor(*particle_system);

        matter_physx::PxOwner<physx::PxParticleBuffer> particle_buffer(
            physics->createParticleBuffer(input.settings.max_particles, 0u,
                                           cuda.get()));
        if (!particle_buffer) {
            return fail_run(FluidBakeCode::CapacityExceeded,
                            "PhysX PBD particle buffer allocation failed");
        }
        particle_buffer->setNbActiveParticles(0u);
        ParticleBufferAttachment attachment(*particle_system,
                                            *particle_buffer);

        const std::uint64_t horizontal_cells_64 =
            static_cast<std::uint64_t>(input.sensor.resolution.x) *
            static_cast<std::uint64_t>(input.sensor.resolution.z);
        if (horizontal_cells_64 == 0u ||
            horizontal_cells_64 >
                std::numeric_limits<std::uint32_t>::max()) {
            return fail_run(FluidBakeCode::InvalidInput,
                            "PhysX PBD fill sensor resolution is invalid");
        }
        matter_physx::GpuFillSensor gpu_sensor;
        std::string gpu_sensor_error;
        if (!gpu_sensor.initialize(
                *cuda, static_cast<std::uint32_t>(horizontal_cells_64),
                input.settings.batch_steps,
                gpu_sensor_error)) {
            return fail_run(cuda_failure_code(
                                gpu_sensor.last_cuda_error(), *cuda),
                            gpu_sensor_error);
        }

        FluidEmissionState emission_state{};
        FillSensorState fill_state{};
        std::vector<FluidParticleActivation> activations;
        std::vector<std::uint64_t> particle_ids;
        particle_ids.reserve(input.settings.max_particles);
        std::vector<std::uint64_t> emitted_per_emitter(
            input.emitters.size(), 0u);
        std::vector<EmitterPlacement> emitter_placements;
        emitter_placements.reserve(input.emitters.size());
        for (const FluidEmitter& emitter : input.emitters) {
            emitter_placements.push_back(
                make_emitter_placement(
                    emitter, spacing, input.settings.max_particles));
        }
        std::uint32_t active_particles = 0u;
        bool completed = false;

        std::uint32_t batch_start = 0u;
        while (batch_start < input.settings.max_steps && !completed) {
            if (callbacks.cancelled && callbacks.cancelled()) {
                return fail_run(FluidBakeCode::Cancelled,
                                "PhysX PBD bake was cancelled between batches");
            }
            const std::uint32_t batch_end = batch_start + std::min(
                input.settings.batch_steps,
                input.settings.max_steps - batch_start);
            gpu_sensor.begin_batch();
            for (std::uint32_t zero_step = batch_start;
                 zero_step < batch_end; ++zero_step) {
                FluidBakeError schedule_error{};
                if (!schedule_fluid_emission_step(
                        input.emitters, input.settings, zero_step,
                        active_particles, emission_state, activations,
                        schedule_error)) {
                    return fail_run(schedule_error.code,
                                    schedule_error.message);
                }

                std::vector<physx::PxVec4> positions;
                std::vector<physx::PxVec4> velocities;
                std::vector<physx::PxU32> phases;
                positions.reserve(activations.size());
                velocities.reserve(activations.size());
                phases.reserve(activations.size());
                std::vector<std::uint32_t> ordinals(input.emitters.size(), 0u);
                for (const FluidParticleActivation& activation : activations) {
                    std::size_t emitter_index = input.emitters.size();
                    for (std::size_t index = 0u;
                         index < input.emitters.size(); ++index) {
                        if (input.emitters[index].id == activation.emitter_id) {
                            emitter_index = index;
                            break;
                        }
                    }
                    if (emitter_index == input.emitters.size()) {
                        return fail_run(FluidBakeCode::BackendFailure,
                                        "PhysX PBD activation references an unknown emitter");
                    }
                    const matter::Float3 position = emitter_activation_position(
                        emitter_placements[emitter_index],
                        emitted_per_emitter[emitter_index]++,
                        ordinals[emitter_index]++, spacing);
                    positions.emplace_back(position.x, position.y, position.z,
                                           1.0f / particle_mass);
                    const matter::Float3 velocity = activation.velocity_mps;
                    velocities.emplace_back(velocity.x, velocity.y, velocity.z,
                                            0.0f);
                    phases.push_back(phase);
                    particle_ids.push_back(activation.id);
                }

                if (!activations.empty()) {
                    physx::PxCUresult upload_result = CUDA_SUCCESS;
                    {
                        physx::PxScopedCudaLock cuda_lock(*cuda);
                        physx::PxCudaContext* context = cuda->getCudaContext();
                        upload_result = context->memcpyHtoD(
                            reinterpret_cast<CUdeviceptr>(
                                particle_buffer->getPositionInvMasses() +
                                active_particles),
                            positions.data(),
                            positions.size() * sizeof(physx::PxVec4));
                        if (upload_result == CUDA_SUCCESS) {
                            upload_result = context->memcpyHtoD(
                                reinterpret_cast<CUdeviceptr>(
                                    particle_buffer->getVelocities() +
                                    active_particles),
                                velocities.data(),
                                velocities.size() * sizeof(physx::PxVec4));
                        }
                        if (upload_result == CUDA_SUCCESS) {
                            upload_result = context->memcpyHtoD(
                                reinterpret_cast<CUdeviceptr>(
                                    particle_buffer->getPhases() +
                                    active_particles),
                                phases.data(),
                                phases.size() * sizeof(physx::PxU32));
                        }
                    }
                    if (options.cuda_error_injection_hook) {
                        const std::uint32_t injected =
                            options.cuda_error_injection_hook(
                                zero_step + 1u,
                                options.cuda_error_injection_user_data);
                        if (injected != 0u) upload_result.value = injected;
                    }
                    if (upload_result != CUDA_SUCCESS) {
                        return fail_run(
                            cuda_failure_code(upload_result.value, *cuda),
                            "CUDA particle activation upload failed with code " +
                                std::to_string(upload_result.value));
                    }
                    active_particles +=
                        static_cast<std::uint32_t>(activations.size());
                    particle_buffer->setNbActiveParticles(active_particles);
                    particle_buffer->raiseFlags(
                        physx::PxParticleBufferFlag::eUPDATE_POSITION);
                    particle_buffer->raiseFlags(
                        physx::PxParticleBufferFlag::eUPDATE_VELOCITY);
                    particle_buffer->raiseFlags(
                        physx::PxParticleBufferFlag::eUPDATE_PHASE);
                }
                const std::uint32_t step = zero_step + 1u;
                notify(PhysxRuntimeEvent::ActivationUploaded, step,
                       static_cast<std::uint32_t>(activations.size()));
                notify(PhysxRuntimeEvent::SimulateBegin, step,
                       active_particles);
                errors.clear();
                scene->simulate(input.settings.fixed_step_seconds);
                physx::PxU32 hardware_error_state = 0u;
                const bool fetched =
                    scene->fetchResults(true, &hardware_error_state);
                if (options.hardware_error_injection_hook) {
                    hardware_error_state |=
                        options.hardware_error_injection_hook(
                            step,
                            options.hardware_error_injection_user_data);
                }
                if (hardware_error_state != 0u ||
                    !cuda->contextIsValid()) {
                    return fail_run(
                        FluidBakeCode::DeviceLost,
                        "PhysX PBD reported hardware error state " +
                            std::to_string(hardware_error_state));
                }
                if (!fetched) {
                    return fail_run(FluidBakeCode::BackendFailure,
                                    "PhysX PBD fetchResults failed");
                }
                if (errors.has_failure()) {
                    return fail_run(
                        errors.translated_code(
                            FluidBakeCode::BackendFailure),
                        errors.diagnostic(
                            "PhysX PBD simulation reported an error"));
                }
                output.stats.simulated_steps = step;
                output.stats.active_particles = active_particles;
                output.stats.peak_particles = std::max(
                    output.stats.peak_particles, active_particles);

                if (!gpu_sensor.enqueue(
                        particle_buffer->getPositionInvMasses(),
                        active_particles, input.sensor,
                        input.dry_collar_bounds_m,
                        gpu_sensor_error)) {
                    return fail_run(cuda_failure_code(
                                        gpu_sensor.last_cuda_error(), *cuda),
                                    gpu_sensor_error);
                }
            }

            std::vector<matter_physx::GpuFillSensorCounts> batch_counts;
            if (!gpu_sensor.read_batch(batch_counts, gpu_sensor_error)) {
                return fail_run(cuda_failure_code(
                                    gpu_sensor.last_cuda_error(), *cuda),
                                gpu_sensor_error);
            }
            const std::uint32_t executed_batch_steps =
                output.stats.simulated_steps - batch_start;
            if (batch_counts.size() != executed_batch_steps) {
                return fail_run(
                    FluidBakeCode::BackendFailure,
                    "GPU fill sensor returned an inconsistent batch size");
            }
            for (const matter_physx::GpuFillSensorCounts& counts :
                 batch_counts) {
                if (counts.non_finite_particles != 0u) {
                    output.stats.non_finite_particles =
                        counts.non_finite_particles;
                    return fail_run(
                        FluidBakeCode::NonFinite,
                        "PhysX PBD produced non-finite particle positions");
                }
                if (counts.escaped_particles != 0u) {
                    output.stats.escaped_particles = counts.escaped_particles;
                    return fail_run(
                        FluidBakeCode::Escaped,
                        "PhysX PBD particles crossed the dry collar");
                }
            }
            for (std::size_t sample = 0u;
                 sample < batch_counts.size(); ++sample) {
                const std::uint32_t step =
                    batch_start + static_cast<std::uint32_t>(sample) + 1u;
                const auto& counts = batch_counts[sample];
                notify(PhysxRuntimeEvent::SensorCountsReady, step,
                       counts.wet_columns);
                FluidBakeError sensor_error{};
                if (!update_fill_sensor_counts(
                        input.sensor, counts.wet_columns,
                        static_cast<std::uint32_t>(horizontal_cells_64), step,
                        fill_state, output.sensor, sensor_error)) {
                    return fail_run(sensor_error.code, sensor_error.message);
                }
                if (output.sensor.complete) {
                    completed = true;
                    break;
                }
            }

            notify(PhysxRuntimeEvent::BatchComplete,
                   output.stats.simulated_steps, active_particles);
            if (callbacks.progress) {
                callbacks.progress({output.stats.simulated_steps,
                                    input.settings.max_steps,
                                    active_particles,
                                    output.sensor.wet_fraction});
            }
            batch_start = batch_end;
        }

        if (!completed) {
            return fail_run(FluidBakeCode::SensorNotReached,
                            "PhysX PBD fill sensor was not reached before max_steps");
        }
        if (particle_ids.size() != active_particles) {
            return fail_run(FluidBakeCode::BackendFailure,
                            "PhysX PBD stable particle id count is inconsistent");
        }
        std::vector<physx::PxVec4> final_positions(active_particles);
        std::vector<physx::PxVec4> final_velocities(active_particles);
        physx::PxCUresult download_result = CUDA_SUCCESS;
        {
            physx::PxScopedCudaLock cuda_lock(*cuda);
            physx::PxCudaContext* context = cuda->getCudaContext();
            download_result = context->memcpyDtoH(
                final_positions.data(),
                reinterpret_cast<CUdeviceptr>(
                    particle_buffer->getPositionInvMasses()),
                final_positions.size() * sizeof(physx::PxVec4));
            if (download_result == CUDA_SUCCESS) {
                download_result = context->memcpyDtoH(
                    final_velocities.data(),
                    reinterpret_cast<CUdeviceptr>(
                        particle_buffer->getVelocities()),
                    final_velocities.size() * sizeof(physx::PxVec4));
            }
        }
        if (download_result != CUDA_SUCCESS) {
            return fail_run(
                cuda_failure_code(download_result.value, *cuda),
                "CUDA final particle snapshot failed with code " +
                    std::to_string(download_result.value));
        }
        output.particles.resize(active_particles);
        for (std::uint32_t index = 0u; index < active_particles; ++index) {
            output.particles[index] = {
                {final_positions[index].x, final_positions[index].y,
                 final_positions[index].z},
                {final_velocities[index].x, final_velocities[index].y,
                 final_velocities[index].z},
                particle_ids[index],
            };
        }
        output.stats.wall_seconds =
            std::chrono::duration<double>(
                std::chrono::steady_clock::now() - wall_start)
                .count();
        error = {};
        return true;
    }

    PhysxRuntimeOptions options;
    physx::PxDefaultAllocator allocator;
    MatterPhysxErrorCallback errors;
    matter_physx::PxOwner<physx::PxFoundation> foundation;
    matter_physx::PxOwner<physx::PxCudaContextManager> cuda;
    matter_physx::PxOwner<physx::PxPhysics> physics;
    bool attempted = false;
    FluidBackendProbe cached_probe{};
};

PhysxRuntime::PhysxRuntime() : PhysxRuntime(PhysxRuntimeOptions{}) {}

PhysxRuntime::PhysxRuntime(PhysxRuntimeOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

PhysxRuntime::~PhysxRuntime() = default;
PhysxRuntime::PhysxRuntime(PhysxRuntime&&) noexcept = default;
PhysxRuntime& PhysxRuntime::operator=(PhysxRuntime&&) noexcept = default;

FluidBackendProbe PhysxRuntime::probe() {
    try {
        if (!impl_) {
            return probe_failure(FluidBakeCode::BackendFailure,
                                 "PhysX runtime has been moved from");
        }
        return impl_->probe();
    } catch (const std::exception& exception) {
        const std::string message =
            "PhysX initialization exception: " +
            std::string(exception.what());
        return impl_ ? impl_->fail(FluidBakeCode::BackendFailure, message)
                     : probe_failure(FluidBakeCode::BackendFailure, message);
    } catch (...) {
        const std::string message =
            "Unknown PhysX initialization exception";
        return impl_ ? impl_->fail(FluidBakeCode::BackendFailure, message)
                     : probe_failure(FluidBakeCode::BackendFailure, message);
    }
}

bool PhysxRuntime::run(const FluidBakeInput& input,
                       const FluidBakeCallbacks& callbacks,
                       FluidBakeOutput& output, FluidBakeError& error) {
    try {
        if (!impl_) {
            output = {};
            error = {FluidBakeCode::BackendFailure,
                     "PhysX runtime has been moved from"};
            return false;
        }
        return impl_->run(input, callbacks, output, error);
    } catch (const std::bad_alloc&) {
        output = {};
        error = {FluidBakeCode::CapacityExceeded,
                 "PhysX run exhausted host memory"};
        return false;
    } catch (const std::exception& exception) {
        output = {};
        error = {FluidBakeCode::BackendFailure,
                 "PhysX run exception: " + std::string(exception.what())};
        return false;
    } catch (...) {
        output = {};
        error = {FluidBakeCode::BackendFailure,
                 "Unknown PhysX run exception"};
        return false;
    }
}

bool PhysxRuntime::run_collision_probe(
    const FluidCollisionProbeInput& input,
    FluidCollisionProbeOutput& output,
    FluidBakeError& error) {
    try {
        if (!impl_) {
            output = {};
            error = {FluidBakeCode::BackendFailure,
                     "PhysX runtime has been moved from"};
            return false;
        }
        return impl_->run_collision_probe(input, output, error);
    } catch (const std::exception& exception) {
        output = {};
        error = {FluidBakeCode::BackendFailure,
                 "PhysX collision probe exception: " +
                     std::string(exception.what())};
        return false;
    } catch (...) {
        output = {};
        error = {FluidBakeCode::BackendFailure,
                 "Unknown PhysX collision probe exception"};
        return false;
    }
}

}  // namespace hydrology
