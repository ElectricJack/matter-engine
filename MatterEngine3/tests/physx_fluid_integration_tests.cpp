#include "check.h"

#include "hydrology/physx_runtime.h"

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

void test_core_sdk_is_statically_linked() {
#ifdef _WIN32
    CHECK(GetModuleHandleA("PhysX_64.dll") == nullptr &&
              GetModuleHandleA("PhysXCommon_64.dll") == nullptr &&
              GetModuleHandleA("PhysXFoundation_64.dll") == nullptr &&
              GetModuleHandleA("PhysXCooking_64.dll") == nullptr,
          "PhysX core/cooking DLLs are absent from the process import graph");
#endif
}

void test_missing_gpu_runtime_is_a_stable_probe_failure() {
    hydrology::PhysxRuntimeOptions options{};
    options.gpu_runtime_path =
        (std::filesystem::temp_directory_path() /
         "matter-definitely-missing-PhysXGpu_64.dll")
            .string();
    hydrology::PhysxRuntime runtime(options);
    const auto probe = runtime.probe();
    CHECK(!probe.available,
          "missing PhysX GPU module is not reported as available");
    CHECK(probe.code == hydrology::FluidBakeCode::BackendUnavailable,
          "missing GPU module receives stable BackendUnavailable code");
    CHECK(probe.message.find("GPU") != std::string::npos ||
              probe.message.find("gpu") != std::string::npos,
          "missing GPU module retains an actionable diagnostic");
}

void throw_during_initialization() {
    throw std::runtime_error("injected PhysX initialization failure");
}

void test_initialization_exceptions_do_not_cross_the_backend_boundary() {
    hydrology::PhysxRuntimeOptions options{};
    options.initialization_hook = &throw_during_initialization;
    hydrology::PhysxRuntime runtime(options);

    hydrology::FluidBackendProbe probe{};
    bool probe_threw = false;
    try {
        probe = runtime.probe();
    } catch (...) {
        probe_threw = true;
    }
    CHECK(!probe_threw && !probe.available &&
              probe.code == hydrology::FluidBakeCode::BackendFailure &&
              probe.message.find("injected PhysX initialization failure") !=
                  std::string::npos,
          "probe translates initialization exceptions into BackendFailure");

    hydrology::FluidBakeOutput output{};
    hydrology::FluidBakeError error{};
    bool run_threw = false;
    try {
        (void)runtime.run({}, {}, output, error);
    } catch (...) {
        run_threw = true;
    }
    CHECK(!run_threw &&
              error.code == hydrology::FluidBakeCode::BackendFailure,
          "run does not leak a cached initialization exception");
}

void test_exact_sdk_cuda_context_and_rtx_identity() {
    for (int iteration = 0; iteration != 3; ++iteration) {
        hydrology::PhysxRuntime runtime;
        const auto probe = runtime.probe();
        CHECK(probe.available, probe.message.c_str());
        CHECK(probe.code == hydrology::FluidBakeCode::Ready,
              "successful native probe reports Ready");
        CHECK(probe.backend_name == "NVIDIA PhysX PBD" &&
                  probe.sdk_version == "5.6.1" &&
                  probe.sdk_version_hex == 0x05060100u,
              "compiled and runtime PhysX identity is exactly 5.6.1");
        CHECK(probe.cuda_context_valid && probe.device_ordinal >= 0,
              "native probe creates a valid CUDA context");
        CHECK(probe.device_name.find("RTX 4090") != std::string::npos,
              "native probe selects the reference RTX 4090");
        CHECK(probe.cuda_driver_version > 0 &&
                  probe.device_total_memory_bytes >
                      (static_cast<std::uint64_t>(20u) << 30u),
              "native probe reports CUDA driver and device memory");

        const auto repeated = runtime.probe();
        CHECK(repeated.available &&
                  repeated.device_name == probe.device_name &&
                  repeated.device_ordinal == probe.device_ordinal,
              "repeated probe is stable without recreating the runtime");
    }
}

}  // namespace

int main() {
    test_core_sdk_is_statically_linked();
    test_missing_gpu_runtime_is_a_stable_probe_failure();
    test_initialization_exceptions_do_not_cross_the_backend_boundary();
    test_exact_sdk_cuda_context_and_rtx_identity();
    test_missing_gpu_runtime_is_a_stable_probe_failure();
    return check_summary();
}
