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
#include <memory>
#include <string>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
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

} // namespace

int main() {
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
    matter::EngineDesc engine_desc;
    engine_desc.cache_root = "cache";
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
        for (size_t i = 0; i < worlds.size(); ++i) {
            std::string candidate = worlds[i].world_name;
            std::transform(candidate.begin(), candidate.end(), candidate.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (candidate == wanted) { initial_world = static_cast<int>(i); break; }
        }
    }

    viewer::ViewerStats stats{};
    stats.world_current = initial_world;
    stats.gpu_cull_active = true;
    stats.connected = true;
    if (const char* value = std::getenv("MATTER_HIZ"))
        stats.hiz_enabled = value[0] != '0';
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
    viewer::CameraController camera_controller;
    const char* screenshot_env = std::getenv("MATTER_SCREENSHOT");
    const std::string screenshot_path = screenshot_env ? screenshot_env : "";
    int screenshot_settle = 0;
    bool bake_ready = false;
    const bool test_resize = std::getenv("MATTER_TEST_RESIZE") != nullptr;
    bool resize_exercised = false;

    int cmd_fd = -1;
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
    if (fifo_path)
        std::printf("MATTER_CMD_FIFO not supported on Windows; ignoring\n");
#endif
    std::string shot_path;
    std::string stats_label;
    int shot_settle = 0;
    bool quit_requested = false;
    bool fatal_error = false;
    auto previous_time = std::chrono::steady_clock::now();

    while (!glfwWindowShouldClose(window) && !quit_requested && !fatal_error) {
        glfwPollEvents();
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - previous_time).count();
        previous_time = now;
        if (key_pressed(window, GLFW_KEY_TAB, tab_down)) {
            camera_capture = !camera_capture;
            camera_controller.set_capture(window, camera_capture);
        }
        if (key_pressed(window, GLFW_KEY_F9, f9_down)) wireframe = !wireframe;
        camera_controller.update(window, dt, camera);

#ifndef _WIN32
        if (cmd_fd >= 0) {
            char bytes[512];
            ssize_t count = 0;
            while ((count = read(cmd_fd, bytes, sizeof(bytes))) > 0)
                cmd_buffer.append(bytes, static_cast<size_t>(count));
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
                    stats.hiz_enabled = std::strcmp(word, "on") == 0;
                } else if (line == "reload") {
                    stats.reload_requested = true;
                } else if (line == "wireframe" || line == "wireframe toggle") {
                    wireframe = !wireframe;
                } else if (line == "wireframe on" || line == "wireframe off") {
                    wireframe = line == "wireframe on";
                } else if (line == "quit") {
                    quit_requested = true;
                } else if (!line.empty()) {
                    std::printf("cmd: unrecognized '%s'\n", line.c_str());
                }
            }
        }
#endif

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
        options.wireframe = wireframe;
        options.hiz_occlusion = stats.hiz_enabled;
        options.pixel_budget = stats.pixel_budget;
        options.active_radius = active_radius;
        options.min_projected_size = min_projected_size;
        const auto render_start = std::chrono::steady_clock::now();
        if (!session->render(camera, frame, options, error)) {
            std::fprintf(stderr, "FATAL: render: %s\n", error.c_str());
            fatal_error = true;
        }
        const matter::FrameStats& frame_stats = session->frame_stats();
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

        ui.begin_frame();
        ui.draw_debug_panel(stats);
        ui.draw_worlds_panel(worlds, stats);
        ui.draw_camera_panel(camera);
        ui.draw_lighting_panel(stats);
        ui.end_frame(frame);

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
            std::fprintf(stderr, "FATAL: screenshot readback: %s\n", error.c_str());
            fatal_error = true;
        }
        if (!vulkan->end_frame(frame, error)) {
            std::fprintf(stderr, "FATAL: end_frame: %s\n", error.c_str());
            fatal_error = true;
        } else if (capture) {
            if (!write_png(capture_path, rgba, frame.extent.width,
                           frame.extent.height)) {
                std::fprintf(stderr, "screenshot FAILED %s\n", capture_path.c_str());
                fatal_error = true;
            } else {
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
            session->reload();
        }
        if (stats.world_switch_requested >= 0 &&
            stats.world_switch_requested < static_cast<int>(worlds.size())) {
            const int selected = stats.world_switch_requested;
            stats.world_switch_requested = -1;
            session.reset();
            session = open_world(worlds[selected]);
            if (!session) { fatal_error = true; continue; }
            stats.world_current = selected;
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
