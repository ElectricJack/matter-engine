#include "ui.h"

#include <array>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <system_error>

#include "imgui.h"
#include "ImGuizmo.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"
#include "matter/vulkan_device.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace viewer {
namespace {

// streaming_state_name and published_streaming_owner were removed in Phase 4
// Task 12 along with draw_sector_streaming_panel — they had no callers once
// the panel was retired in favor of the Properties-panel specialized editor.

} // namespace

void reset_lighting_controls(ViewerStats& stats) {
    stats.lighting = matter::VulkanLightingOverrides{};
}

void prepare_world_reload(ViewerStats& stats) {
    reset_lighting_controls(stats);
}

void complete_world_switch(ViewerStats& stats, bool succeeded) {
    if (succeeded) reset_lighting_controls(stats);
}

std::vector<WorldEntry> scan_worlds(const std::string& examples_root) {
    namespace fs = std::filesystem;
    std::vector<WorldEntry> out;
    std::error_code ec;

    auto scan_project = [&](const fs::path& project) {
        std::error_code project_ec;
        const fs::path objects = project / "objects";
        const fs::path worlds = project / "worlds";
        if (!fs::is_directory(objects, project_ec) ||
            !fs::is_directory(worlds, project_ec)) return;
        for (auto wit = fs::directory_iterator(worlds, project_ec);
             !project_ec && wit != fs::directory_iterator();
             wit.increment(project_ec)) {
            const fs::path world_file = wit->path();
            if (!fs::is_regular_file(world_file, project_ec) ||
                world_file.extension() != ".js") continue;
            WorldEntry e;
            e.label = world_file.stem().string();
            e.project_dir = project.string();
            e.world_name = world_file.stem().string();
            out.push_back(std::move(e));
        }
    };

    const fs::path root(examples_root);
    scan_project(root);
    for (auto it = fs::directory_iterator(root, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (fs::is_directory(it->path(), ec)) scan_project(it->path());
    }

    std::sort(out.begin(), out.end(),
              [](const WorldEntry& a, const WorldEntry& b) { return a.label < b.label; });
    return out;
}

bool Ui::setup(GLFWwindow* window, matter::VulkanDevice& vulkan,
               std::string& error) {
    vulkan_ = &vulkan;
    image_count_ = vulkan.swapchain_image_count();
    const VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 128},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 128},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 128},
    };
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool.maxSets = 384;
    pool.poolSizeCount = 3;
    pool.pPoolSizes = pool_sizes;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    const VkResult pool_result = vkCreateDescriptorPool(
        vulkan.device(), &pool, nullptr, &descriptor_pool);
    if (pool_result != VK_SUCCESS) {
        error = "vkCreateDescriptorPool(ImGui) failed with VkResult " +
                std::to_string(static_cast<int>(pool_result));
        vulkan_ = nullptr;
        return false;
    }
    descriptor_pool_ = reinterpret_cast<std::uint64_t>(descriptor_pool);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    imgui_context_initialized_ = true;
    ImGui::StyleColorsDark();
    if (!ImGui_ImplGlfw_InitForVulkan(window, true)) {
        error = "ImGui GLFW Vulkan initialization failed";
        shutdown();
        return false;
    }
    glfw_backend_initialized_ = true;
    if (!initialize_vulkan_backend(vulkan.swapchain_format(), image_count_, error)) {
        shutdown();
        return false;
    }
    return true;
}

bool Ui::initialize_vulkan_backend(VkFormat color_format,
                                   std::uint32_t image_count,
                                   std::string& error) {
    if (!vulkan_ || descriptor_pool_ == 0 || image_count == 0 ||
        color_format == VK_FORMAT_UNDEFINED) {
        error = "invalid ImGui Vulkan backend configuration";
        return false;
    }
    ImGui_ImplVulkan_InitInfo init{};
    init.ApiVersion = VK_API_VERSION_1_3;
    init.Instance = vulkan_->instance();
    init.PhysicalDevice = vulkan_->physical_device();
    init.Device = vulkan_->device();
    init.QueueFamily = vulkan_->graphics_queue_family();
    init.Queue = vulkan_->graphics_queue();
    init.DescriptorPool = reinterpret_cast<VkDescriptorPool>(descriptor_pool_);
    init.MinImageCount = image_count;
    init.ImageCount = image_count;
    init.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init.UseDynamicRendering = true;
    init.PipelineRenderingCreateInfo = {
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    init.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    init.PipelineRenderingCreateInfo.pColorAttachmentFormats = &color_format;
    if (!ImGui_ImplVulkan_Init(&init)) {
        error = "ImGui Vulkan initialization failed";
        return false;
    }
    vulkan_backend_initialized_ = true;
    swapchain_format_ = color_format;
    image_count_ = image_count;
    return true;
}

void Ui::shutdown() {
    if (!vulkan_) return;
    vulkan_->wait_idle();
    if (vulkan_backend_initialized_) {
        ImGui_ImplVulkan_Shutdown();
        vulkan_backend_initialized_ = false;
    }
    if (glfw_backend_initialized_) {
        ImGui_ImplGlfw_Shutdown();
        glfw_backend_initialized_ = false;
    }
    if (imgui_context_initialized_) {
        ImGui::DestroyContext();
        imgui_context_initialized_ = false;
    }
    if (descriptor_pool_ != 0) {
        vkDestroyDescriptorPool(
            vulkan_->device(),
            reinterpret_cast<VkDescriptorPool>(descriptor_pool_), nullptr);
    }
    descriptor_pool_ = 0;
    image_count_ = 0;
    swapchain_format_ = VK_FORMAT_UNDEFINED;
    vulkan_ = nullptr;
}

bool Ui::prepare_vulkan_backend(const matter::VulkanFrame& frame,
                                std::string& error) {
    if (frame.swapchain_format != swapchain_format_) {
        // Pipeline and font texture descriptors belong to the Vulkan backend.
        // Tear them down before any draw data can reference them, while keeping
        // the ImGui context and GLFW input backend alive.
        vulkan_->wait_idle();
        if (vulkan_backend_initialized_) {
            ImGui_ImplVulkan_Shutdown();
            vulkan_backend_initialized_ = false;
        }
        return initialize_vulkan_backend(frame.swapchain_format,
                                         frame.image_count, error);
    }
    if (frame.swapchain_recreated || frame.image_count != image_count_) {
        image_count_ = frame.image_count;
        ImGui_ImplVulkan_SetMinImageCount(image_count_);
    }
    return true;
}

bool Ui::begin_frame(const matter::VulkanFrame& frame, std::string& error) {
    if (!prepare_vulkan_backend(frame, error)) return false;
    gizmo_submitted_ = false;
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();
    return true;
}

bool Ui::end_frame(const matter::VulkanFrame& frame, std::string& error) {
    (void)error;
    ImGui::Render();
    VkRenderingAttachmentInfo attachment{
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = frame.swapchain_image_view;
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = frame.extent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    vkCmdBeginRendering(frame.command_buffer, &rendering);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), frame.command_buffer);
    vkCmdEndRendering(frame.command_buffer);
    return true;
}

void Ui::draw_debug_panel(ViewerStats& s) {
    ImGui::Begin("Viewer Debug");

    ImGui::Text("FPS: %.1f  (%.2f ms)", s.fps, s.frame_ms);
    if (s.gpu_timers_supported) {
        const float sum = s.gpu_cull_ms + s.gpu_gbuffer_ms + s.gpu_blas_ms +
                          s.gpu_tlas_ms + s.gpu_rt_ms + s.gpu_denoise_ms +
                          s.gpu_dlss_ms + s.gpu_composite_ms;
        const float unaccounted = s.gpu_total_ms - sum;
        ImGui::Text("GPU %.1fms | Cull %.1f GBuf %.1f BLAS %.1f TLAS %.1f RT %.1f Den %.1f DLSS %.1f Comp %.1f (other %.1f)",
                    s.gpu_total_ms, s.gpu_cull_ms,
                    s.gpu_gbuffer_ms, s.gpu_blas_ms, s.gpu_tlas_ms, s.gpu_rt_ms,
                    s.gpu_denoise_ms, s.gpu_dlss_ms, s.gpu_composite_ms,
                    unaccounted);
    } else {
        ImGui::TextDisabled("GPU timers unavailable");
    }
    ImGui::Text("CPU: resolve %.2f  build %.2f  draw %.2f ms",
                s.resolve_ms, s.build_ms, s.draw_ms);
    ImGui::Text("Camera: %.1f, %.1f, %.1f", s.cam_pos[0], s.cam_pos[1], s.cam_pos[2]);
    ImGui::Separator();

    ImGui::Text("Instances: %d active / %d total", s.instances_active, s.instances_total);
    ImGui::Text("Occupied sectors: %d", s.occupied_sectors);
    ImGui::Separator();

    ImGui::Text("Provider: %s", s.connected ? "connected" : "disconnected");
    ImGui::Text("Last connect: %d baked, %d cache hits", s.parts_baked, s.cache_hits);
    ImGui::Text("Last reconcile want: %d", s.last_want_count);
    ImGui::Separator();

    const char* hit_tag = s.batch_cache_hit ? " [cached]" : "";
    ImGui::Text("Raster: %d batches / %d tris  culled: %d%s",
                s.raster_batches, s.raster_tris, s.culled_clusters, hit_tag);
    if (s.gpu_cull_active) {
        ImGui::Text("GPU cull: emitted %d  frustum %d  hiz %d",
                    s.gpu_emitted, s.gpu_culled, s.gpu_culled_hiz);
        ImGui::TextDisabled("HiZ occlusion: not available in Vulkan milestone");
        ImGui::TextDisabled("Wireframe: not available in Vulkan milestone");
        ImGui::TextDisabled("Render path: Vulkan raster only");
    }
    ImGui::Separator();

    ImGui::SliderFloat("Pixel budget", &s.pixel_budget, 0.1f, 2.0f, "%.2f");
    const char* resolvers[] = { "PassThrough", "SectorLod" };
    ImGui::Combo("Resolver", &s.resolver_choice, resolvers, 2);
    if (ImGui::Button("Reload world")) s.reload_requested = true;

    ImGui::SeparatorText("Lighting");
    ImGui::SliderFloat("Exposure (EV)", &s.lighting.exposure_ev, -6.0f, 6.0f,
                       "%.2f");
    ImGui::SliderFloat("Sun", &s.lighting.sun_multiplier, 0.0f, 4.0f,
                       "%.2f");
    ImGui::SliderFloat("Sky", &s.lighting.sky_multiplier, 0.0f, 4.0f,
                       "%.2f");
    ImGui::SliderFloat("Emission", &s.lighting.emission_multiplier, 0.0f,
                       4.0f, "%.2f");
    if (ImGui::Button("Reset to World")) reset_lighting_controls(s);

    ImGui::End();
}

void Ui::draw_camera_panel(matter::CameraDesc& cam) {
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 270.0f, 20.0f),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(250, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("Camera");

    ImGui::DragFloat3("Position", &cam.position.x, 0.1f);
    ImGui::DragFloat3("Target", &cam.target.x, 0.1f);

    float dx = cam.position.x - cam.target.x;
    float dy = cam.position.y - cam.target.y;
    float dz = cam.position.z - cam.target.z;
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    if (dist < 0.0001f) dist = 0.0001f;
    float yaw = atan2f(dz, dx);
    float pitch = asinf(dy / dist);
    bool changed = false;
    const float orbit_step = 0.04f; // radians per repeat tick

    ImGui::PushButtonRepeat(true);
    ImGui::Text("Orbit:");
    if (ImGui::Button("Left"))  { yaw -= orbit_step; changed = true; }
    ImGui::SameLine();
    if (ImGui::Button("Right")) { yaw += orbit_step; changed = true; }
    ImGui::SameLine();
    if (ImGui::Button("Up"))    { pitch += orbit_step; changed = true; }
    ImGui::SameLine();
    if (ImGui::Button("Down"))  { pitch -= orbit_step; changed = true; }

    if (ImGui::Button("Zoom In"))  { dist *= 0.96f; changed = true; }
    ImGui::SameLine();
    if (ImGui::Button("Zoom Out")) { dist *= 1.04f; changed = true; }
    ImGui::PopButtonRepeat();

    if (ImGui::SliderFloat("Distance", &dist, 1.0f, 150.0f)) changed = true;

    // Clamp pitch just shy of the poles so the orbit never flips/gimbal-locks.
    const float pitch_limit = 1.5533f; // ~89 degrees
    if (pitch > pitch_limit) pitch = pitch_limit;
    if (pitch < -pitch_limit) pitch = -pitch_limit;
    if (dist < 1.0f) dist = 1.0f;

    if (changed) {
        cam.position.x = cam.target.x + dist * cosf(pitch) * cosf(yaw);
        cam.position.y = cam.target.y + dist * sinf(pitch);
        cam.position.z = cam.target.z + dist * cosf(pitch) * sinf(yaw);
    }

    if (ImGui::Button("Reset View")) {
        cam.position = {20.0f, 16.0f, 34.0f};
        cam.target   = {0.0f, 9.0f, 0.0f};
        cam.up       = {0.0f, 1.0f, 0.0f};
    }

    ImGui::Separator();
    ImGui::TextColored(ImVec4(1, 1, 0, 1), "TAB: free-fly (WASD + mouse)");

    ImGui::End();
}

void Ui::draw_worlds_panel(const std::vector<WorldEntry>& worlds, ViewerStats& stats) {
    ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(200, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("Worlds");

    for (int i = 0; i < (int)worlds.size(); ++i) {
        const bool is_current = (i == stats.world_current);
        if (is_current) ImGui::BeginDisabled(true);
        if (ImGui::Button(worlds[i].label.c_str())) {
            stats.world_switch_requested = i;
        }
        if (is_current) ImGui::EndDisabled();
    }

    ImGui::End();
}

void Ui::update_sector_streaming(matter::WorldSession& session,
                                 const matter::CameraDesc& camera) {
    flecs::world& world = session.ecs();
    const flecs::entity_t selected_before = streaming_anchor_.selected;
    matter_viewer::validate_anchor(streaming_anchor_, world);
    if (selected_before != 0 && streaming_anchor_.selected == 0) {
        anchor_id_input_ = 0;
    }
    const float camera_position[3] = {
        camera.position.x, camera.position.y, camera.position.z};
    matter_viewer::follow_camera(streaming_anchor_, world, camera_position);
}

// draw_sector_streaming_panel retired in Phase 4 Task 12 — sector streaming
// editing moved into the Properties panel via SpecializedEditors
// (MatterViewer/specialized_editors.h). update_sector_streaming above (the
// per-frame anchor/follow logic, not UI) is unaffected.

bool Ui::camera_input_allowed() const {
    if (ImGui::GetCurrentContext() == nullptr) return true;
    const ImGuiIO& io = ImGui::GetIO();
    const bool gizmo_over =
        gizmo_submitted_ && ImGuizmo::IsOver(ImGuizmo::TRANSLATE);
    return matter_viewer::camera_input_allowed(
        io.WantCaptureMouse, io.WantCaptureKeyboard,
        gizmo_over, ImGuizmo::IsUsing());
}

} // namespace viewer
