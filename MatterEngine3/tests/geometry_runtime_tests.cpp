#include "matter/windows_compat.h"
#define GLFW_INCLUDE_NONE
#include "render/geometry_world_runtime.h"
#include "render/part_store.h"
#include "matter/vulkan_device.h"
#include <GLFW/glfw3.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>

static unsigned thread_count() {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 entry{}; entry.dwSize = sizeof(entry);
    unsigned count = 0;
    if (Thread32First(snapshot, &entry)) do {
        if (entry.th32OwnerProcessID == GetCurrentProcessId()) ++count;
    } while (Thread32Next(snapshot, &entry));
    CloseHandle(snapshot);
    return count;
}
#endif

int main(int argc, char** argv) {
    unsigned failures = 0;
    const auto check = [&](bool ok, const char* message) {
        if (!ok) { std::printf("FAIL: %s\n", message); ++failures; }
    };
#ifdef _WIN32
    const auto before = thread_count();
    check(before != 0, "thread census is available");
#endif
    // These are the runtime observations used by ordinary source frames,
    // including temporal revisions and the GPU pick reverse map.
    viewer::GeometryWorldRuntime runtime;
    for (unsigned frame = 0; frame < 3; ++frame) {
        const auto paging = runtime.take_profile();
        const auto stats = runtime.stats();
        check(runtime.revision() == 0, "inactive runtime publishes no revisions");
        check(runtime.source_part(123, 456) == 456, "inactive picks retain source identity");
        check(paging.bank_capacity == 0 && paging.bank_backing_allocations == 0,
              "inactive runtime reserves no paging payload bank");
        check(paging.update.count == 0 && paging.scene_submitted == 0 &&
              paging.queued_reads == 0 && paging.pending_uploads == 0,
              "inactive observations trigger no scans, streaming or publication");
        check(stats.assets == 0 && stats.pages == 0 && stats.gpu_bytes == 0,
              "inactive runtime retains no geometry residency");
    }
#ifdef _WIN32
    check(thread_count() == before, "inactive runtime starts no paging worker lanes");
#endif
    if (argc == 2 && std::strcmp(argv[1], "--gpu") == 0) {
#ifdef MATTER_VK_TEST_LAYER_PATH
        SetDllDirectoryA(MATTER_VK_TEST_LAYER_PATH);
        SetEnvironmentVariableA("VK_LAYER_PATH", MATTER_VK_TEST_LAYER_PATH);
#endif
        check(glfwInit() == GLFW_TRUE, "GLFW initializes");
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        auto* window = glfwCreateWindow(64, 64, "Geometry runtime lifecycle", nullptr, nullptr);
        check(window != nullptr, "native test window exists");
        std::string error;
        auto vk = window ? matter::VulkanDevice::create(window, true, error) : nullptr;
        check(bool(vk), error.c_str());
        if (vk) {
            {
                viewer::VkSceneRenderer renderer(*vk);
                viewer::PartStore store((std::filesystem::temp_directory_path() / "matter-runtime-lifecycle").string());
                viewer::GeometryWorldRuntime lifecycle;
                matter::VulkanFrame frame{}; frame.extent = {64, 64};
                matter::CameraDesc camera{};
                viewer::VkSceneInstance source{}; source.part_hash = 456; source.instance_id = 123;
                std::vector<viewer::VkSceneInstance> output;
                const auto update = [&](const std::vector<viewer::VkSceneInstance>& admitted) {
                    const bool ok = lifecycle.update(store, renderer, *vk, frame, camera, 1, admitted, output, error);
                    check(ok, error.c_str());
                };
                update({source});
                check(output.size() == 1 && output[0].part_hash == 456 && output[0].instance_id == 123,
                      "disabled update forwards ordinary source instances");
                lifecycle.reset(renderer);
                check(lifecycle.take_profile().bank_capacity == 0 && lifecycle.revision() == 0,
                      "disabled update/reset never initializes paging");
                store.set_geometry_pages_enabled(true);
#ifdef _WIN32
                _putenv_s("MATTER_GEOMETRY_PREPARE_ONLY", "1");
                _putenv_s("MATTER_GEOMETRY_PAGES_PROFILE", "1");
#else
                setenv("MATTER_GEOMETRY_PREPARE_ONLY", "1", 1);
                setenv("MATTER_GEOMETRY_PAGES_PROFILE", "1", 1);
#endif
                update({source});
                check(lifecycle.take_profile().bank_capacity == 0 && output.size() == 1,
                      "prepare-only opt-in keeps paging workers and banks inactive");
#ifdef _WIN32
                _putenv_s("MATTER_GEOMETRY_PREPARE_ONLY", "0");
#else
                setenv("MATTER_GEOMETRY_PREPARE_ONLY", "0", 1);
#endif
                update({});
                const auto initialized = lifecycle.take_profile();
                check(initialized.bank_capacity != 0 && initialized.bank_backing_allocations == 1,
                      "first paging update commits exactly one page bank");
                check(initialized.update.count == 1, "first paging update executes successfully");
                lifecycle.reset(renderer);
                update({});
                check(lifecycle.take_profile().bank_capacity == initialized.bank_capacity,
                      "opt-in reset reuses the initialized runtime bank");
                lifecycle.reset(renderer);
            }
            vk->wait_idle();
            std::printf("validation errors: %u\n", vk->validation_error_count());
            check(vk->validation_error_count() == 0, "runtime lifecycle has no Vulkan validation errors");
        }
        vk.reset();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
    }
    std::printf("geometry_runtime_tests: %s\n", failures ? "FAIL" : "ALL PASS");
    return failures ? 1 : 0;
}
