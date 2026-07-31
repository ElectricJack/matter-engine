#include "ui.h"

#include <array>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <system_error>

#include "imgui.h"
#include "imgui_internal.h"
#include "ImGuizmo.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"
#include "matter/vulkan_device.h"
#include "editor_props.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace viewer {
namespace {

// streaming_state_name and published_streaming_owner were removed in Phase 4
// Task 12 along with draw_sector_streaming_panel — they had no callers once
// the panel was retired in favor of the Properties-panel specialized editor.

} // namespace

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
    // Dear ImGui's Vulkan backend (1.92+) allocates separate SAMPLED_IMAGE and
    // SAMPLER descriptor sets from this pool (see imgui_impl_vulkan.cpp's
    // DescriptorSetLayoutTexture/DescriptorSetLayoutSampler); without those
    // pool sizes the validation layer warns on every vkAllocateDescriptorSets.
    const VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 128},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 128},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 128},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 128},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 16},
    };
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool.maxSets = 384;
    pool.poolSizeCount = 5;
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
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
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
    init.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init.UseDynamicRendering = true;
    init.PipelineInfoMain.PipelineRenderingCreateInfo = {
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    init.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    init.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &color_format;
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
    destroy_viewport_target();
    if (rt_sampler_) {
        vkDestroySampler(vulkan_->device(), rt_sampler_, nullptr);
        rt_sampler_ = VK_NULL_HANDLE;
    }
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
    rendered_to_viewport_target_ = false;
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();
    if (!hide_ui_) {
        build_dockspace();
    } else {
        const ImVec2 d = ImGui::GetIO().DisplaySize;
        viewport_rect_ = ViewportRect{0, 0, d.x, d.y};
    }
    return true;
}

bool Ui::end_frame(const matter::VulkanFrame& frame, std::string& error) {
    (void)error;
    ImGui::Render();
    VkRenderingAttachmentInfo attachment{
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = frame.swapchain_image_view;
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    // CLEAR only when the 3D went OFFSCREEN this frame -- then the swapchain
    // holds nothing worth keeping and ImGui repaints it, viewport image and
    // all. When the 3D rendered straight to the swapchain (UI hidden, or the
    // viewport target failed to allocate) it must LOAD, or ImGui's pass wipes
    // the frame that was just drawn.
    //
    // This used to key on has_viewport_target(), which asks whether the target
    // OBJECT exists rather than whether it was USED. The two only agreed
    // because the UI-hidden path was reachable only via MATTER_HIDE_UI at
    // startup, where no target had ever been created. F11 toggles it at
    // runtime, leaving a live rt_image_ from before the toggle: the 3D went to
    // the swapchain, the stale target still said CLEAR, and presentation mode
    // rendered black.
    attachment.loadOp = rendered_to_viewport_target_
                            ? VK_ATTACHMENT_LOAD_OP_CLEAR
                            : VK_ATTACHMENT_LOAD_OP_LOAD;
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

namespace {
constexpr float kToolbarHeight = 40.0f;
constexpr float kSceneWidthFrac = 0.22f;
constexpr float kPropertiesWidthFrac = 0.26f;
constexpr float kConsoleHeightFrac = 0.20f;
} // namespace

void Ui::build_dockspace() {
    const ImVec2 display = ImGui::GetIO().DisplaySize;

    ImGui::SetNextWindowPos(ImVec2(0, kToolbarHeight));
    ImGui::SetNextWindowSize(ImVec2(display.x, display.y - kToolbarHeight));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##DockHost", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoNavFocus |
                     ImGuiWindowFlags_NoDocking);
    ImGui::PopStyleVar();

    const ImGuiID dockspace_id = ImGui::GetID("MainDockSpace");

    if (ImGui::DockBuilderGetNode(dockspace_id) == nullptr) {
        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id,
                                  ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id,
                                      ImVec2(display.x,
                                             display.y - kToolbarHeight));

        ImGuiID center = dockspace_id;
        const ImGuiID left = ImGui::DockBuilderSplitNode(
            center, ImGuiDir_Left, kSceneWidthFrac, nullptr, &center);
        const ImGuiID right = ImGui::DockBuilderSplitNode(
            center, ImGuiDir_Right,
            kPropertiesWidthFrac / (1.0f - kSceneWidthFrac), nullptr,
            &center);
        const ImGuiID bottom = ImGui::DockBuilderSplitNode(
            center, ImGuiDir_Down, kConsoleHeightFrac, nullptr, &center);

        ImGui::DockBuilderDockWindow("Scene", left);
        ImGui::DockBuilderDockWindow("Properties", right);
        ImGui::DockBuilderDockWindow("Console", bottom);
        ImGui::DockBuilderDockWindow("Viewport", center);
        ImGui::DockBuilderDockWindow("Viewer Debug", right);
        ImGui::DockBuilderDockWindow("Camera", right);
        // Tabbed with Properties: all three are property editors — one over
        // the selection, one over the registered tunable groups, and the two
        // curated cuts of that registry (lighting/volumetrics, and everything
        // that trades quality for frame time).
        ImGui::DockBuilderDockWindow("Tunables", right);
        ImGui::DockBuilderDockWindow("Lighting", right);
        // "Performance" replaced the former "LOD Settings" window, which was
        // never docked here at all (it opened floating). Docking it now only
        // affects layouts built from scratch; an imgui.ini written before the
        // rename keeps whatever position it had for the old name and opens
        // this one floating.
        ImGui::DockBuilderDockWindow("Performance", right);
        ImGui::DockBuilderDockWindow("Bake Lab", bottom);
        // Promoted out of Bake Lab's former "Assets" tab (now a standalone
        // pane usable outside Bake Lab too). Docked into the same left node
        // as Scene, tabbed alongside it, since both are project/hierarchy
        // browsers over the current world(s).
        ImGui::DockBuilderDockWindow("Assets", left);

        ImGui::DockBuilderFinish(dockspace_id);
    }

    ImGui::DockSpace(dockspace_id, ImVec2(0, 0),
                     ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::End();

    (void)ImGui::DockBuilderGetCentralNode(dockspace_id);
}

void Ui::prepare_viewport_rect() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Viewport", nullptr,
                 ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    viewport_hovered_ = ImGui::IsWindowHovered();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float sx = std::floor(pos.x);
    const float sy = std::floor(pos.y);
    const float sw = std::floor(avail.x > 0 ? avail.x : 0);
    const float sh = std::floor(avail.y > 0 ? avail.y : 0);
    viewport_rect_ = ViewportRect{sx, sy, sw, sh};
    ImGuizmo::SetAlternativeWindow(ImGui::GetCurrentWindow());
    ImGui::End();
}

void Ui::draw_viewport_window() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Viewport", nullptr,
                 ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (rt_descriptor_ && viewport_rect_.w > 0 && viewport_rect_.h > 0) {
        ImGui::SetCursorScreenPos(
            ImVec2(viewport_rect_.x, viewport_rect_.y));
        ImGui::Image(rt_descriptor_,
                     ImVec2(viewport_rect_.w, viewport_rect_.h));
    }
    ImGui::End();
}

bool Ui::ensure_viewport_target(uint32_t width, uint32_t height,
                                VkFormat format, std::string& error) {
    if (rt_image_ && rt_width_ == width && rt_height_ == height) {
        pending_rt_frames_ = 0;
        return true;
    }
    if (rt_image_) {
        if (width != pending_rt_w_ || height != pending_rt_h_) {
            pending_rt_w_ = width;
            pending_rt_h_ = height;
            pending_rt_frames_ = 1;
            return true;
        }
        if (++pending_rt_frames_ < 4) return true;
    }
    destroy_viewport_target();
    if (width == 0 || height == 0) return true;
    const VkDevice device = vulkan_->device();

    VkImageCreateInfo img{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    img.imageType = VK_IMAGE_TYPE_2D;
    img.format = format;
    img.extent = {width, height, 1};
    img.mipLevels = 1;
    img.arrayLayers = 1;
    img.samples = VK_SAMPLE_COUNT_1_BIT;
    img.tiling = VK_IMAGE_TILING_OPTIMAL;
    img.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    img.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &img, nullptr, &rt_image_) != VK_SUCCESS) {
        error = "failed to create viewport render target image";
        return false;
    }

    VkMemoryRequirements mem_req;
    vkGetImageMemoryRequirements(device, rt_image_, &mem_req);
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(vulkan_->physical_device(), &mem_props);
    uint32_t mem_type = UINT32_MAX;
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((mem_req.memoryTypeBits & (1u << i)) &&
            (mem_props.memoryTypes[i].propertyFlags &
             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            mem_type = i;
            break;
        }
    }
    if (mem_type == UINT32_MAX) {
        error = "no suitable memory type for viewport render target";
        destroy_viewport_target();
        return false;
    }

    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = mem_req.size;
    alloc.memoryTypeIndex = mem_type;
    if (vkAllocateMemory(device, &alloc, nullptr, &rt_memory_) != VK_SUCCESS) {
        error = "failed to allocate viewport render target memory";
        destroy_viewport_target();
        return false;
    }
    vkBindImageMemory(device, rt_image_, rt_memory_, 0);

    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = rt_image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device, &view_info, nullptr, &rt_view_) != VK_SUCCESS) {
        error = "failed to create viewport render target image view";
        destroy_viewport_target();
        return false;
    }

    if (!rt_sampler_) {
        VkSamplerCreateInfo samp{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samp.magFilter = VK_FILTER_LINEAR;
        samp.minFilter = VK_FILTER_LINEAR;
        samp.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samp.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(device, &samp, nullptr, &rt_sampler_) !=
            VK_SUCCESS) {
            error = "failed to create viewport sampler";
            destroy_viewport_target();
            return false;
        }
    }

    rt_descriptor_ = ImGui_ImplVulkan_AddTexture(
        rt_sampler_, rt_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!rt_descriptor_) {
        error = "failed to register viewport texture with ImGui";
        destroy_viewport_target();
        return false;
    }

    rt_width_ = width;
    rt_height_ = height;
    return true;
}

void Ui::destroy_viewport_target() {
    if (!vulkan_) return;
    const VkDevice device = vulkan_->device();
    vulkan_->wait_idle();
    if (rt_descriptor_) {
        ImGui_ImplVulkan_RemoveTexture(rt_descriptor_);
        rt_descriptor_ = VK_NULL_HANDLE;
    }
    if (rt_view_) { vkDestroyImageView(device, rt_view_, nullptr); rt_view_ = VK_NULL_HANDLE; }
    if (rt_image_) { vkDestroyImage(device, rt_image_, nullptr); rt_image_ = VK_NULL_HANDLE; }
    if (rt_memory_) { vkFreeMemory(device, rt_memory_, nullptr); rt_memory_ = VK_NULL_HANDLE; }
    rt_width_ = rt_height_ = 0;
}

matter::VulkanFrame Ui::viewport_render_frame(const matter::VulkanFrame& frame,
                                               std::string& error) {
    const uint32_t w = static_cast<uint32_t>(viewport_rect_.w);
    const uint32_t h = static_cast<uint32_t>(viewport_rect_.h);
    if (w == 0 || h == 0 || hide_ui_) return frame;
    if (!ensure_viewport_target(w, h, frame.swapchain_format, error))
        return frame;

    VkImageMemoryBarrier2 to_color{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    to_color.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    to_color.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    to_color.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    to_color.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                             VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    to_color.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_color.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_color.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_color.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_color.image = rt_image_;
    to_color.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &to_color;
    vkCmdPipelineBarrier2(frame.command_buffer, &dep);

    matter::VulkanFrame vp_frame = frame;
    vp_frame.swapchain_image = rt_image_;
    vp_frame.swapchain_image_view = rt_view_;
    vp_frame.extent = {rt_width_, rt_height_};
    // Only this path actually redirects the 3D pass offscreen. Both early
    // returns above hand back the swapchain frame, and end_frame has to be
    // able to tell the difference.
    rendered_to_viewport_target_ = true;
    return vp_frame;
}

void Ui::transition_viewport_for_sampling(VkCommandBuffer cmd) {
    if (rt_image_ == VK_NULL_HANDLE) return;
    // The barrier below declares oldLayout = COLOR_ATTACHMENT_OPTIMAL, which is
    // only true if the 3D pass actually rendered into this target this frame.
    // In presentation mode (F11) a stale target survives the toggle while the
    // 3D goes straight to the swapchain, so the image is still
    // SHADER_READ_ONLY_OPTIMAL from the last UI frame and transitioning it
    // would be a layout mismatch. main calls this unconditionally every frame,
    // so the guard belongs here.
    if (!rendered_to_viewport_target_) return;
    VkImageMemoryBarrier2 to_read{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    to_read.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    to_read.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    to_read.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    to_read.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    to_read.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_read.image = rt_image_;
    to_read.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &to_read;
    vkCmdPipelineBarrier2(cmd, &dep);
}

ToolbarActions Ui::draw_toolbar(matter::scene::SimulationMode mode) {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(display.x, kToolbarHeight), ImGuiCond_Always);
    ImGui::Begin("Toolbar", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoScrollbar);
    const ToolbarActions actions = draw_toolbar_contents(toolbar_state_, mode);
    ImGui::End();
    draw_viewport_border_tint(mode, viewport_rect_.x, viewport_rect_.y,
                              viewport_rect_.w, viewport_rect_.h);
    return actions;
}

void Ui::draw_scene_panel(EditorModel& editor, matter::WorldSession* session,
                          SceneCommands* commands, matter::scene::SimulationMode mode,
                          matter::CameraDesc* camera, SelectionSet* selection,
                          const FieldCommands* fields, ConsoleLog* console_log,
                          const std::unordered_set<uint64_t>* authored_entity_ids) {
    ImGui::Begin("Scene");
    draw_scene_tree(scene_tree_state_, editor, session, commands, mode, camera,
                    selection, fields, console_log, authored_entity_ids);
    ImGui::End();
}

void Ui::draw_properties_panel(const SelectionSet& selection, EditorModel& editor,
                               const PropertiesRegistry& registry,
                               const FieldCommands& fields,
                               const ComponentCommands& components,
                               matter::scene::SimulationMode mode,
                               const part_graph_snapshot::Snapshot* snapshot,
                               SpecializedEditors& specialized,
                               const matter::Float3& camera_position) {
    ImGui::Begin("Properties");
    draw_properties_contents(properties_state_, selection, editor, registry,
                             fields, components, mode, snapshot,
                             specialized, camera_position);
    ImGui::End();
}

void Ui::draw_console_panel(ConsoleLog& log, EditorProps& props) {
    ImGui::Begin("Console");
    // console.filters is edited directly through the ConsolePanelState&
    // reference (draw_console_contents), never through draw_group — but it
    // is the exact same bound struct, so Tunables must still treat this as
    // the group's home. See EditorProps::panel_home.
    if (matter::props::Binding* b = props.console())
        props.note_panel_home(b->schema().path, "Console");
    draw_console_contents(console_state_, log);
    ImGui::End();
}

void Ui::draw_tunables_panel(EditorProps& props) {
    ImGui::Begin("Tunables");
    draw_tunables_contents(tunables_state_, props);
    ImGui::End();
}

void Ui::draw_lighting_panel(EditorProps& props) {
    ImGui::Begin("Lighting");
    draw_lighting_contents(props);
    ImGui::End();
}

void Ui::reset_scene_tree_cache() {
    reset_scene_tree_graph_cache(scene_tree_state_);
}

void Ui::select_baked_root(uint64_t resolved_hash) {
    scene_tree_state_.selected_root_hash = resolved_hash;
}

matter::WorldSession::StreamingLodConfig streaming_config_from(
    const StreamingLodPrefs& prefs) {
    matter::WorldSession::StreamingLodConfig out;
    out.terrain_lod_enabled = prefs.terrain_lod_enabled;
    for (const LodRing& r : parse_lod_rings(prefs.scatter_rings))
        out.scatter_rings.push_back({r.radius, r.value});
    for (const LodRing& r : parse_lod_rings(prefs.terrain_bands))
        out.terrain_bands.push_back({r.radius, r.value});
    out.hysteresis = prefs.hysteresis;
    out.max_inflight = prefs.max_inflight;
    out.fail_cooldown_updates = prefs.fail_cooldown_updates;
    return out;
}

namespace {

// Ring lists are persisted as ONE string field each (see
// streaming_lod_prefs.h): sanitize (drop non-positive radii, sort ascending)
// on the way out so a dragged-past-its-neighbour handle can never produce a
// file the streamer would reject.
std::string encode_rings(
    std::vector<matter::WorldSession::StreamingLodRing> rows) {
    rows.erase(std::remove_if(rows.begin(), rows.end(),
                              [](const auto& r) { return !(r.radius > 0.0f); }),
               rows.end());
    std::stable_sort(rows.begin(), rows.end(),
                     [](const auto& a, const auto& b) { return a.radius < b.radius; });
    std::vector<LodRing> plain;
    plain.reserve(rows.size());
    for (const auto& r : rows) plain.push_back({r.radius, r.value});
    return format_lod_rings(plain);
}

std::vector<matter::WorldSession::StreamingLodRing> decode_rings(
    const std::string& text) {
    std::vector<matter::WorldSession::StreamingLodRing> out;
    for (const LodRing& r : parse_lod_rings(text)) out.push_back({r.radius, r.value});
    return out;
}

}  // namespace

void Ui::draw_performance_panel(matter::WorldSession* session,
                                EditorProps& props,
                                const ViewerCommands& commands,
                                const matter::CameraDesc& camera) {
    // ---- Ownership declarations: UNCONDITIONAL, before Begin() ------------
    //
    // Every group this panel owns, claimed up front — deliberately BEFORE the
    // `if (!ImGui::Begin(...))` gate below, not interleaved with the
    // draw_group() calls the way a first attempt at this put them.
    //
    // Why it has to be up here: Performance is docked in the SAME tab group
    // as Tunables (Properties/Viewer Debug/Performance/Lighting/Tunables all
    // share one dock node — see the layout comment below). ImGui::Begin()
    // returns FALSE for a docked window that is not the currently SELECTED
    // tab in its node, which is exactly the case whenever the user is
    // actually looking at Tunables — the scenario this whole issue is about.
    // A note_panel_home call gated behind that Begin() would then never fire
    // while Tunables is the visible tab, so the checkbox would silently stop
    // hiding Performance's groups the moment it was needed, which was caught
    // by replaying issue dd98763c's own shot with the Tunables tab forced
    // into focus: every Performance-owned group reappeared even though the
    // checkbox was on. Declaring ownership here instead means it runs every
    // frame main.cpp calls this function — which is every frame, regardless
    // of which docked tab ImGui is currently showing — so the claim tracks
    // "does this panel exist and own this group" rather than "did this exact
    // pixel get painted this frame". draw_lighting_contents and
    // draw_debug_panel happen not to gate on Begin() at all, so they were
    // already immune to this; Performance is the one panel here that does.
    if (matter::props::Binding* b = props.budget())
        props.note_panel_home(b->schema().path, "Performance");
    if (matter::props::Binding* b = props.pom())
        props.note_panel_home(b->schema().path, "Performance");
    if (matter::props::Binding* b = props.vt_budgets())
        props.note_panel_home(b->schema().path, "Performance");
    if (matter::props::Binding* b = props.vt_enrich())
        props.note_panel_home(b->schema().path, "Performance");
    if (matter::props::Binding* b = props.stream_runtime())
        props.note_panel_home(b->schema().path, "Performance");
    if (matter::props::Binding* b = props.draw_overrides())
        props.note_panel_home(b->schema().path, "Performance");
    // camera.prefs and stream.lod are drawn by draw_streaming_lod_section
    // below, but claimed HERE rather than there for the same reason as
    // everything else on this list: that section runs after the Begin() gate
    // too.
    if (matter::props::Binding* b = props.camera())
        props.note_panel_home(b->schema().path, "Performance");
    if (matter::props::Binding* b = props.streaming())
        props.note_panel_home(b->schema().path, "Performance");

    // Renamed from "LOD Settings": an existing imgui.ini keyed on the old name
    // no longer matches, so this window opens floating for pre-existing
    // layouts and docked (right node, tabbed with Properties/Tunables) for
    // fresh ones. Group open/closed state is unaffected — that is keyed on
    // each group's ###path, which did not change.
    if (!ImGui::Begin("Performance")) {
        ImGui::End();
        return;
    }

    // ---- Live quality/throughput dials -----------------------------------
    // One binding each; ownership was already declared above. A panel
    // chooses WHERE a group appears, it does not own the widgets.
    if (matter::props::Binding* b = props.budget()) draw_group(*b);
    if (matter::props::Binding* b = props.pom()) draw_group(*b, nullptr, false);
    if (matter::props::Binding* b = props.vt_budgets())
        draw_group(*b, nullptr, false);
    // WS2: the tier-2 enrichment parameters sit next to the residency budgets
    // they are drained under, and the process-lifetime streaming knobs
    // (workers / prebuild, both ReadOnly) next to the streaming section below.
    if (matter::props::Binding* b = props.vt_enrich())
        draw_group(*b, nullptr, false);
    if (matter::props::Binding* b = props.stream_runtime())
        draw_group(*b, nullptr, false);

    // ---- Per-module draw overrides (view-time filter) --------------------
    // Sits with the other frame-time dials rather than in the streaming
    // section below: these apply immediately and never require a reload.
    // Header defaults OPEN now (issue editor-draw-overrides-panel-home,
    // absorbed into dd98763c): Tunables no longer shows Draw Overrides by
    // default, so a closed header here would have made the group
    // unreachable at a glance rather than merely one extra click away.
    if (matter::props::Binding* b = props.draw_overrides()) {
        if (ImGui::CollapsingHeader("Draw Overrides###draw.overrides",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SameLine();
            ImGui::TextDisabled("[World]%s", b->dirty() ? " *" : "");
            draw_draw_overrides_section(*b);
        }
    }

    draw_streaming_lod_section(session, props, commands, camera);
    ImGui::End();
}

void Ui::draw_streaming_lod_section(matter::WorldSession* session,
                                    EditorProps& props,
                                    const ViewerCommands& commands,
                                    const matter::CameraDesc& camera) {
    // ---- Live controls: applied every frame, no reload -------------------
    // The pixel-budget and Ground POM distance sliders that used to be
    // duplicated here are now registered property groups (viewer.budget /
    // render.pom) drawn above. The camera far-plane hand slider went the same
    // way, into the User-scope camera.prefs group below — it lives here rather
    // than in the Camera panel because draw distance is a frame-time dial
    // first and a viewpoint setting second, and because it is the axis scale
    // the transition bars below are drawn against.
    ImGui::SeparatorText("Dynamic (applies immediately)");
    // camera.prefs' note_panel_home lives in draw_performance_panel's
    // unconditional preamble, not here — see that comment for why.
    if (matter::props::Binding* cam = props.camera()) draw_group_fields(*cam);

    // ---- Streaming profile: consumed once at world connect ---------------
    ImGui::SeparatorText("Streaming (requires world reload)");
    matter::props::Binding* sb = props.streaming();
    if (!session || !sb) {
        ImGui::TextDisabled("No world session");
        return;
    }
    auto& st = lod_settings_;
    // Every edit below — schema field or hand-written ring widget — lands in
    // the props DRAFT, never in the live instance. prop_edit_target is what
    // guarantees the two halves write to the same place.
    StreamingLodPrefs* draft =
        static_cast<StreamingLodPrefs*>(prop_edit_target(*sb));
    matter::WorldSession::StreamingLodConfig active;
    const bool have_active = session->streaming_lod_config(active);
    st.have = have_active;
    if (!draft || !st.have) {
        ImGui::TextDisabled("Waiting for a streaming world connect...");
        return;
    }

    // Rebuild the ring view: the draft's own override where it has one, the
    // world's ACTIVE profile where it does not (an empty override string means
    // exactly "keep the world's own rings"). Skipped mid-drag so a handle
    // being dragged is never yanked back by the re-decode.
    const bool dragging = st.drag_scatter >= 0 || st.drag_bands >= 0;
    if (!dragging) {
        st.edit.scatter_rings = draft->scatter_rings.empty()
                                    ? active.scatter_rings
                                    : decode_rings(draft->scatter_rings);
        st.edit.terrain_bands = draft->terrain_bands.empty()
                                    ? active.terrain_bands
                                    : decode_rings(draft->terrain_bands);
    }
    st.edit.sector_size = active.sector_size;

    // Schema fields (the ladder toggle plus the two ring strings, which show
    // exactly what will be written to the world props file).
    draw_group_fields(*sb);
    // Read AFTER the widgets so a toggle takes effect on the band section
    // below this frame, not next.
    st.edit.terrain_lod_enabled = draft->terrain_lod_enabled;
    bool rings_changed = false;
    ImGui::Spacing();

    // Multi-handle transition bar: one rail scaled 0..camera-far-plane with
    // a draggable handle per LOD transition. Handles cannot cross (order is
    // structural), so spacing can be shaped freely and stays valid. Each
    // segment shows its level number; each handle shows its radius above.
    const float axis_max = std::max(camera.far_plane, 500.0f);
    auto transition_bar = [&](const char* str_id,
                              std::vector<matter::WorldSession::
                                              StreamingLodRing>& rows,
                              int& drag_state) -> bool {
        if (rows.empty()) {
            ImGui::TextDisabled("(no rings configured)");
            return false;
        }
        bool changed = false;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float label_h = ImGui::GetTextLineHeight();
        const float bar_h = 26.0f;
        const float w = std::max(80.0f, ImGui::GetContentRegionAvail().x);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 bar0{origin.x, origin.y + label_h + 3.0f};
        ImGui::InvisibleButton(str_id, ImVec2(w, label_h + 3.0f + bar_h));
        auto to_x = [&](float r) {
            return bar0.x + std::min(1.0f, std::max(0.0f, r / axis_max)) * w;
        };

        // Segments: rows[i] covers (radius[i-1], radius[i]]. Brightness maps
        // detail (higher value = more detail = brighter).
        int value_max = 1;
        for (const auto& r : rows) value_max = std::max(value_max, r.value);
        float seg_x = bar0.x;
        for (int i = 0; i < (int)rows.size(); ++i) {
            const float seg_end = to_x(rows[i].radius);
            const float t = float(rows[i].value) / float(value_max);
            const ImU32 fill = ImGui::GetColorU32(
                ImVec4(0.18f + 0.16f * t, 0.32f + 0.25f * t,
                       0.45f + 0.35f * t, 1.0f));
            dl->AddRectFilled(ImVec2(seg_x, bar0.y),
                              ImVec2(seg_end, bar0.y + bar_h), fill, 2.0f);
            char label[16];
            std::snprintf(label, sizeof(label), "%d", rows[i].value);
            const ImVec2 ts = ImGui::CalcTextSize(label);
            if (seg_end - seg_x > ts.x + 6.0f) {
                dl->AddText(ImVec2((seg_x + seg_end - ts.x) * 0.5f,
                                   bar0.y + (bar_h - ts.y) * 0.5f),
                            ImGui::GetColorU32(ImGuiCol_Text), label);
            }
            seg_x = seg_end;
        }
        // Beyond the outer ring: not streamed.
        if (seg_x < bar0.x + w) {
            dl->AddRectFilled(ImVec2(seg_x, bar0.y),
                              ImVec2(bar0.x + w, bar0.y + bar_h),
                              ImGui::GetColorU32(ImVec4(0.12f, 0.12f, 0.14f, 1.0f)),
                              2.0f);
        }

        // Handles + radius labels above them.
        const ImU32 handle_col = ImGui::GetColorU32(ImGuiCol_SliderGrabActive);
        for (int i = 0; i < (int)rows.size(); ++i) {
            const float x = to_x(rows[i].radius);
            dl->AddRectFilled(ImVec2(x - 2.5f, bar0.y - 3.0f),
                              ImVec2(x + 2.5f, bar0.y + bar_h + 3.0f),
                              i == drag_state
                                  ? ImGui::GetColorU32(ImGuiCol_CheckMark)
                                  : handle_col,
                              1.5f);
            char rl[24];
            std::snprintf(rl, sizeof(rl), "%.0f", rows[i].radius);
            const ImVec2 ts = ImGui::CalcTextSize(rl);
            float lx = x - ts.x * 0.5f;
            lx = std::max(bar0.x, std::min(lx, bar0.x + w - ts.x));
            dl->AddText(ImVec2(lx, origin.y),
                        ImGui::GetColorU32(ImGuiCol_TextDisabled), rl);
        }

        // Interaction: grab the nearest handle within 10 px; drag clamps
        // between the neighboring handles so transitions never reorder.
        if (ImGui::IsItemActivated()) {
            const float mx = ImGui::GetIO().MousePos.x;
            float best = 10.0f;
            drag_state = -1;
            for (int i = 0; i < (int)rows.size(); ++i) {
                const float d = std::fabs(to_x(rows[i].radius) - mx);
                if (d < best) { best = d; drag_state = i; }
            }
        }
        if (ImGui::IsItemActive() && drag_state >= 0 &&
            drag_state < (int)rows.size()) {
            const float mx = ImGui::GetIO().MousePos.x;
            float r = (mx - bar0.x) / w * axis_max;
            const float lo = drag_state > 0
                                 ? rows[drag_state - 1].radius + 1.0f
                                 : 16.0f;
            const float hi = drag_state + 1 < (int)rows.size()
                                 ? rows[drag_state + 1].radius - 1.0f
                                 : axis_max;
            r = std::max(lo, std::min(r, hi));
            if (r != rows[drag_state].radius) {
                rows[drag_state].radius = r;
                changed = true;
            }
        }
        if (ImGui::IsItemDeactivated()) drag_state = -1;
        if (ImGui::IsItemHovered() && drag_state < 0)
            ImGui::SetTooltip("Drag a handle to move that LOD transition");
        return changed;
    };

    ImGui::TextUnformatted("Scatter detail (tier 2 near ... 0 far)");
    rings_changed |= transition_bar("##scatter_bar", st.edit.scatter_rings,
                               st.drag_scatter);
    ImGui::Spacing();
    if (st.edit.terrain_lod_enabled) {
        ImGui::TextUnformatted(
            "Terrain LOD (5 native voxel ... 0 one quad)");
        rings_changed |= transition_bar("##bands_bar", st.edit.terrain_bands,
                                   st.drag_bands);
        // 2:1 spacing hint against the world's sector size.
        bool tight = false;
        for (size_t i = 1; i < st.edit.terrain_bands.size(); ++i) {
            if (st.edit.terrain_bands[i].radius -
                    st.edit.terrain_bands[i - 1].radius <
                2.0f * st.edit.sector_size)
                tight = true;
        }
        if (tight)
            ImGui::TextColored(
                ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                "Bands closer than 2 sectors (%.0f m) get re-balanced by "
                "the streamer.",
                2.0f * st.edit.sector_size);
        // The streamed area is bounded by the outer SCATTER ring; terrain
        // bands beyond it exist but no sector out there is resident.
        if (!st.edit.scatter_rings.empty() &&
            !st.edit.terrain_bands.empty() &&
            st.edit.terrain_bands.back().radius >
                st.edit.scatter_rings.back().radius + 1.0f)
            ImGui::TextDisabled(
                "Terrain bands beyond the outer scatter ring (%.0f m) are "
                "outside the streamed area.",
                st.edit.scatter_rings.back().radius);
    }

    // Numeric fallback for precise values, level edits, and add/remove.
    if (ImGui::TreeNode("Numeric editing")) {
        auto ring_table =
            [&st, &rings_changed](const char* label, const char* value_label,
                  std::vector<matter::WorldSession::StreamingLodRing>& rows,
                  int value_min, int value_max) {
                ImGui::PushID(label);
                ImGui::TextUnformatted(label);
                int remove_index = -1;
                for (int i = 0; i < (int)rows.size(); ++i) {
                    ImGui::PushID(i);
                    ImGui::SetNextItemWidth(120.0f);
                    rings_changed |= ImGui::DragFloat("##radius", &rows[i].radius,
                                                 8.0f, 16.0f, 20000.0f,
                                                 "%.0f m");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(110.0f);
                    rings_changed |= ImGui::SliderInt(value_label, &rows[i].value,
                                                 value_min, value_max);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("X")) remove_index = i;
                    ImGui::PopID();
                }
                if (remove_index >= 0 && rows.size() > 1) {
                    rows.erase(rows.begin() + remove_index);
                    rings_changed = true;
                }
                if (rows.size() < 8 && ImGui::SmallButton("+ Add")) {
                    matter::WorldSession::StreamingLodRing next =
                        rows.empty()
                            ? matter::WorldSession::StreamingLodRing{256.0f, 0}
                            : rows.back();
                    next.radius += 256.0f;
                    rows.push_back(next);
                    rings_changed = true;
                }
                ImGui::PopID();
            };
        ring_table("Scatter rings", "##tier", st.edit.scatter_rings, 0, 2);
        ImGui::Spacing();
        if (st.edit.terrain_lod_enabled)
            ring_table("Terrain LOD bands", "##lod", st.edit.terrain_bands,
                       0, 5);
        ImGui::TreePop();
    }

    // Fold the hand-written widgets' result back into the SAME draft the
    // schema fields edit, so the generic Apply bar below commits both halves.
    if (rings_changed) {
        if (const matter::props::Desc* d =
                prop_find_field(sb->schema(), "scatter_rings"))
            matter::props::set_string(draft, *d,
                                      encode_rings(st.edit.scatter_rings));
        if (const matter::props::Desc* d =
                prop_find_field(sb->schema(), "terrain_bands"))
            matter::props::set_string(draft, *d,
                                      encode_rings(st.edit.terrain_bands));
    }

    // The generic pending-changes bar replaces the bespoke "Apply & Reload
    // World" button: it pushes the draft into the live instance, then runs
    // EditorProps' reload request — the SAME closure the Tunables panel uses,
    // which is what hands the applied override to the session before issuing
    // the reload. Neither panel needs to know that ordering.
    draw_draft_bar(*sb, props.reload_request(), "Apply & Reload World");

    if (ImGui::Button("Reset to World Defaults")) {
        matter::props::discard_draft(*sb);
        matter::props::reset_group(*sb);   // back to "no override at all"
        session->clear_streaming_lod_overrides();
        if (commands.reload) commands.reload();
    }
    ImGui::SetItemTooltip(
        "Drops the editor's streaming override and reloads, so the world's own "
        "authored profile applies again.");
    ImGui::TextDisabled(
        "Saved per world; re-applied before the first bake on the next load.");
}

void Ui::draw_debug_panel(ViewerStats& s, const ViewerCommands& commands,
                          EditorProps& props) {
    ImGui::Begin("Viewer Debug");

    ImGui::Text("FPS: %.1f  (%.2f ms)", s.fps, s.frame_ms);
    if (s.gpu_timers_supported) {
        const float sum = s.gpu_cull_ms + s.gpu_gbuffer_ms + s.gpu_blas_ms +
                          s.gpu_tlas_ms + s.gpu_rt_ms + s.gpu_denoise_ms +
                          s.gpu_dlss_ms + s.gpu_composite_ms + s.gpu_vol_ms;
        const float unaccounted = s.gpu_total_ms - sum;
        ImGui::Text("GPU %.1fms | Cull %.1f GBuf %.1f BLAS %.1f TLAS %.1f RT %.1f Den %.1f DLSS %.1f Comp %.1f Vol %.1f (other %.1f)",
                    s.gpu_total_ms, s.gpu_cull_ms,
                    s.gpu_gbuffer_ms, s.gpu_blas_ms, s.gpu_tlas_ms, s.gpu_rt_ms,
                    s.gpu_denoise_ms, s.gpu_dlss_ms, s.gpu_composite_ms,
                    s.gpu_vol_ms, unaccounted);
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

    // Main-loop attribution. These partition the same window `frame_ms` covers,
    // so `other` is genuinely unmeasured code, not clock skew. `render` already
    // contains the resolve/build/draw line above it — don't double-count them.
    {
        const float loop_sum = s.loop_poll_ms + s.loop_acquire_ms + s.loop_ui_ms +
                               s.loop_tick_ms + s.loop_pump_ms + s.loop_lab_ms +
                               s.loop_render_ms + s.loop_present_ms;
        ImGui::Text("Loop: poll %.2f  acq %.2f  ui %.2f  tick %.2f",
                    s.loop_poll_ms, s.loop_acquire_ms, s.loop_ui_ms,
                    s.loop_tick_ms);
        ImGui::Text("      pump %.2f  lab %.2f  render %.2f  present %.2f",
                    s.loop_pump_ms, s.loop_lab_ms, s.loop_render_ms,
                    s.loop_present_ms);
        ImGui::Text("      sum %.2f  other %.2f ms", loop_sum,
                    s.frame_ms - loop_sum);
        ImGui::Text("      peak: pump %.2f  acq %.2f ms",
                    s.loop_peak_pump_ms, s.loop_peak_acquire_ms);
    }
    ImGui::Separator();

    if (s.vt_active) {
        ImGui::Text("VT: %u/%u variants  pool %u/%u (%u pinned)  mesh %.1f/%.1f MiB",
                    s.vt_variants, s.vt_max_variants, s.vt_pool_used,
                    s.vt_pool_capacity, s.vt_pool_pinned,
                    static_cast<double>(s.vt_mesh_bytes) / (1024.0 * 1024.0),
                    static_cast<double>(s.vt_mesh_budget_bytes) /
                        (1024.0 * 1024.0));
        if (s.vt_rejected_variants > 0) {
            ImGui::TextColored(
                ImVec4(1.0f, 0.35f, 0.30f, 1.0f),
                "VT: %u variant registration%s REJECTED — affected parts "
                "ignore their surfaces() classification",
                s.vt_rejected_variants,
                s.vt_rejected_variants == 1 ? "" : "s");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "The variant layer pool (MATTER_VT_MAX_VARIANTS) or the CPU "
                    "mesh budget (MATTER_VT_MESH_BUDGET_MB) filled up. Rejected "
                    "parts fall back to the legacy per-material path: correct "
                    "geometry, but the authored surfaces() tape is ignored.");
        }
        ImGui::Separator();
    }

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

    // This panel is stats + debug views now. The tunable groups it used to
    // draw moved out, one binding untouched in each case (a panel chooses
    // WHERE a group appears; it never owned the widgets):
    //   render.lighting, render.volumetrics -> the Lighting panel
    //   viewer.budget, render.pom           -> the Performance panel
    // Tunables no longer repeats them by default (issue dd98763c): each
    // group is hidden there once ITS panel notes ownership at its own draw
    // call, which is what the note_panel_home calls below (and the ones in
    // draw_performance_panel / draw_lighting_contents / draw_console_panel)
    // do. This replaces the static "moved to X" comment that used to live
    // here — that mapping had already drifted once, which is why it is a
    // fact panels declare now instead of a list maintained by hand.
    //
    // resolver_choice / debug_view_mode / vol_debug_view below are the THREE
    // fields viewer.debug describes, edited here as raw Combos (not through
    // draw_group — main.cpp copies debug_view_mode/vol_debug_view onward into
    // the render structs every frame, so the registered struct member IS the
    // widget's backing value, just not through the generic renderer).
    if (matter::props::Binding* b = props.viewer_debug())
        props.note_panel_home(b->schema().path, "Viewer Debug");
    const char* resolvers[] = { "PassThrough", "SectorLod" };
    ImGui::Combo("Resolver", &s.resolver_choice, resolvers, 2);
    if (ImGui::Button("Reload world") && commands.reload) commands.reload();

    ImGui::SeparatorText("Debug View");
    const char* debug_views[] = { "None", "Normals" };
    ImGui::Combo("View", &s.debug_view_mode, debug_views, 2);
    // Not a registered property: main.cpp overwrites the volumetrics struct's
    // vol_debug_view from this every frame, so it has nowhere to persist to.
    // Kept with the other debug views rather than following render.volumetrics
    // into the Lighting panel — it selects a visualization, not a tunable.
    if (s.volumetrics.enabled) {
        const char* vol_views[] = { "Off", "Density", "Scatter", "Integrated" };
        ImGui::Combo("Vol debug##vd", &s.vol_debug_view, vol_views, 4);
    }
    // overlay.animation: also edited (same struct, hand-written widgets, not
    // draw_group) by the Animation panel and Bake Lab. Claiming it HERE too
    // is harmless — note_panel_home just overwrites the same "claimed" fact
    // with a different panel name, and Tunables only cares whether panel_home
    // returns non-null, not which panel it names last.
    if (matter::props::Binding* b = props.animation_overlay())
        props.note_panel_home(b->schema().path, "Viewer Debug");
    draw_animation_debug_overlay_controls(s.animation_overlay);
    if (s.animation_overlay.enabled) {
        if (!s.animation_debug_query_ok)
            ImGui::TextDisabled("Animation data unavailable");
        else if (s.animation_debug_instances == 0)
            ImGui::TextDisabled("No live animation bindings");
        else
            ImGui::TextDisabled("%u live animation binding%s",
                                s.animation_debug_instances,
                                s.animation_debug_instances == 1 ? "" : "s");
    }

    ImGui::End();
}

void Ui::draw_vt_warning_banner(const ViewerStats& s) {
    if (!s.vt_active || s.vt_rejected_variants == 0) return;
    char text[256];
    std::snprintf(text, sizeof(text),
                  "VT budget exceeded: %u variant%s rejected — affected parts "
                  "ignore their surfaces() classification  (variants %u/%u, "
                  "mesh %.0f/%.0f MiB)",
                  s.vt_rejected_variants,
                  s.vt_rejected_variants == 1 ? "" : "s", s.vt_variants,
                  s.vt_max_variants,
                  static_cast<double>(s.vt_mesh_bytes) / (1024.0 * 1024.0),
                  static_cast<double>(s.vt_mesh_budget_bytes) /
                      (1024.0 * 1024.0));
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const ImVec2 text_size = ImGui::CalcTextSize(text);
    const float pad = 6.0f;
    const float x = viewport_rect_.x +
                    (viewport_rect_.w - text_size.x) * 0.5f - pad;
    const float y = viewport_rect_.y + 8.0f;
    draw->AddRectFilled(ImVec2(x, y),
                        ImVec2(x + text_size.x + pad * 2.0f,
                               y + text_size.y + pad * 2.0f),
                        IM_COL32(120, 20, 10, 220), 4.0f);
    draw->AddText(ImVec2(x + pad, y + pad), IM_COL32(255, 210, 200, 255),
                  text);
}

void Ui::draw_bake_lab_panel(BakeLab& lab, matter::evt::Hub* app_hub,
                             matter::WorldSession* session,
                             const std::vector<WorldEntry>& worlds,
                             ViewerStats& stats) {
    // A pending window-raise means the lab.focus_tab command just fired (via
    // the Asset Browser's "Open in Workbench"): make sure the window is visible
    // and raise it to the front. draw_contents (below) separately selects the
    // Workbench tab itself. Poll+clear here so the raise applies exactly once.
    if (lab.take_window_raise()) {
        lab.visible = true;
        ImGui::SetNextWindowFocus();
    }
    if (!lab.visible) return;
    ImGui::Begin("Bake Lab", &lab.visible);
    lab.draw_contents(app_hub, session, worlds, stats.animation_overlay);
    ImGui::End();
}

void Ui::draw_asset_browser_panel(AssetBrowser& browser,
                                  const std::vector<WorldEntry>& worlds, ViewerStats& stats,
                                  const std::string& shared_lib_root,
                                  const ViewerCommands& commands) {
    ImGui::Begin("Assets");
    browser.draw(worlds, stats, shared_lib_root, commands);
    ImGui::End();
}

void Ui::draw_camera_panel(matter::CameraDesc& cam, const CameraPrefs& prefs) {
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
    // CameraPrefs::orbit_step / orbit_zoom_step (camera.prefs, Scope::User).
    // Defaults reproduce the former literals exactly: 0.04 rad, and
    // 1 -/+ 0.04 = 0.96 / 1.04 on the distance.
    const float orbit_step = prefs.orbit_step;

    ImGui::PushButtonRepeat(true);
    ImGui::Text("Orbit:");
    if (ImGui::Button("Left"))  { yaw -= orbit_step; changed = true; }
    ImGui::SameLine();
    if (ImGui::Button("Right")) { yaw += orbit_step; changed = true; }
    ImGui::SameLine();
    if (ImGui::Button("Up"))    { pitch += orbit_step; changed = true; }
    ImGui::SameLine();
    if (ImGui::Button("Down"))  { pitch -= orbit_step; changed = true; }

    if (ImGui::Button("Zoom In")) {
        dist *= 1.0f - prefs.orbit_zoom_step;
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Zoom Out")) {
        dist *= 1.0f + prefs.orbit_zoom_step;
        changed = true;
    }
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

void Ui::draw_worlds_panel(const std::vector<WorldEntry>& worlds, ViewerStats& stats,
                           const ViewerCommands& commands) {
    ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(200, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("Worlds");

    for (int i = 0; i < (int)worlds.size(); ++i) {
        const bool is_current = (i == stats.world_current);
        if (is_current) ImGui::BeginDisabled(true);
        if (ImGui::Button(worlds[i].label.c_str()) && commands.switch_world) {
            commands.switch_world(i);
        }
        if (is_current) ImGui::EndDisabled();
    }

    ImGui::End();
}

void Ui::draw_gizmo(const SelectionSet& selection, const FieldCommands& fields,
                    const matter::CameraDesc& camera,
                    matter::scene::SimulationMode mode, float viewport_x,
                    float viewport_y, float viewport_w, float viewport_h) {
    gizmo_submitted_ = viewer::draw_gizmo(gizmo_state_, selection, fields,
                                          camera, mode, viewport_x, viewport_y,
                                          viewport_w, viewport_h);
}

void Ui::update_gizmo_hotkeys() {
    viewer::update_gizmo_hotkeys(gizmo_state_);
}

bool Ui::ensure_streaming_anchor(matter::WorldSession& session) {
    // World-kind detection. sea_level() succeeds ONLY for world-kind sessions;
    // closed-world (expand/tileset) sessions return false and must not get a
    // stray anchor entity — their geometry never comes from streamed sectors.
    float sea_level = 0.0f;
    if (!session.sea_level(sea_level)) return false;

    flecs::world& world = session.ecs();

    // Idempotence is derived from live ECS state, never from a latched bool:
    // validate_anchor drops the selection when the ECS world identity changed
    // (reload / world switch installs a fresh Flecs world) or when the anchor
    // entity is no longer alive, so a new world always falls through to the
    // create path below.
    const flecs::entity_t selected_before = streaming_anchor_.selected;
    matter_viewer::validate_anchor(streaming_anchor_, world);
    if (selected_before != 0 && streaming_anchor_.selected == 0) {
        anchor_id_input_ = 0;
    }

    if (streaming_anchor_.selected != 0) {
        const flecs::entity anchor(world.c_ptr(), streaming_anchor_.selected);
        // Both components must still be present: SectorStreaming is what the
        // coordinator's owner observer keys off, LocalTransform is what the
        // transform system needs to publish the WorldTransform the streaming
        // system samples.
        if (anchor.has<matter::ecs::LocalTransform>() &&
            anchor.has<matter::streaming::SectorStreaming>()) {
            return false;
        }
    }

    if (streaming_anchor_.selected == 0) {
        // Also sets follow_editor_camera = true, which re-arms the
        // update_sector_streaming -> follow_camera path below.
        matter_viewer::create_anchor(streaming_anchor_, world);
    }
    if (!matter_viewer::attach_streaming(streaming_anchor_, world)) {
        matter_viewer::clear_anchor(streaming_anchor_);
        anchor_id_input_ = 0;
        std::fprintf(stderr,
                     "[streaming] failed to attach the sector streaming anchor\n");
        return false;
    }

    anchor_id_input_ = streaming_anchor_.selected;
    std::printf(
        "[streaming] auto-attached sector streaming anchor entity %llu "
        "(world-kind session, sea level %.2f)\n",
        static_cast<unsigned long long>(streaming_anchor_.selected),
        static_cast<double>(sea_level));
    return true;
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
// (MatterEditor/src/specialized_editors.h). update_sector_streaming above (the
// per-frame anchor/follow logic, not UI) is unaffected.

bool Ui::camera_input_allowed() const {
    if (ImGui::GetCurrentContext() == nullptr) return true;
    const ImGuiIO& io = ImGui::GetIO();
    const bool gizmo_over = gizmo_submitted_ && ImGuizmo::IsOver();
    const bool mouse_captured = io.WantCaptureMouse && !viewport_hovered_;
    const bool kb_captured = io.WantCaptureKeyboard && !viewport_hovered_;
    return matter_viewer::camera_input_allowed(
        mouse_captured, kb_captured,
        gizmo_over, ImGuizmo::IsUsing());
}

} // namespace viewer
