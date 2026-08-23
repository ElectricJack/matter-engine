#include "hydrology/physx_runtime.h"

#include "physx_raii.h"

#include "PxPhysicsAPI.h"
#include "gpu/PxGpu.h"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>

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

}  // namespace hydrology
