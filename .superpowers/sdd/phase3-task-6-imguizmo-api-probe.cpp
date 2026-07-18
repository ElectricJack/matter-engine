#include "imgui.h"
#include "ImGuizmo.h"

bool imguizmo_translate_hover_compile_probe() {
    return ImGuizmo::IsOver(ImGuizmo::TRANSLATE);
}
