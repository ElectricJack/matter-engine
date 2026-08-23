#include "hydrology/physx_runtime.h"

#include "physx_raii.h"

#include "PxPhysicsAPI.h"
#include "extensions/PxDefaultCpuDispatcher.h"
#include "extensions/PxRigidActorExt.h"
#include "extensions/PxRigidBodyExt.h"
#include "gpu/PxGpu.h"

#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
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

bool PhysxRuntime::run(const FluidBakeInput&, const FluidBakeCallbacks&,
                       FluidBakeOutput& output, FluidBakeError& error) {
    try {
        output = {};
        const FluidBackendProbe state = probe();
        if (!state.available) {
            error = {state.code, state.message};
            return false;
        }
        error = {
            FluidBakeCode::BackendFailure,
            "PhysX particle execution is not implemented in this milestone"};
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
