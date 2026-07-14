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
