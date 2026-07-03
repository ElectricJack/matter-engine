#include "ui.h"

#include <cmath>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace viewer {

void Ui::setup() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(glfwGetCurrentContext(), true);
    ImGui_ImplOpenGL3_Init("#version 330");
}

void Ui::shutdown() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void Ui::begin_frame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void Ui::end_frame() {
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void Ui::draw_debug_panel(ViewerStats& s) {
    ImGui::Begin("Viewer Debug");

    ImGui::Text("FPS: %.1f  (%.2f ms)", s.fps, s.frame_ms);
    ImGui::Text("Camera: %.1f, %.1f, %.1f", s.cam_pos[0], s.cam_pos[1], s.cam_pos[2]);
    ImGui::Separator();

    ImGui::Text("Instances: %d active / %d total", s.instances_active, s.instances_total);
    ImGui::Text("Occupied sectors: %d", s.occupied_sectors);
    ImGui::Separator();

    ImGui::Text("Provider: %s", s.connected ? "connected" : "disconnected");
    ImGui::Text("Last connect: %d baked, %d cache hits", s.parts_baked, s.cache_hits);
    ImGui::Text("Last reconcile want: %d", s.last_want_count);
    ImGui::Separator();

    ImGui::Text("Raster: %d batches / %d tris  culled: %d", s.raster_batches, s.raster_tris, s.culled_clusters);
    ImGui::Separator();

    const char* resolvers[] = { "PassThrough", "SectorLod" };
    ImGui::Combo("Resolver", &s.resolver_choice, resolvers, 2);
    if (ImGui::Button("Reload world")) s.reload_requested = true;

    ImGui::End();
}

void Ui::draw_camera_panel(Camera3D& cam) {
    ImGui::SetNextWindowPos(ImVec2(GetScreenWidth() - 270.0f, 20.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(250, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("Camera");

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

} // namespace viewer
