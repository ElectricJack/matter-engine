// MatterEditor/src/glfw_vulkan_only_context.c
//
// Compiled into the Windows editor target only (MatterEditor/Makefile appends
// it to GLFW_WIN_OBJ, and excludes GLFW's own wgl_context.c on purpose). The
// Linux/X11 counterpart is glfw_vulkan_only_context_x11.c. Both exist because
// the editor is Vulkan-only: the GL/raylib render path was deleted, and these
// stubs keep the last OpenGL imports out of the binary — the link step asserts
// no OpenGL import survives.
//
// Every function here returns "failed" rather than doing nothing successfully:
// the window is created with GLFW_CLIENT_API = GLFW_NO_API (main.cpp), so none
// of them should ever be reached, and if one is, failing loudly at that call is
// better than pretending a context exists.

#include "internal.h"

// The Windows milestone always creates GLFW_NO_API windows.  Keeping GLFW's
// WGL translation unit would nevertheless retain pixel-format and SwapBuffers
// imports through the platform dispatch table, so provide fail-closed stubs for
// the context path that must never be entered.
GLFWbool _glfwInitWGL(void) { return GLFW_FALSE; }
void _glfwTerminateWGL(void) {}
GLFWbool _glfwCreateContextWGL(_GLFWwindow* window,
                               const _GLFWctxconfig* ctxconfig,
                               const _GLFWfbconfig* fbconfig) {
    (void) window; (void) ctxconfig; (void) fbconfig;
    return GLFW_FALSE;
}
