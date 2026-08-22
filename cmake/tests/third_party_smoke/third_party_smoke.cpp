#include <GLFW/glfw3.h>
#include <imgui.h>
#include <ImGuizmo.h>
#include <autoremesher/remesh.h>
#include <box3d/box3d.h>
#include <flecs.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/runtime/animation.h>
#include <ozz/base/memory/allocator.h>

extern "C" {
#include <quickjs.h>
}

#include <bc7enc.h>
#include <rgbcx.h>

#include <cstdio>

int main() {
    JSRuntime* runtime = JS_NewRuntime();
    if (runtime == nullptr) {
        return 1;
    }
    JS_FreeRuntime(runtime);

    ecs_world_t* world = ecs_init();
    if (world == nullptr || ecs_fini(world) != 0) {
        return 2;
    }

    const b3WorldDef world_def = b3DefaultWorldDef();
    int glfw_major = 0;
    int glfw_minor = 0;
    int glfw_revision = 0;
    glfwGetVersion(&glfw_major, &glfw_minor, &glfw_revision);

    ozz::memory::Allocator* allocator = ozz::memory::default_allocator();
    const ozz::animation::Animation animation;
    const ozz::animation::offline::RawAnimation raw_animation;

    bc7enc_compress_block_init();
    rgbcx::init();

    const bool boundaries_ok = world_def.enableContinuous &&
        glfw_major > 0 && allocator != nullptr && animation.num_tracks() == 0 &&
        raw_animation.Validate() && ImGui::GetVersion() != nullptr &&
        !ImGuizmo::IsUsing() && autoremesher::AUTOREMESHER_CORE_VERSION != nullptr;
    if (!boundaries_ok) {
        return 3;
    }

    std::printf("matter_third_party_smoke: GLFW %d.%d.%d, ImGui %s, autoremesher %s\n",
                glfw_major, glfw_minor, glfw_revision, ImGui::GetVersion(),
                autoremesher::AUTOREMESHER_CORE_VERSION);
    return 0;
}
