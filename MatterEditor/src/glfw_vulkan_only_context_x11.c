#include "internal.h"

// Linux/X11 counterpart to glfw_vulkan_only_context.c (see that file for the
// Win32/WGL rationale -- the same reasoning applies here, backend swapped).
//
// GLFW's x11_window.c calls _glfwInitGLX / _glfwChooseVisualGLX /
// _glfwCreateContextGLX whenever ctxconfig->client != GLFW_NO_API
// (x11_window.c, _glfwCreateWindowX11, around line 1968-2005 in this GLFW
// checkout). This editor always creates its window with
// glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API) before glfwCreateWindow (see
// MatterEditor/src/main.cpp), so none of these are ever invoked at runtime.
// But the symbols must still resolve at LINK time: glx_context.c (the real
// GLX/libGL implementation) is deliberately excluded from GLFW_LINUX_SRC in
// MatterEditor/Makefile, so without these stubs the Linux Vulkan target would
// fail to link with undefined references. This mirrors the Win32/WGL stub
// approach exactly, one platform backend later.
//
// UNVERIFIED (Phase 5b, Linux Vulkan port): written by reading x11_window.c
// and x11_platform.h on the Windows machine that authored this file -- there
// is no Linux toolchain here to compile or link it. The four symbols below
// are exactly the ones referenced from x11_init.c / x11_window.c outside of
// glx_context.c itself, confirmed by:
//   grep -rn "_glfw[A-Za-z]*GLX" third_party/raylib/src/external/glfw/src/*.c
// against this vendored GLFW checkout. _glfwDestroyContextGLX is declared in
// x11_platform.h but has no caller anywhere in this GLFW checkout, so it is
// intentionally NOT stubbed here -- if some other translation unit ends up
// calling it, the build will fail with a clear undefined-reference link
// error rather than silently link a no-op.
GLFWbool _glfwInitGLX(void) { return GLFW_FALSE; }
void _glfwTerminateGLX(void) {}
GLFWbool _glfwChooseVisualGLX(const _GLFWwndconfig* wndconfig,
                              const _GLFWctxconfig* ctxconfig,
                              const _GLFWfbconfig* fbconfig,
                              Visual** visual, int* depth) {
    (void) wndconfig; (void) ctxconfig; (void) fbconfig;
    (void) visual; (void) depth;
    return GLFW_FALSE;
}
GLFWbool _glfwCreateContextGLX(_GLFWwindow* window,
                               const _GLFWctxconfig* ctxconfig,
                               const _GLFWfbconfig* fbconfig) {
    (void) window; (void) ctxconfig; (void) fbconfig;
    return GLFW_FALSE;
}
