// MatterEngine3 Vulkan world viewer. The production path creates a GLFW
// NO_API window and presents genuine WorldSession data through VkSceneRenderer.
// MATTER_CAM, MATTER_WORLD, MATTER_HIZ, MATTER_SCREENSHOT and FIFO commands are
// retained from the legacy viewer.
#include "matter/engine_context.h"
#include "matter/vulkan_device.h"
#include "matter/world_session.h"
#include "camera_controller.h"
#include "ui.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "external/stb_image_write.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <string>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

void init_camera(matter::CameraDesc& camera) {
    camera.position = {20.0f, 16.0f, 34.0f};
    camera.target = {0.0f, 9.0f, 0.0f};
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.vertical_fov_radians = 0.78539816339f;
    camera.near_plane = 1.0f;
    camera.far_plane = 5000.0f;
}

void apply_world_resolver_defaults(const std::string& world_name,
                                   float& active_radius,
                                   float& min_projected_size,
                                   viewer::ViewerStats& stats) {
    if (world_name == "Meadow") {
        active_radius = 400.0f;
        min_projected_size = 0.0015f;
        stats.resolver_choice = 1;
    } else {
        active_radius = 64.0f;
        min_projected_size = 0.0f;
        stats.resolver_choice = 0;
    }
}

bool key_pressed(GLFWwindow* window, int key, bool& previous) {
    const bool down = glfwGetKey(window, key) == GLFW_PRESS;
    const bool pressed = down && !previous;
    previous = down;
    return pressed;
}

bool write_png(const std::string& path, const std::vector<uint8_t>& rgba,
               uint32_t width, uint32_t height) {
    if (rgba.size() != static_cast<size_t>(width) * height * 4) return false;
    const std::filesystem::path output(path);
    std::error_code ec;
    if (output.has_parent_path())
        std::filesystem::create_directories(output.parent_path(), ec);
    return stbi_write_png(path.c_str(), static_cast<int>(width),
                          static_cast<int>(height), 4, rgba.data(),
                          static_cast<int>(width * 4)) != 0;
}

std::string examples_root() {
    if (std::filesystem::is_directory("MatterEngine3/examples"))
        return "MatterEngine3/examples";
    return "../MatterEngine3/examples";
}

std::string shared_lib_root() {
    if (std::filesystem::is_directory("MatterEngine3/shared-lib"))
        return "MatterEngine3/shared-lib";
    return "../MatterEngine3/shared-lib";
}

struct PerfRunConfig {
    bool enabled = false;
    std::string output_path;
    double warmup_seconds = 0.0;
    double sample_seconds = 0.0;
};

struct PerfCounters {
    uint64_t vertex_uploads = 0;
    uint64_t cluster_uploads = 0;
    uint64_t instance_uploads = 0;
    uint64_t immediate_submits = 0;
};

bool parse_perf_seconds(const char* value, const char* name, double& result,
                        std::string& error) {
    char* end = nullptr;
    result = std::strtod(value, &end);
    if (end == value || *end != '\0' || !std::isfinite(result) || result < 0.0) {
        error = std::string(name) + " must be a finite non-negative number";
        return false;
    }
    return true;
}

bool read_perf_run_config(PerfRunConfig& config, std::string& error) {
    const char* output = std::getenv("MATTER_PERF_OUTPUT");
    const char* warmup = std::getenv("MATTER_PERF_WARMUP_SECONDS");
    const char* sample = std::getenv("MATTER_PERF_SAMPLE_SECONDS");
    if (!output && !warmup && !sample) return true;
    if (!output || !*output || !warmup || !*warmup || !sample || !*sample) {
        error = "MATTER_PERF_OUTPUT, MATTER_PERF_WARMUP_SECONDS, and "
                "MATTER_PERF_SAMPLE_SECONDS must be set together";
        return false;
    }
    config.enabled = true;
    config.output_path = output;
    if (!parse_perf_seconds(warmup, "MATTER_PERF_WARMUP_SECONDS",
                            config.warmup_seconds, error) ||
        !parse_perf_seconds(sample, "MATTER_PERF_SAMPLE_SECONDS",
                            config.sample_seconds, error)) {
        return false;
    }
    if (!(config.sample_seconds > 0.0)) {
        error = "MATTER_PERF_SAMPLE_SECONDS must be greater than zero";
        return false;
    }
    return true;
}

PerfCounters capture_perf_counters(const matter::FrameStats& stats) {
    return {stats.vk_vertex_uploads, stats.vk_cluster_uploads,
            stats.vk_instance_uploads, stats.vk_immediate_submits};
}

double median_of_sorted(const std::vector<double>& sorted) {
    const size_t middle = sorted.size() / 2;
    return (sorted.size() & 1) != 0
               ? sorted[middle]
               : (sorted[middle - 1] + sorted[middle]) * 0.5;
}

std::string json_string(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const unsigned char c : value) {
        switch (c) {
            case '\"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (c < 0x20) {
                    char encoded[7]{};
                    std::snprintf(encoded, sizeof(encoded), "\\u%04x", c);
                    escaped += encoded;
                } else {
                    escaped += static_cast<char>(c);
                }
        }
    }
    return escaped;
}

bool write_perf_result(const PerfRunConfig& config, const std::string& world,
                       std::vector<double> frame_times, const PerfCounters& start,
                       const PerfCounters& finish,
                       const matter::FrameStats& frame_stats,
                       uint64_t dlss_reset_start,
                       uint32_t validation_errors,
                       std::string& error) {
    if (frame_times.empty()) {
        error = "no performance frames were sampled";
        return false;
    }
    std::sort(frame_times.begin(), frame_times.end());
    const double median_frame_ms = median_of_sorted(frame_times);
    const size_t p95_index = static_cast<size_t>(
        std::ceil(static_cast<double>(frame_times.size()) * 0.95)) - 1;
    const double p95_frame_ms = frame_times[p95_index];
    const double median_fps = median_frame_ms > 0.0 ? 1000.0 / median_frame_ms : 0.0;
    std::ofstream output(config.output_path, std::ios::out | std::ios::trunc);
    if (!output) {
        error = "could not write MATTER_PERF_OUTPUT '" + config.output_path + "'";
        return false;
    }
    output << std::fixed << std::setprecision(6)
           << "{\"world\":\"" << world << "\",\"frames\":"
           << frame_times.size() << ",\"frame_metric\":\"end_to_end_cadence\""
           << ",\"median_frame_ms\":" << median_frame_ms
           << ",\"median_fps\":" << median_fps
           << ",\"p95_frame_ms\":" << p95_frame_ms
           << ",\"static_vertex_upload_delta\":"
           << (finish.vertex_uploads - start.vertex_uploads)
           << ",\"static_cluster_upload_delta\":"
           << (finish.cluster_uploads - start.cluster_uploads)
           << ",\"stable_instance_upload_delta\":"
           << (finish.instance_uploads - start.instance_uploads)
           << ",\"immediate_submit_delta\":"
           << (finish.immediate_submits - start.immediate_submits)
           << ",\"selected_dlss_mode\":\""
           << matter::dlss_mode_name(frame_stats.dlss_selected_mode) << "\""
           << ",\"active_dlss_mode\":\""
           << matter::dlss_mode_name(frame_stats.dlss_active_mode) << "\""
           << ",\"dlss_internal_width\":" << frame_stats.dlss_internal_width
           << ",\"dlss_internal_height\":" << frame_stats.dlss_internal_height
           << ",\"dlss_output_width\":" << frame_stats.dlss_output_width
           << ",\"dlss_output_height\":" << frame_stats.dlss_output_height
           << ",\"dlss_reset_delta\":"
           << (frame_stats.dlss_reset_count >= dlss_reset_start
                   ? frame_stats.dlss_reset_count - dlss_reset_start
                   : frame_stats.dlss_reset_count)
           << ",\"rt_available\":"
           << (frame_stats.vk_rt_available ? "true" : "false")
           << ",\"rt_enabled\":"
           << (frame_stats.vk_rt_effective ? "true" : "false")
           << ",\"rt_samples\":" << frame_stats.vk_rt_samples
           << ",\"rt_debug_view\":"
           << (frame_stats.vk_rt_debug_view ? "true" : "false")
           << ",\"vk_rt_available\":"
           << (frame_stats.vk_rt_available ? "true" : "false")
           << ",\"vk_rt_effective\":"
           << (frame_stats.vk_rt_effective ? "true" : "false")
           << ",\"vk_rt_trace_dispatches\":"
           << frame_stats.vk_rt_trace_dispatches
           << ",\"vk_rt_fallback_reason\":\""
           << json_string(frame_stats.vk_rt_fallback_reason) << "\""
           << ",\"fallback_reason\":\""
           << json_string(frame_stats.dlss_reason) << "\""
           << ",\"validation_errors\":" << validation_errors << "}\n";
    if (!output) {
        error = "failed while writing MATTER_PERF_OUTPUT '" + config.output_path + "'";
        return false;
    }
    return true;
}

} // namespace

int main() {
    PerfRunConfig perf;
    std::string perf_error;
    if (!read_perf_run_config(perf, perf_error)) {
        std::fprintf(stderr, "FATAL: %s\n", perf_error.c_str());
        return 1;
    }
    if (!glfwInit()) {
        std::fprintf(stderr, "FATAL: glfwInit failed\n");
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    GLFWwindow* window = glfwCreateWindow(
        1280, 720, "MatterEngine3 World Viewer", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "FATAL: glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }

    std::string error;
    auto vulkan = matter::VulkanDevice::create(window, true, error);
    if (!vulkan) {
        std::fprintf(stderr, "FATAL: %s\n", error.c_str());
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    const bool disable_vulkan_rt =
        std::getenv("MATTER_DISABLE_VK_RT") != nullptr;
    std::printf("Vulkan RT available=%s enabled=%s reason=%s\n",
                vulkan->ray_tracing_available() ? "true" : "false",
                vulkan->ray_tracing_available() && !disable_vulkan_rt
                    ? "true"
                    : "false",
                vulkan->ray_tracing_available()
                    ? (disable_vulkan_rt ? "disabled by MATTER_DISABLE_VK_RT"
                                         : "none")
                    : vulkan->ray_tracing_unavailable_reason().c_str());
    matter::EngineDesc engine_desc;
    const char* cache_root_env = std::getenv("MATTER_CACHE_ROOT");
    engine_desc.cache_root = cache_root_env ? cache_root_env : "cache";
    engine_desc.render_device = vulkan.get();
    auto engine = matter::EngineContext::create(engine_desc, error);
    if (!engine) {
        std::fprintf(stderr, "FATAL: %s\n", error.c_str());
        vulkan.reset();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    viewer::Ui ui;
    if (!ui.setup(window, *vulkan, error)) {
        std::fprintf(stderr, "FATAL: %s\n", error.c_str());
        engine.reset();
        vulkan.reset();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    auto worlds = viewer::scan_worlds(examples_root());
    std::printf("worlds available (%d):\n", static_cast<int>(worlds.size()));
    for (size_t i = 0; i < worlds.size(); ++i)
        std::printf("  [%zu] %s  (%s / %s)\n", i, worlds[i].label.c_str(),
                    worlds[i].schemas_dir.c_str(),
                    worlds[i].world_data_dir.c_str());
    if (worlds.empty()) {
        std::fprintf(stderr, "FATAL: no worlds found under %s\n",
                     examples_root().c_str());
        ui.shutdown();
        engine.reset();
        vulkan.reset();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    matter::CameraDesc camera{};
    init_camera(camera);
    if (const char* value = std::getenv("MATTER_CAM")) {
        float c[6];
        if (std::sscanf(value, "%f,%f,%f,%f,%f,%f", &c[0], &c[1], &c[2],
                        &c[3], &c[4], &c[5]) == 6) {
            camera.position = {c[0], c[1], c[2]};
            camera.target = {c[3], c[4], c[5]};
            std::printf("MATTER_CAM: eye(%.1f,%.1f,%.1f) target(%.1f,%.1f,%.1f)\n",
                        c[0], c[1], c[2], c[3], c[4], c[5]);
        }
    }

    int initial_world = 0;
    if (const char* value = std::getenv("MATTER_WORLD")) {
        std::string wanted(value);
        std::transform(wanted.begin(), wanted.end(), wanted.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        bool found = false;
        for (size_t i = 0; i < worlds.size(); ++i) {
            std::string candidate = worlds[i].world_name;
            std::transform(candidate.begin(), candidate.end(), candidate.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (candidate == wanted) {
                initial_world = static_cast<int>(i);
                found = true;
                break;
            }
        }
        if (!found) {
            std::fprintf(stderr,
                         "FATAL: MATTER_WORLD '%s' is not a committed world\n",
                         value);
            ui.shutdown();
            engine.reset();
            vulkan.reset();
            glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }
    }

    viewer::ViewerStats stats{};
    stats.world_current = initial_world;
    stats.gpu_cull_active = true;
    stats.connected = true;
    if (std::getenv("MATTER_HIZ"))
        std::printf("MATTER_HIZ: not available in Vulkan milestone; ignored\n");
    float active_radius = 64.0f;
    float min_projected_size = 0.0f;
    apply_world_resolver_defaults(worlds[initial_world].world_name,
                                  active_radius, min_projected_size, stats);
    bool wireframe = false;

    const std::string shared_lib = shared_lib_root();
    auto open_world = [&](const viewer::WorldEntry& entry) {
        matter::WorldDesc desc;
        desc.schemas_dir = entry.schemas_dir.c_str();
        desc.world_data_dir = entry.world_data_dir.c_str();
        desc.world_name = entry.world_name.c_str();
        desc.shared_lib_dir = shared_lib.c_str();
        desc.enable_live_edit = std::getenv("MATTER_LIVE_EDIT") != nullptr;
        std::string world_error;
        auto result = engine->open_world(desc, world_error);
        if (!result) {
            std::fprintf(stderr, "open_world: %s\n", world_error.c_str());
            return result;
        }
        result->request_bake();
        return result;
    };
    auto session = open_world(worlds[initial_world]);
    if (!session) {
        ui.shutdown(); engine.reset(); vulkan.reset();
        glfwDestroyWindow(window); glfwTerminate();
        return 1;
    }

    bool camera_capture = false;
    bool tab_down = false;
    bool f9_down = false;
    bool f8_down = false;
    bool dlss_modes_supported = false;
    matter::DlssMode selected_dlss_mode = matter::DlssMode::Native;
    if (const char* initial_dlss_mode = std::getenv("MATTER_DLSS_MODE")) {
        if (std::strcmp(initial_dlss_mode, "quality") == 0)
            selected_dlss_mode = matter::DlssMode::Quality;
        else if (std::strcmp(initial_dlss_mode, "balanced") == 0)
            selected_dlss_mode = matter::DlssMode::Balanced;
        else if (std::strcmp(initial_dlss_mode, "performance") == 0)
            selected_dlss_mode = matter::DlssMode::Performance;
        else if (std::strcmp(initial_dlss_mode, "native") != 0)
            std::fprintf(stderr,
                         "MATTER_DLSS_MODE: expected native, quality, balanced, or performance; using native\n");
    }
    matter::DlssMode reported_selected_dlss_mode =
        static_cast<matter::DlssMode>(255);
    matter::DlssMode reported_active_dlss_mode =
        static_cast<matter::DlssMode>(255);
    uint32_t reported_dlss_internal_width = UINT32_MAX;
    uint32_t reported_dlss_internal_height = UINT32_MAX;
    uint32_t reported_dlss_output_width = UINT32_MAX;
    uint32_t reported_dlss_output_height = UINT32_MAX;
    uint64_t reported_dlss_resets = UINT64_MAX;
    bool reported_vk_rt_effective = false;
    uint32_t reported_vk_rt_dispatches = UINT32_MAX;
    std::string reported_vk_rt_reason;
    bool reported_vk_rt_once = false;
    viewer::CameraController camera_controller;
    const char* screenshot_env = std::getenv("MATTER_SCREENSHOT");
    const std::string screenshot_path = screenshot_env ? screenshot_env : "";
    int screenshot_settle = 0;
    int screenshot_failures = 0;
    bool bake_ready = false;
    bool selected_world_reported = false;
    const bool test_resize = std::getenv("MATTER_TEST_RESIZE") != nullptr;
    const bool hide_ui = std::getenv("MATTER_HIDE_UI") != nullptr;
    bool resize_exercised = false;
    if (hide_ui) std::printf("viewer: UI hidden by MATTER_HIDE_UI\n");

    int cmd_fd = -1;
#ifdef _WIN32
    HANDLE cmd_handle = INVALID_HANDLE_VALUE;
    LARGE_INTEGER cmd_offset{};
#endif
    std::string cmd_buffer;
    const char* fifo_path = std::getenv("MATTER_CMD_FIFO");
#ifndef _WIN32
    if (fifo_path) {
        mkfifo(fifo_path, 0600);
        cmd_fd = open(fifo_path, O_RDWR | O_NONBLOCK);
        if (cmd_fd >= 0)
            std::printf("MATTER_CMD_FIFO: listening on %s\n", fifo_path);
        else
            std::printf("MATTER_CMD_FIFO: failed to open %s\n", fifo_path);
    }
#else
    if (fifo_path) {
        // Windows has no POSIX FIFO. Poll an append-only command file so the
        // documented command stream remains practical and nonblocking.
        cmd_handle = CreateFileA(fifo_path, GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE |
                                     FILE_SHARE_DELETE,
                                 nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                 nullptr);
        if (cmd_handle != INVALID_HANDLE_VALUE)
            std::printf("MATTER_CMD_FIFO: polling command file %s\n", fifo_path);
        else
            std::printf("MATTER_CMD_FIFO: failed to open command file %s\n",
                        fifo_path);
    }
#endif
    std::string shot_path;
    std::string stats_label;
    int shot_settle = 0;
    bool quit_requested = false;
    bool fatal_error = false;
    enum class PerfPhase { WaitingForBake, Warming, Sampling, Complete };
    PerfPhase perf_phase = PerfPhase::WaitingForBake;
    std::chrono::steady_clock::time_point perf_phase_start{};
    PerfCounters perf_start_counters{};
    uint64_t perf_start_dlss_resets = 0;
    std::vector<double> perf_frame_times;
    auto previous_time = std::chrono::steady_clock::now();

    while (!glfwWindowShouldClose(window) && !quit_requested && !fatal_error) {
        // This starts before event polling and begin_frame(), whose fence wait and
        // swapchain acquire are part of the user-visible frame cadence.
        const auto perf_frame_start = std::chrono::steady_clock::now();
        glfwPollEvents();
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - previous_time).count();
        previous_time = now;
        if (key_pressed(window, GLFW_KEY_TAB, tab_down)) {
            camera_capture = !camera_capture;
            camera_controller.set_capture(window, camera_capture);
        }
        if (key_pressed(window, GLFW_KEY_F9, f9_down))
            std::printf("wireframe: not available in Vulkan milestone\n");
        if (key_pressed(window, GLFW_KEY_F8, f8_down)) {
            if (!dlss_modes_supported) {
                selected_dlss_mode = matter::DlssMode::Native;
                std::printf("DLSS: Native (%s)\n",
                            vulkan->dlss_unavailable_reason().c_str());
            } else {
                switch (selected_dlss_mode) {
                    case matter::DlssMode::Native:
                        selected_dlss_mode = matter::DlssMode::Quality;
                        break;
                    case matter::DlssMode::Quality:
                        selected_dlss_mode = matter::DlssMode::Balanced;
                        break;
                    case matter::DlssMode::Balanced:
                        selected_dlss_mode = matter::DlssMode::Performance;
                        break;
                    case matter::DlssMode::Performance:
                        selected_dlss_mode = matter::DlssMode::Native;
                        break;
                }
            }
        }
        camera_controller.update(window, dt, camera);

#ifndef _WIN32
        if (cmd_fd >= 0) {
            char bytes[512];
            ssize_t count = 0;
            while ((count = read(cmd_fd, bytes, sizeof(bytes))) > 0)
                cmd_buffer.append(bytes, static_cast<size_t>(count));
        }
#else
        if (cmd_handle != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER size{};
            if (GetFileSizeEx(cmd_handle, &size) &&
                size.QuadPart < cmd_offset.QuadPart)
                cmd_offset.QuadPart = 0;
            if (size.QuadPart > cmd_offset.QuadPart) {
                SetFilePointerEx(cmd_handle, cmd_offset, nullptr, FILE_BEGIN);
                char bytes[512];
                DWORD count = 0;
                while (ReadFile(cmd_handle, bytes, sizeof(bytes), &count,
                                nullptr) && count > 0) {
                    cmd_buffer.append(bytes, static_cast<size_t>(count));
                    cmd_offset.QuadPart += count;
                    if (count < sizeof(bytes)) break;
                }
            }
        }
#endif
        {
            size_t newline = 0;
            while ((newline = cmd_buffer.find('\n')) != std::string::npos) {
                std::string line = cmd_buffer.substr(0, newline);
                cmd_buffer.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                float c[6]; char word[256];
                if (std::sscanf(line.c_str(), "cam %f %f %f %f %f %f",
                                &c[0], &c[1], &c[2], &c[3], &c[4], &c[5]) == 6) {
                    camera.position = {c[0], c[1], c[2]};
                    camera.target = {c[3], c[4], c[5]};
                } else if (std::sscanf(line.c_str(), "shot %255s", word) == 1) {
                    shot_path = word; shot_settle = 3;
                } else if (std::sscanf(line.c_str(), "stats %255s", word) == 1) {
                    stats_label = word;
                } else if (std::sscanf(line.c_str(), "budget %f", &c[0]) == 1) {
                    stats.pixel_budget = std::max(0.05f, std::min(4.0f, c[0]));
                } else if (std::sscanf(line.c_str(), "hiz %255s", word) == 1) {
                    std::printf("hiz: not available in Vulkan milestone\n");
                } else if (std::sscanf(line.c_str(), "dlss %255s", word) == 1) {
                    if (std::strcmp(word, "native") == 0)
                        selected_dlss_mode = matter::DlssMode::Native;
                    else if (std::strcmp(word, "quality") == 0)
                        selected_dlss_mode = matter::DlssMode::Quality;
                    else if (std::strcmp(word, "balanced") == 0)
                        selected_dlss_mode = matter::DlssMode::Balanced;
                    else if (std::strcmp(word, "performance") == 0)
                        selected_dlss_mode = matter::DlssMode::Performance;
                    else
                        std::printf("dlss: expected native, quality, balanced, or performance\n");
                } else if (line == "reload") {
                    stats.reload_requested = true;
                } else if (line == "wireframe" || line == "wireframe toggle") {
                    std::printf("wireframe: not available in Vulkan milestone\n");
                } else if (line == "wireframe on" || line == "wireframe off") {
                    std::printf("wireframe: not available in Vulkan milestone\n");
                } else if (line == "quit") {
                    quit_requested = true;
                } else if (!line.empty()) {
                    std::printf("cmd: unrecognized '%s'\n", line.c_str());
                }
            }
        }

        const float focus[3] = {camera.position.x, camera.position.y,
                                camera.position.z};
        session->set_bake_focus(focus);
        session->tick();
        session->pump_gpu_jobs(4.0f);
        matter::Event event;
        while (session->poll_event(event)) {
            if (event.type == matter::EventType::BakePartDone)
                std::printf("bake %d/%d %s\n", event.done, event.total,
                            event.module.c_str());
            else if (event.type == matter::EventType::BakeFinished) {
                std::printf("bake finished (%d errors)\n", event.errors);
                bake_ready = event.errors == 0;
                matter::InstanceInfo selected{};
                if (bake_ready && !selected_world_reported &&
                    session->instance_info(0, selected)) {
                    std::printf("selected world %s hash %016llx\n",
                                worlds[stats.world_current].world_name.c_str(),
                                static_cast<unsigned long long>(selected.part_hash));
                    selected_world_reported = true;
                }
                if (fifo_path) std::printf("viewer: bake ready\n");
            } else if (event.type == matter::EventType::BakeError)
                std::printf("bake error [%s]: %s\n", event.module.c_str(),
                            event.message.c_str());
        }
        if (test_resize && bake_ready && !resize_exercised) {
            glfwSetWindowSize(window, 960, 540);
            glfwPollEvents();
            screenshot_settle = 0;
            resize_exercised = true;
        }

        matter::VulkanFrame frame{};
        if (!vulkan->begin_frame(frame, error)) {
            if (error.find("zero-sized") != std::string::npos) {
                glfwWaitEventsTimeout(0.05);
                continue;
            }
            std::fprintf(stderr, "FATAL: begin_frame: %s\n", error.c_str());
            break;
        }

        matter::RenderOptions options;
        options.path = matter::RenderPath::GpuDriven;
        options.resolver = stats.resolver_choice == 1
                               ? matter::ResolverKind::SectorLod
                               : matter::ResolverKind::PassThrough;
        options.wireframe = false;
        options.hiz_occlusion = false;
        options.pixel_budget = stats.pixel_budget;
        options.active_radius = active_radius;
        options.min_projected_size = min_projected_size;
        options.dlss_mode = selected_dlss_mode;
        options.vulkan_lighting = stats.lighting;
        options.vulkan_ray_tracing.enabled =
            vulkan->ray_tracing_available() && !disable_vulkan_rt;
        const auto render_start = std::chrono::steady_clock::now();
        if (!session->render(camera, frame, options, error)) {
            std::fprintf(stderr, "FATAL: render: %s\n", error.c_str());
            fatal_error = true;
        }
        const matter::FrameStats& frame_stats = session->frame_stats();
        dlss_modes_supported = vulkan->dlss_available() &&
                               frame_stats.dlss_reason.empty();
        if (reported_selected_dlss_mode != frame_stats.dlss_selected_mode ||
            reported_active_dlss_mode != frame_stats.dlss_active_mode ||
            reported_dlss_internal_width != frame_stats.dlss_internal_width ||
            reported_dlss_internal_height != frame_stats.dlss_internal_height ||
            reported_dlss_output_width != frame_stats.dlss_output_width ||
            reported_dlss_output_height != frame_stats.dlss_output_height ||
            reported_dlss_resets != frame_stats.dlss_reset_count) {
            std::printf(
                "DLSS selected=%s active=%s internal=%ux%u output=%ux%u resets=%llu reason=%s\n",
                matter::dlss_mode_name(frame_stats.dlss_selected_mode),
                matter::dlss_mode_name(frame_stats.dlss_active_mode),
                frame_stats.dlss_internal_width,
                frame_stats.dlss_internal_height,
                frame_stats.dlss_output_width, frame_stats.dlss_output_height,
                static_cast<unsigned long long>(frame_stats.dlss_reset_count),
                frame_stats.dlss_reason.empty() ? "none"
                                                : frame_stats.dlss_reason.c_str());
            reported_selected_dlss_mode = frame_stats.dlss_selected_mode;
            reported_active_dlss_mode = frame_stats.dlss_active_mode;
            reported_dlss_internal_width = frame_stats.dlss_internal_width;
            reported_dlss_internal_height = frame_stats.dlss_internal_height;
            reported_dlss_output_width = frame_stats.dlss_output_width;
            reported_dlss_output_height = frame_stats.dlss_output_height;
            reported_dlss_resets = frame_stats.dlss_reset_count;
        }
        const bool vk_rt_observation_valid =
            frame_stats.vk_rt_effective ||
            !frame_stats.vk_rt_fallback_reason.empty();
        if (vk_rt_observation_valid &&
            (!reported_vk_rt_once ||
             reported_vk_rt_effective != frame_stats.vk_rt_effective ||
             reported_vk_rt_dispatches != frame_stats.vk_rt_trace_dispatches ||
             reported_vk_rt_reason != frame_stats.vk_rt_fallback_reason)) {
            std::printf(
                "Vulkan RT observed effective=%s dispatches=%u reason=%s\n",
                frame_stats.vk_rt_effective ? "true" : "false",
                frame_stats.vk_rt_trace_dispatches,
                frame_stats.vk_rt_fallback_reason.empty()
                    ? "none"
                    : frame_stats.vk_rt_fallback_reason.c_str());
            reported_vk_rt_effective = frame_stats.vk_rt_effective;
            reported_vk_rt_dispatches = frame_stats.vk_rt_trace_dispatches;
            reported_vk_rt_reason = frame_stats.vk_rt_fallback_reason;
            reported_vk_rt_once = true;
        }
        stats.frame_ms = std::chrono::duration<float, std::milli>(
                             std::chrono::steady_clock::now() - render_start).count();
        stats.fps = stats.frame_ms > 0.0f ? 1000.0f / stats.frame_ms : 0.0f;
        stats.cam_pos[0] = camera.position.x;
        stats.cam_pos[1] = camera.position.y;
        stats.cam_pos[2] = camera.position.z;
        stats.resolve_ms = frame_stats.resolve_ms;
        stats.build_ms = frame_stats.build_ms;
        stats.draw_ms = frame_stats.draw_ms;
        stats.instances_active = static_cast<int>(frame_stats.instances_resolved);
        stats.gpu_emitted = static_cast<int>(frame_stats.instances_drawn);
        stats.gpu_culled = static_cast<int>(frame_stats.clusters_culled);
        stats.gpu_culled_hiz = static_cast<int>(frame_stats.hiz_culled);
        stats.culled_clusters = stats.gpu_culled;
        stats.raster_tris = static_cast<int>(frame_stats.triangles);
        stats.instances_total = static_cast<int>(frame_stats.instances_total);
        stats.parts_baked = static_cast<int>(frame_stats.parts_baked);
        stats.cache_hits = static_cast<int>(frame_stats.cache_hits);
        std::memcpy(stats.probe_dims, frame_stats.probe_dims,
                    sizeof(stats.probe_dims));

        const bool ui_frame_ready = ui.begin_frame(frame, error);
        if (!ui_frame_ready) {
            std::fprintf(stderr, "FATAL: ImGui Vulkan prepare: %s\n",
                         error.c_str());
            fatal_error = true;
        } else if (!hide_ui) {
            ui.draw_debug_panel(stats);
            ui.draw_worlds_panel(worlds, stats);
            ui.draw_camera_panel(camera);
        }
        if (ui_frame_ready && !ui.end_frame(frame, error)) {
            std::fprintf(stderr, "FATAL: ImGui Vulkan backend: %s\n", error.c_str());
            fatal_error = true;
        }

        bool capture = false;
        std::string capture_path;
        if (!screenshot_path.empty() && bake_ready && frame_stats.instances_drawn > 0 &&
            ++screenshot_settle >= 3) {
            capture = true; capture_path = screenshot_path;
        } else if (shot_settle > 0 && frame_stats.instances_drawn > 0 &&
                   --shot_settle == 0) {
            capture = true; capture_path = shot_path;
        }
        std::vector<uint8_t> rgba;
        if (capture && !session->readback_swapchain_rgba8(frame, rgba, error)) {
            ++screenshot_failures;
            std::fprintf(stderr, "screenshot readback retry %d/5: %s\n",
                         screenshot_failures, error.c_str());
            capture = false;
            if (capture_path == screenshot_path) screenshot_settle = 1;
            else shot_settle = 2;
            if (screenshot_failures >= 5) {
                std::fprintf(stderr, "FATAL: screenshot readback exhausted retries\n");
                fatal_error = true;
            }
        }
        bool frame_presented = false;
        const bool frame_completed =
            vulkan->end_frame(frame, frame_presented, error);
        session->finish_vulkan_frame(
            frame.serial, frame_presented && !fatal_error);
        // end_frame() records the queue submit and present boundary. Keep this
        // separate from stats.frame_ms, which intentionally remains the local
        // CPU render-recording time shown by the interactive HUD.
        const double perf_frame_cadence_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - perf_frame_start).count();
        if (!frame_completed) {
            std::fprintf(stderr, "FATAL: end_frame: %s\n", error.c_str());
            fatal_error = true;
        } else if (capture) {
            if (!write_png(capture_path, rgba, frame.extent.width,
                           frame.extent.height)) {
                std::fprintf(stderr, "screenshot FAILED %s\n", capture_path.c_str());
                fatal_error = true;
            } else {
                screenshot_failures = 0;
                std::printf("screenshot written to %s\n", capture_path.c_str());
#ifndef _WIN32
                if (capture_path == shot_path) {
                    const std::string done = shot_path + ".done";
                    if (FILE* file = std::fopen(done.c_str(), "w")) std::fclose(file);
                }
#endif
                if (capture_path == screenshot_path) quit_requested = true;
            }
        }

        if (perf.enabled && perf_phase != PerfPhase::Complete && !fatal_error) {
            const auto perf_now = std::chrono::steady_clock::now();
            if (perf_phase == PerfPhase::WaitingForBake) {
                if (bake_ready && frame_stats.instances_drawn > 0) {
                    perf_phase = PerfPhase::Warming;
                    perf_phase_start = perf_now;
                    std::printf("perf: bake ready; warming for %.3f seconds\n",
                                perf.warmup_seconds);
                }
            } else if (perf_phase == PerfPhase::Warming &&
                       std::chrono::duration<double>(perf_now - perf_phase_start)
                               .count() >= perf.warmup_seconds) {
                perf_phase = PerfPhase::Sampling;
                perf_phase_start = perf_now;
                perf_start_counters = capture_perf_counters(frame_stats);
                perf_start_dlss_resets = frame_stats.dlss_reset_count;
                perf_frame_times.clear();
                std::printf("perf: sampling for %.3f seconds\n",
                            perf.sample_seconds);
            } else if (perf_phase == PerfPhase::Sampling) {
                perf_frame_times.push_back(perf_frame_cadence_ms);
                if (std::chrono::duration<double>(perf_now - perf_phase_start)
                        .count() >= perf.sample_seconds) {
                    const PerfCounters perf_finish_counters =
                        capture_perf_counters(frame_stats);
                    const uint32_t validation_errors =
                        vulkan->validation_error_count();
                    if (!write_perf_result(
                            perf, worlds[stats.world_current].world_name,
                            perf_frame_times, perf_start_counters,
                            perf_finish_counters, frame_stats,
                            perf_start_dlss_resets,
                            validation_errors, perf_error)) {
                        std::fprintf(stderr, "FATAL: perf: %s\n",
                                     perf_error.c_str());
                        fatal_error = true;
                    } else if (validation_errors != 0) {
                        std::fprintf(stderr,
                                     "FATAL: perf observed %u Vulkan validation errors\n",
                                     validation_errors);
                        fatal_error = true;
                    } else {
                        std::printf("perf: wrote %zu frames to %s\n",
                                    perf_frame_times.size(),
                                    perf.output_path.c_str());
                        quit_requested = true;
                    }
                    perf_phase = PerfPhase::Complete;
                }
            }
        }

        if (!stats_label.empty()) {
            std::printf("STATS,%s,%.2f,%.2f,%.2f,%.2f,%d,%d,%d,%d,%d\n",
                        stats_label.c_str(), stats.frame_ms, stats.resolve_ms,
                        stats.build_ms, stats.draw_ms, stats.instances_active,
                        stats.raster_batches, stats.raster_tris,
                        stats.culled_clusters, stats.gpu_culled_hiz);
            stats_label.clear();
        }
        if (stats.reload_requested) {
            stats.reload_requested = false;
            bake_ready = false; screenshot_settle = 0;
            viewer::prepare_world_reload(stats);
            session->reload();
        }
        if (stats.world_switch_requested >= 0 &&
            stats.world_switch_requested < static_cast<int>(worlds.size())) {
            const int selected = stats.world_switch_requested;
            stats.world_switch_requested = -1;
            auto next_session = open_world(worlds[selected]);
            if (!next_session) {
                viewer::complete_world_switch(stats, false);
                continue;
            }
            session = std::move(next_session);
            viewer::complete_world_switch(stats, true);
            stats.world_current = selected;
            selected_world_reported = false;
            bake_ready = false; screenshot_settle = 0;
            apply_world_resolver_defaults(worlds[selected].world_name,
                                          active_radius,
                                          min_projected_size, stats);
        }
    }

#ifndef _WIN32
    if (cmd_fd >= 0) close(cmd_fd);
    if (fifo_path) unlink(fifo_path);
#endif
    if (camera_capture) camera_controller.set_capture(window, false);
#ifndef _WIN32
    if (cmd_fd >= 0) close(cmd_fd);
#else
    if (cmd_handle != INVALID_HANDLE_VALUE) CloseHandle(cmd_handle);
#endif
    session.reset();
    ui.shutdown();
    engine.reset();
    const uint32_t validation_errors = vulkan->validation_error_count();
    vulkan.reset();
    glfwDestroyWindow(window);
    glfwTerminate();
    if (validation_errors != 0) {
        std::fprintf(stderr, "FATAL: Vulkan validation errors: %u\n",
                     validation_errors);
        return 1;
    }
    return fatal_error ? 1 : 0;
}
