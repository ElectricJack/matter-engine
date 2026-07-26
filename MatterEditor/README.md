# MatterEditor

Interactive world editor for MatterEngine3. Vulkan renderer, GLFW window/input,
Dear ImGui UI, QuickJS-ng script host, Box3d physics.

## Windows (verified, primary target)

Built and run in this repository's normal development environment (MSYS2
UCRT64 on Windows). See the root `CLAUDE.md` for the toolchain setup and
exact build commands:

```bash
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
make -C MatterEditor windows TMP="C:/Users/<you>/AppData/Local/Temp" TEMP="C:/Users/<you>/AppData/Local/Temp"
```

`windows` is `.DEFAULT_GOAL` in `MatterEditor/Makefile`, so a bare `make -C
MatterEditor` also builds it. Output: `build/windows/editor.exe`. Links no
GL and no raylib (`WIN_LIBS` in the Makefile has neither); Streamline/DLSS is
opt-in via `HAVE_STREAMLINE=1` plus `STREAMLINE_PATH=`.

## Linux (Phase 5b, UNTESTED — read before relying on this)

`make -C MatterEditor linux` compiles the same Vulkan editor sources as the
Windows target, using a Vulkan-only GLFW build against X11 instead of Win32,
and links `-lvulkan` + `-lX11` instead of `-lGL`. Output: `build/linux/editor`.

**This target has never been compiled, linked, or run.** It was written on a
Windows machine with no Linux toolchain, Vulkan loader, X11/GLFW development
headers, or Linux hardware available anywhere in that environment. Everything
below is either "verified by reading the source and Vulkan/GLFW headers
actually present in this repo" or explicitly marked as a guess. Treat this
target as a well-reasoned starting point for someone with a real Linux box,
not as a working build.

The one thing that *was* checked mechanically: `make -C MatterEditor linux
-n` (dry run) expands to a sensible-looking command sequence — correct source
paths, no reference to the deleted `main_linux.cpp`/`ui_linux.cpp`, no
`-lGL`, no raylib on the link line, `-DMATTER_HAVE_STREAMLINE=0` present. A
dry run cannot catch a missing header, a wrong function signature, or a
runtime failure, so this only rules out the most obvious classes of mistake
(wrong source paths, wrong flags, forgotten explicit source-list entries —
this repo does not glob for sources).

### Build (once you have a real Linux machine)

```bash
make -C MatterEditor linux \
    VULKAN_INCLUDE=/usr/include \
    VULKAN_LIB_DIR=/usr/lib/x86_64-linux-gnu \
    GLSLC=/usr/bin/glslc
```

`VULKAN_INCLUDE` / `VULKAN_LIB_DIR` / `GLSLC` are the *same* variables the
Windows target uses; their defaults point at an MSYS2 UCRT64 install
(`/ucrt64/...`), which will not exist on a real Linux box, so override them.
Exact paths are distro-dependent — the values above are Debian/Ubuntu-shaped
(`libvulkan-dev`'s multiarch lib directory); adjust for your distribution.

You will also need, at minimum: a C/C++ toolchain (`gcc`/`g++`), the Vulkan
loader + headers + `glslc` (Debian/Ubuntu: `libvulkan-dev
vulkan-validationlayers glslc` or `shaderc`, package names vary by distro),
and X11 development headers (`libx11-dev libxrandr-dev libxinerama-dev
libxcursor-dev libxi-dev libxext-dev` — this is the set GLFW's own
`CMakeLists.txt` checks for when building its X11 backend; not independently
confirmed against a package manager here).

### What this reuses vs. adds

- **Reuses, unmodified**: the same `APP_SRC` / `WIN_ME3_CPP` / `WIN_MSL_CPP`
  / `WIN_PIPELINE_C` / `IMGUI_SRC_WIN` source lists the Windows target
  compiles. These were never actually Windows-specific — see the "Linux
  Vulkan target (Phase 5b)" comment block in `MatterEditor/Makefile` for the
  full reasoning (the `WIN_` prefix is historical).
- **Adds**: a Vulkan-only GLFW build for X11 (mirrors
  `src/glfw_vulkan_only_context.c`'s Win32/WGL-stub approach in
  `src/glfw_vulkan_only_context_x11.c`, stubbing the GLX entry points GLFW's
  X11 backend references so the real GLX/libGL implementation
  (`glx_context.c`) never needs to be compiled in), and a small fix in
  `MatterEngine3/src/render/vk_context.cpp` (see below).
- **X11 only, not Wayland.** This matches the old (Phase-5a-deleted) GL Linux
  target's dependency set and keeps the port bounded. GLFW 3.4 supports
  building both backends with runtime auto-selection, so adding
  `_GLFW_WAYLAND` later is additive, not a rewrite.
- **No DLSS.** `HAVE_STREAMLINE` is hardcoded to `0` for this target and is
  not threaded through from the shared `HAVE_STREAMLINE`/`STREAMLINE_PATH`
  variables the Windows target reads — Streamline is a Windows-only SDK, so
  there is no way to accidentally build this target with Streamline on.

### `vk_context.cpp` fix (not just a Makefile change)

Reading `MatterEngine3/src/render/vk_context.cpp` for this landing found a
spot that would not even **compile** on Linux, beyond the two known gaps the
Phase 5b plan called out (instance-extension selection, validation-layer
availability):

`missing_device_capabilities()` and `create_logical_device()` unconditionally
referenced `VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME` and
`VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME`. Those macros only exist
when `<vulkan/vulkan_win32.h>` is pulled in (which only happens when
`VK_USE_PLATFORM_WIN32_KHR` is defined), so referencing them outside an
`#ifdef _WIN32` is a hard compile error on Linux, not a runtime gap. Tracing
what actually *uses* that capability found nothing: it is unimplemented
CUDA/OptiX external-memory/semaphore interop
(`docs/superpowers/specs/2026-07-13-vulkan-temporal-foundation-design.md`),
with no `vkGetMemoryWin32HandleKHR` / `vkImportSemaphoreWin32HandleKHR` call
anywhere in this repo. The fix gates all three spots to `_WIN32` (byte-for-byte
identical on Windows) and simply drops the requirement on Linux rather than
guessing at the `_fd`-suffixed equivalents for a capability nothing consumes.
Full reasoning is in the comments at each site in that file.

### Instance extensions and validation layers (the two gaps the plan called out)

Both turned out to already be handled by portable APIs, so **no code change
was needed** for either:

- **Instance extensions**: `create_instance()` gets its surface-related
  extensions from `glfwGetRequiredInstanceExtensions()`, which is itself
  platform-aware inside GLFW (returns `VK_KHR_win32_surface` on Windows,
  `VK_KHR_xlib_surface` on X11) — nothing in `vk_context.cpp` hardcodes
  `VK_KHR_win32_surface`.
- **Validation layers**: `vkEnumerateInstanceLayerProperties` /
  `VK_LAYER_KHRONOS_validation` is portable Vulkan API surface with no
  Windows-specific code path in this file.
- **`create_surface()`** already has a working `#else` branch for non-Windows
  (`glfwCreateWindowSurface`) — this predates Phase 5b.

Neither of these has been exercised against a real Linux Vulkan loader or a
real `VK_LAYER_KHRONOS_validation` install, so "handled by portable APIs" is
a code-reading conclusion, not a tested one.

### SPIR-V embedding

`MatterEngine3/tools/embed_spirv.py` and `tools/embed_shaders.py` were read
end-to-end: both use `pathlib`/`os.path.join` and only ever key embedded
entries by basename/logical-path strings, never a full OS path. No
Windows-specific assumption found. Not independently re-run against Linux
`glslc` output to confirm.

### Known unknowns (do not treat any of these as "probably fine")

- **Whether it compiles at all.** No Linux toolchain was available to try.
- **X11/Vulkan dev package names and versions** for any given distro.
- **`VK_LAYER_KHRONOS_validation` availability** on a real Linux Vulkan SDK
  install, and whether validation output differs from Windows in a way that
  changes `select_physical_device()`'s behavior.
- **Instance-extension list correctness at runtime** — confirmed by reading
  GLFW's implementation, not by running it against a Linux Vulkan loader.
- **`third_party/box3d/libbox3d.a`** (the native, non-mingw build this target
  links) is a committed binary; whether it is ABI-compatible with whatever
  toolchain actually builds this target is unconfirmed. If not, `make -C
  third_party/box3d` rebuilds it natively (an existing rule, unrelated to
  Phase 5b).
- **Wayland is not implemented.** X11 only; see above.
- **`make -C MatterEditor dist`** does not produce a usable Linux package —
  it unconditionally requires a Streamline SDK and copies `.dll` files,
  neither of which makes sense off Windows. Fixing `dist` for Linux was out
  of scope for this phase.
- **GLFW's runtime dependency loading.** GLFW's X11 backend `dlopen()`s
  `libX11`/`libXrandr`/`libXinerama`/`libXcursor`/`libXi` itself at runtime
  (confirmed by reading `x11_init.c`) rather than linking them at build time,
  but `x11_window.c` also calls Xlib functions like `XCreateWindow` directly,
  which *does* require linking `-lX11` — both halves were read, but the
  actual runtime behavior (whether the dlopen'd libraries are found, whether
  the directly-linked and dlopen'd copies of libX11 interact correctly) is
  unverified.

If you get this building on a real machine, the most valuable next step is
running it against `RenderDoc` or `vkconfig` with validation layers on, and
comparing `vk_context.cpp`'s device-selection log line
(`std::printf("Vulkan adapter: ...")`) against what you'd expect for your GPU.
