# Vulkan and Canonical Matrices Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the Windows MatterViewer OpenGL renderer with Vulkan 1.3, canonicalize camera and instance matrix semantics, and restore CUDA/OptiX lighting through Vulkan external interop with a proven `HAVE_CUDA=1` build.

**Architecture:** MatterViewer owns a direct GLFW `GLFW_NO_API` window and a `matter::VulkanDevice`; the engine records scene work into its active frame. Matrices use row-major storage, column-vector algebra, `projection * view * model` composition, Vulkan `[0,1]` depth, and explicit GPU packing. This is a clean cut: old OpenGL code remains only until Vulkan raster and OptiX parity pass, then leaves the Windows build.

**Tech Stack:** C++17, Vulkan 1.3, SPIR-V via `glslc`, GLFW, Dear ImGui Vulkan backend, CUDA Driver API, NVIDIA OptiX, MinGW-w64, Make, and the existing C++ test harness.

## Global Constraints

- Follow `docs/superpowers/specs/2026-07-13-vulkan-temporal-foundation-design.md`.
- Windows NVIDIA is the first interactive target; Linux/WSL headless tests must remain buildable.
- Do not build a runtime OpenGL/Vulkan selector or permanent cross-API renderer.
- Store matrices row-major and apply them to column vectors; `A * B` applies `B` first.
- Use a right-handed world, positive Y up, negative view-space Z forward, and Vulkan depth `[0,1]`.
- Keep persisted instance bytes unchanged; translation remains at `[3,7,11]`.
- Keep camera matrices unjittered. Motion vectors and jitter belong to Phase 2.
- Keep HiZ disabled by default and outside this migration's debugging scope.
- Reset/bypass temporal history during camera motion until Phase 2 validates dense motion vectors.
- Every Windows build command must specify `HAVE_CUDA=1`. Through Task 9 (before
  Task 10 interop exists), the manifest is exactly `VULKAN=1`, `OPENGL=0`,
  `CUDA_AVAILABLE=1`, `OPTIX_AVAILABLE=1`, `CUDA_ACTIVE=0`, and
  `OPTIX_ACTIVE=0`. Task 10 activates CUDA interop; Task 11 activates OptiX;
  the Task 13 final gate therefore requires both `CUDA_ACTIVE=1` and
  `OPTIX_ACTIVE=1`.
- Preserve unrelated working-tree changes and stage only each task's files.

---

## File Map

**Public contracts**

- Create `MatterEngine3/include/matter/math_types.h`: `Float3`, `Float4`, `Mat4f`.
- Create `MatterEngine3/include/matter/camera.h`: raylib-free `CameraDesc`.
- Create `MatterEngine3/include/matter/vulkan_device.h`: Vulkan device, swapchain, and frame interface.
- Modify `MatterEngine3/include/matter/engine_context.h`: accept `VulkanDevice*`.
- Modify `MatterEngine3/include/matter/world_session.h`: replace `Camera3D` with `CameraDesc`.

**Matrix implementation**

- Create `MatterEngine3/src/render/matrix_math.h` and `MatterEngine3/src/render/matrix_math.cpp`: multiply, inverse, look-at, projection, transforms, and frustum planes.
- Create `MatterEngine3/src/render/frame_matrices.h` and `MatterEngine3/src/render/frame_matrices.cpp`: authoritative camera matrices.
- Create `MatterEngine3/src/render/gpu_matrix_pack.h`: explicit GLSL `mat4` packing.
- Create `MatterEngine3/tests/matrix_tests.cpp`.

**Vulkan implementation**

- Create `MatterEngine3/src/render/vk_context.cpp`.
- Create `MatterEngine3/src/render/vk_resources.h` and `MatterEngine3/src/render/vk_resources.cpp`.
- Create `MatterEngine3/src/render/vk_pipeline.h` and `MatterEngine3/src/render/vk_pipeline.cpp`.
- Create `MatterEngine3/src/render/vk_scene_renderer.h` and `MatterEngine3/src/render/vk_scene_renderer.cpp`.
- Create `MatterEngine3/src/render/vk_cuda_interop.h` and `MatterEngine3/src/render/vk_cuda_interop.cpp`.
- Create `MatterEngine3/tests/vulkan_smoke_tests.cpp`.
- Create `MatterEngine3/shaders_vk/cull.comp`, `raster.vert`, `gbuffer.frag`, `composite.vert`, `composite.frag`, and `transform_probe.comp`.
- Create `MatterEngine3/tools/embed_spirv.py` and generated `MatterEngine3/shaders_gen/embedded_spirv.h`.

**Viewer/build**

- Create `MatterViewer/camera_controller.h` and `MatterViewer/camera_controller.cpp`.
- Modify `MatterViewer/main.cpp`, `ui.h`, and `ui.cpp`.
- Create `MatterViewer/tools/check_vulkan_toolchain.sh`.
- Create `MatterViewer/tools/run_vulkan_acceptance.ps1`.
- Create `MatterViewer/tests/camera_paths/forward_back.json`, `MatterViewer/tests/camera_paths/strafe.json`, and `MatterViewer/tests/camera_paths/yaw_pitch.json`.
- Modify `MatterViewer/Makefile` and `MatterEngine3/Makefile`.

**OptiX migration**

- Refactor `MatterEngine3/src/render/rt_lighting.h` and `MatterEngine3/src/render/rt_lighting.cpp` to consume Vulkan/CUDA shared resources.
- Modify `MatterEngine3/src/render/shaders_rt/rt_params.h` and `lighting_raygen.cu` for canonical Vulkan depth reconstruction.
- Remove Windows build references to GL renderer sources, GL shaders, raylib graphics, and ImGui OpenGL.

---

### Task 1: Lock the Vulkan/CUDA Toolchain and Build Evidence

**Files:**
- Create: `MatterViewer/tools/check_vulkan_toolchain.sh`
- Modify: `MatterViewer/Makefile`

**Interfaces:**
- Produces: `make -C MatterViewer vulkan-preflight HAVE_CUDA=1` and `build/windows/build_features.txt`.

- [ ] **Step 1: Add the prerequisite checker**

```sh
#!/usr/bin/env sh
set -eu
: "${WIN_CXX:=x86_64-w64-mingw32-g++-posix}"
: "${VULKAN_INCLUDE:=/usr/x86_64-w64-mingw32/include}"
: "${VULKAN_LIB_DIR:=/usr/x86_64-w64-mingw32/lib}"
: "${GLSLC:=glslc}"
: "${CUDA_PATH:=/usr/local/cuda}"
: "${OPTIX_PATH:=$HOME/NVIDIA-OptiX-SDK-7.7.0}"
command -v "$WIN_CXX" >/dev/null
command -v "$GLSLC" >/dev/null
test -f "$VULKAN_INCLUDE/vulkan/vulkan.h"
test -f "$VULKAN_LIB_DIR/libvulkan-1.a"
test -f "$CUDA_PATH/include/cuda.h"
test -f "$OPTIX_PATH/include/optix.h"
src="$(mktemp --suffix=.cpp)"; exe="$(mktemp --suffix=.exe)"
printf '#include <vulkan/vulkan.h>\nint main(){return vkEnumerateInstanceVersion(0);}\n' > "$src"
"$WIN_CXX" "$src" -I"$VULKAN_INCLUDE" -L"$VULKAN_LIB_DIR" -lvulkan-1 -o "$exe"
rm -f "$src" "$exe"
printf 'vulkan-preflight: OK CUDA=1 OPTIX=1\n'
```

- [ ] **Step 2: Wire preflight and feature evidence into Make**

```makefile
GLSLC ?= glslc
VULKAN_INCLUDE ?= /usr/x86_64-w64-mingw32/include
VULKAN_LIB_DIR ?= /usr/x86_64-w64-mingw32/lib
WIN_FEATURES = build/windows/build_features.txt

.PHONY: vulkan-preflight
vulkan-preflight:
	@test "$(HAVE_CUDA)" = "1" || (echo "ERROR: Phase 1 requires HAVE_CUDA=1"; exit 1)
	@WIN_CXX="$(WIN_CXX)" VULKAN_INCLUDE="$(VULKAN_INCLUDE)" \
	 VULKAN_LIB_DIR="$(VULKAN_LIB_DIR)" GLSLC="$(GLSLC)" \
	 CUDA_PATH="$(CUDA_PATH)" OPTIX_PATH="$(OPTIX_PATH)" sh tools/check_vulkan_toolchain.sh

$(WIN_FEATURES):
	@mkdir -p $(dir $@)
	@printf 'VULKAN=1\nOPENGL=0\nCUDA_AVAILABLE=1\nOPTIX_AVAILABLE=1\nCUDA_ACTIVE=0\nOPTIX_ACTIVE=0\n' > $@
```

- [ ] **Step 3: Verify**

Run:

```sh
make -C MatterViewer vulkan-preflight HAVE_CUDA=1
make -C MatterViewer build/windows/build_features.txt HAVE_CUDA=1
cat MatterViewer/build/windows/build_features.txt
```

Expected: preflight succeeds and the pre-Task-10 manifest contains exactly the
six declared availability/activity lines above.

- [ ] **Step 4: Commit**

```sh
git add MatterViewer/tools/check_vulkan_toolchain.sh MatterViewer/Makefile
git commit -m "build: add Vulkan CUDA toolchain preflight"
```

### Task 2: Introduce Canonical Matrix and Camera Types

**Files:**
- Create: `MatterEngine3/include/matter/math_types.h`
- Create: `MatterEngine3/include/matter/camera.h`
- Create: `MatterEngine3/src/render/matrix_math.h`
- Create: `MatterEngine3/src/render/matrix_math.cpp`
- Create: `MatterEngine3/src/render/frame_matrices.h`
- Create: `MatterEngine3/src/render/frame_matrices.cpp`
- Create: `MatterEngine3/src/render/gpu_matrix_pack.h`
- Create: `MatterEngine3/tests/matrix_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile`

**Interfaces:**
- Produces: `Mat4f`, `CameraDesc`, `FrameMatrices`, `mat4_mul`, `look_at_rh`, `perspective_rh_zo`, `mat4_inverse`, `extract_frustum_planes_zo`, `pack_glsl_mat4`.

- [ ] **Step 1: Write failing analytic tests**

```cpp
static bool closef(float a,float b,float e){ return std::fabs(a-b)<=e; }
static bool close3(Float3 a,Float3 b,float e){ return closef(a.x,b.x,e)&&closef(a.y,b.y,e)&&closef(a.z,b.z,e); }
static void test_matrix_translation_uses_3_7_11() {
    auto t = viewer::mat4_translation({3,4,5});
    CHECK(closef(t.m[3],3,1e-6f)&&closef(t.m[7],4,1e-6f)&&closef(t.m[11],5,1e-6f), "translation indices");
    CHECK(close3(viewer::transform_point(t,{1,2,3}),{4,6,8},1e-6f), "translated point");
}
static void test_vulkan_projection_maps_near_far_to_zero_one() {
    auto p = viewer::perspective_rh_zo(60*kPi/180, 16.0f/9.0f, 1, 5000);
    CHECK(closef(viewer::project_ndc(p,{0,0,-1}).z,0,1e-5f), "near maps to zero");
    CHECK(closef(viewer::project_ndc(p,{0,0,-5000}).z,1,1e-5f), "far maps to one");
}
static void test_composition_is_projection_times_view() {
    matter::CameraDesc c{{0,0,5},{0,0,0},{0,1,0},60*kPi/180,0.1f,100};
    viewer::FrameMatrices f{}; std::string error;
    CHECK(viewer::build_frame_matrices(c,1280,720,f,error),"build frame matrices");
    CHECK(close3(viewer::project_ndc(f.world_to_clip,{0,0,0}),{0,0,0.980981f},1e-5f), "projection times view");
}
static void test_glsl_pack_is_explicit() {
    auto p = viewer::pack_glsl_mat4(viewer::mat4_translation({3,4,5}));
    CHECK(closef(p.elements[12],3,1e-6f)&&closef(p.elements[13],4,1e-6f)&&closef(p.elements[14],5,1e-6f), "explicit GLSL pack");
}
int main(){ test_matrix_translation_uses_3_7_11(); test_vulkan_projection_maps_near_far_to_zero_one(); test_composition_is_projection_times_view(); test_glsl_pack_is_explicit(); return check_summary(); }
```

- [ ] **Step 2: Add `run-matrix` and confirm it fails before implementation**

```makefile
MATRIX_TARGET = matrix_tests
MATRIX_CPP = matrix_tests.cpp ../src/render/matrix_math.cpp ../src/render/frame_matrices.cpp
$(MATRIX_TARGET): $(MATRIX_CPP)
	$(CC) $(MATRIX_CPP) -o $@ $(CFLAGS) $(INCLUDE_PATHS)
.PHONY: run-matrix
run-matrix: $(MATRIX_TARGET)
	./$(MATRIX_TARGET)
```

Run: `make -C MatterEngine3/tests run-matrix`. Expected: missing symbols.

- [ ] **Step 3: Define the POD contracts**

```cpp
namespace matter {
struct Float3 { float x=0,y=0,z=0; };
struct Float4 { float x=0,y=0,z=0,w=0; };
struct Mat4f { float m[16] = {}; }; // row-major storage, column-vector algebra
struct CameraDesc {
    Float3 position{20,16,34}, target{0,9,0}, up{0,1,0};
    float vertical_fov_radians=0.78539816339f, near_plane=1, far_plane=5000;
};
}
```

- [ ] **Step 4: Implement the exact view/projection formulas**

```cpp
Mat4f perspective_rh_zo(float fovy,float aspect,float n,float f) {
    float y=1/std::tan(fovy*0.5f); Mat4f p{};
    p.m[0]=y/aspect; p.m[5]=y; p.m[10]=f/(n-f); p.m[11]=f*n/(n-f); p.m[14]=-1;
    return p;
}
Mat4f look_at_rh(Float3 eye,Float3 target,Float3 up_hint) {
    Float3 f=normalize(target-eye), r=normalize(cross(f,up_hint)), u=cross(r,f);
    return Mat4f{{r.x,r.y,r.z,-dot(r,eye), u.x,u.y,u.z,-dot(u,eye),
                  -f.x,-f.y,-f.z,dot(f,eye), 0,0,0,1}};
}
```

Implement multiply, inverse, point/vector transforms, project/unproject, and normalize frustum planes using `left=row3+row0`, `right=row3-row0`, `bottom=row3+row1`, `top=row3-row1`, `near=row2`, `far=row3-row2`.

- [ ] **Step 5: Build authoritative frame matrices**

```cpp
struct FrameMatrices {
    matter::Mat4f world_to_view, view_to_clip, world_to_clip, clip_to_world;
    float frustum_planes[6][4]{};
};
bool build_frame_matrices(const CameraDesc&,uint32_t width,uint32_t height,
                          FrameMatrices&,std::string& error);
```

Set `world_to_clip = mat4_mul(view_to_clip, world_to_view)`. Reject zero extent, invalid planes, degenerate look direction, and singular inverse with a specific error.

- [ ] **Step 6: Verify and commit**

Run: `make -C MatterEngine3/tests run-matrix`. Expected: all tests pass.

```sh
git add MatterEngine3/include/matter/math_types.h MatterEngine3/include/matter/camera.h \
  MatterEngine3/src/render/matrix_math.h MatterEngine3/src/render/matrix_math.cpp \
  MatterEngine3/src/render/frame_matrices.h MatterEngine3/src/render/frame_matrices.cpp \
  MatterEngine3/src/render/gpu_matrix_pack.h MatterEngine3/tests/matrix_tests.cpp MatterEngine3/tests/Makefile
git commit -m "refactor(render): define canonical matrix contract"
```

### Task 3: Migrate CPU Consumers Without Changing Persisted Transforms

**Files:**
- Modify: `MatterEngine3/src/render/raster_cull.h`, `gpu_cull_types.h`, `part_store.cpp`, `world_composer.cpp`
- Modify: `MatterEngine3/src/provider/resolvers.cpp`, `MatterEngine3/src/matter_engine.cpp`
- Modify: `MatterEngine3/tests/gpu_cull_tests.cpp`, `viewer_logic_tests.cpp`

**Interfaces:**
- Consumes: Task 2 matrix functions.
- Produces: one CPU representation and explicitly packed shader matrices.

- [ ] **Step 1: Add byte-compatibility and Vulkan-frustum regressions**

```cpp
static void test_serialized_transform_bytes_are_unchanged() {
    float fixture[16]={1,0,0,7, 0,1,0,8, 0,0,1,9, 0,0,0,1};
    matter::Mat4f m{}; std::memcpy(m.m,fixture,sizeof fixture);
    CHECK(close3(viewer::transform_point(m,{0,0,0}),{7,8,9},1e-6f), "serialized translation");
    CHECK(std::memcmp(m.m,fixture,sizeof fixture)==0, "serialized bytes unchanged");
}
```

- [ ] **Step 2: Replace `make_lookat`/`make_perspective`/`mul16(view,proj)`**

```cpp
matter::CameraDesc camera{eye,target,up,fovy_radians,near_z,far_z};
viewer::FrameMatrices frame{}; std::string error;
if (!viewer::build_frame_matrices(camera,fb_width,fb_height,frame,error)) return report(error);
```

Use `frame.world_to_clip` and `frame.frustum_planes` everywhere. Delete the transposed camera builders.

- [ ] **Step 3: Replace `transpose_to_gl` with named packing**

```cpp
struct GpuInstanceRec { GpuMat4 object_to_world; uint32_t part_slot,base_lod,cluster_start,cluster_count; };
inline GpuInstanceRec pack_instance(const float source[16]) {
    matter::Mat4f m{}; std::memcpy(m.m,source,sizeof m.m);
    GpuInstanceRec out{}; out.object_to_world=pack_glsl_mat4(m); return out;
}
```

- [ ] **Step 4: Verify and commit**

Run:

```sh
make -C MatterEngine3/tests run-matrix run-gpucull run-viewer-logic run-partstore run-comp
```

Expected: all pass and fixture bytes remain unchanged.

```sh
git add MatterEngine3/src/render/raster_cull.h MatterEngine3/src/render/gpu_cull_types.h \
  MatterEngine3/src/render/part_store.cpp MatterEngine3/src/render/world_composer.cpp \
  MatterEngine3/src/provider/resolvers.cpp MatterEngine3/src/matter_engine.cpp \
  MatterEngine3/tests/gpu_cull_tests.cpp MatterEngine3/tests/viewer_logic_tests.cpp
git commit -m "refactor(render): remove implicit camera transpose"
```

### Task 4: Replace the Public raylib Camera API

**Files:**
- Modify: `MatterEngine3/include/matter/world_session.h`, `engine_context.h`, `MatterEngine3/src/matter_engine.cpp`
- Create: `MatterViewer/camera_controller.h`
- Create: `MatterViewer/camera_controller.cpp`
- Modify: `MatterViewer/main.cpp`, `ui.h`, `ui.cpp`
- Test: `MatterEngine3/tests/viewer_logic_tests.cpp`

**Interfaces:**
- Produces: `WorldSession::render(const CameraDesc&,int,int,const RenderOptions&)` and pure `apply_camera_input`.

- [ ] **Step 1: Test forward/back and yaw analytically**

```cpp
static void test_forward_moves_along_camera_forward() {
    CameraDesc c{{0,0,5},{0,0,0},{0,1,0},kQuarterPi,1,5000};
    CameraInput input{}; input.forward=1; apply_camera_input(c,input,1,2,0.002f);
    CHECK(close3(c.position,{0,0,3},1e-5f), "forward position");
    CHECK(close3(c.target,{0,0,-2},1e-5f), "forward target");
}
```

- [ ] **Step 2: Remove raylib from public headers**

```cpp
#include "matter/camera.h"
void render(const CameraDesc&,int framebuffer_width,int framebuffer_height,const RenderOptions&);
```

- [ ] **Step 3: Add the pure controller plus GLFW adapter**

```cpp
struct CameraInput { float forward=0,right=0,up=0,yaw_pixels=0,pitch_pixels=0; bool speed_boost=false; };
void apply_camera_input(CameraDesc&,const CameraInput&,float dt,float speed,float radians_per_pixel);
class CameraController { public: void update(GLFWwindow*,float,CameraDesc&); void set_capture(GLFWwindow*,bool); };
```

- [ ] **Step 4: Convert the ImGui camera panel to `CameraDesc`**

Use `ImGui::DragFloat3` for position/target and maintain the existing orbit/zoom controls with engine vector math.

- [ ] **Step 5: Verify and commit**

Run: `make -C MatterEngine3/tests run-viewer-logic` and `rg -n 'raylib.h|Camera3D' MatterEngine3/include/matter`.

Expected: tests pass; search has no matches.

```sh
git add MatterEngine3/include/matter/world_session.h MatterEngine3/include/matter/engine_context.h \
  MatterEngine3/src/matter_engine.cpp MatterViewer/camera_controller.h MatterViewer/camera_controller.cpp \
  MatterViewer/main.cpp \
  MatterViewer/ui.h MatterViewer/ui.cpp MatterEngine3/tests/viewer_logic_tests.cpp
git commit -m "refactor(viewer): replace raylib camera API"
```

### Task 5: Add Vulkan Device, Swapchain, and Validation Smoke Test

**Files:**
- Create: `MatterEngine3/include/matter/vulkan_device.h`
- Create: `MatterEngine3/src/render/vk_context.cpp`
- Create: `MatterEngine3/tests/vulkan_smoke_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile`, `MatterViewer/Makefile`

**Interfaces:**
- Produces: `VulkanDevice::create`, `begin_frame`, `end_frame`, `wait_idle`, native getters.

- [ ] **Step 1: Write a hidden-window three-frame smoke test**

```cpp
glfwInit(); glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API);
GLFWwindow* w=glfwCreateWindow(320,200,"vk-smoke",nullptr,nullptr);
std::string err; auto vk=VulkanDevice::create(w,true,err); CHECK(vk!=nullptr,err.c_str());
for(int i=0;i<3;++i){ VulkanFrame f{}; CHECK(vk->begin_frame(f,err),"begin frame"); CHECK(vk->end_frame(f,err),"end frame"); }
CHECK(vk->validation_error_count()==0,"no validation errors");
```

- [ ] **Step 2: Define the frame contract**

```cpp
struct VulkanFrame { VkCommandBuffer command_buffer=VK_NULL_HANDLE; uint32_t image_index=0; VkExtent2D extent{}; uint64_t serial=0; bool swapchain_recreated=false; };
class VulkanDevice {
public:
 static std::unique_ptr<VulkanDevice> create(GLFWwindow*,bool,std::string&);
 bool begin_frame(VulkanFrame&,std::string&); bool end_frame(const VulkanFrame&,std::string&);
 VkInstance instance() const; VkPhysicalDevice physical_device() const; VkDevice device() const;
 VkQueue graphics_queue() const; uint32_t graphics_queue_family() const;
};
```

- [ ] **Step 3: Create Vulkan 1.3 with validation and external-interoperability extensions**

Require swapchain, dynamic rendering, synchronization2, buffer device address, descriptor indexing, external memory/semaphore, and Win32 external-handle extensions. Report missing names. Use two frame slots, fences, image-available/render-finished semaphores, and recreate on out-of-date/suboptimal after nonzero framebuffer size.

- [ ] **Step 4: Add the standalone smoke-build target**

```makefile
WIN_AR ?= x86_64-w64-mingw32-ar
GLFW_SRC_DIR = ../Libraries/raylib/src/external/glfw/src
GLFW_WIN_SRC = context.c egl_context.c init.c input.c monitor.c null_init.c \
               null_joystick.c null_monitor.c null_window.c osmesa_context.c \
               platform.c vulkan.c wgl_context.c win32_init.c win32_joystick.c \
               win32_module.c win32_monitor.c win32_thread.c win32_time.c \
               win32_window.c window.c
GLFW_WIN_OBJ = $(addprefix build/windows/glfw/,$(GLFW_WIN_SRC:.c=.o))
GLFW_WIN_LIB = build/windows/libglfw3.a

build/windows/glfw/%.o: $(GLFW_SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(WIN_CC) -c $< -o $@ -D_GLFW_WIN32 -I../Libraries/raylib/src/external/glfw/include
$(GLFW_WIN_LIB): $(GLFW_WIN_OBJ)
	$(WIN_AR) rcs $@ $^

.PHONY: vulkan-smoke
vulkan-smoke: vulkan-preflight $(GLFW_WIN_LIB) build/windows/vulkan_smoke_tests.exe
```

At this task, `vulkan_smoke_tests.exe` links `vulkan_smoke_tests.cpp`, `vk_context.cpp`, `$(GLFW_WIN_LIB)`, and `-L$(VULKAN_LIB_DIR) -lvulkan-1`. Later tasks append their tested Vulkan modules to this target.

- [ ] **Step 5: Verify and commit**

Run:

```sh
make -C MatterViewer vulkan-smoke HAVE_CUDA=1
powershell.exe -NoProfile -Command '& .\MatterViewer\build\windows\vulkan_smoke_tests.exe'
```

Expected: selected adapter/driver printed, exit 0, zero validation errors.

```sh
git add MatterEngine3/include/matter/vulkan_device.h MatterEngine3/src/render/vk_context.cpp \
  MatterEngine3/tests/vulkan_smoke_tests.cpp MatterEngine3/tests/Makefile MatterViewer/Makefile
git commit -m "feat(vulkan): add validated device and swapchain"
```

### Task 6: Add Vulkan Resources, Pipelines, and SPIR-V Embedding

**Files:**
- Create: `MatterEngine3/src/render/vk_resources.h`
- Create: `MatterEngine3/src/render/vk_resources.cpp`
- Create: `MatterEngine3/src/render/vk_pipeline.h`
- Create: `MatterEngine3/src/render/vk_pipeline.cpp`
- Create: `MatterEngine3/tools/embed_spirv.py`
- Create: `MatterEngine3/shaders_vk/transform_probe.comp`
- Modify: `MatterEngine3/Makefile`, `MatterViewer/Makefile`, `MatterEngine3/tests/vulkan_smoke_tests.cpp`

**Interfaces:**
- Produces: `VkBufferResource`, `VkImageResource`, descriptors, transitions, upload/readback, embedded SPIR-V lookup.

- [ ] **Step 1: Add a failing CPU/GPU transform-probe test**

```cpp
Mat4f m=mat4_mul(mat4_translation({3,4,5}),mat4_rotation_y(0.5f));
Float4 in{1,2,3,1},out{},expected=transform(m,in);
auto close4=[](Float4 a,Float4 b,float e){return std::fabs(a.x-b.x)<=e&&std::fabs(a.y-b.y)<=e&&std::fabs(a.z-b.z)<=e&&std::fabs(a.w-b.w)<=e;};
run_transform_probe(*vk,pack_glsl_mat4(m),in,out);
CHECK(close4(out,expected,1e-5f),"CPU GPU transform parity");
```

- [ ] **Step 2: Add RAII resources**

```cpp
struct VkBufferResource { VkDevice device{}; VkBuffer buffer{}; VkDeviceMemory memory{}; VkDeviceSize size{}; VkDeviceAddress address{}; void* mapped{}; void reset(); };
struct VkImageResource { VkDevice device{}; VkImage image{}; VkImageView view{}; VkDeviceMemory memory{}; VkFormat format{}; VkExtent3D extent{}; VkImageLayout layout{}; void reset(); };
```

- [ ] **Step 3: Compile and embed SPIR-V**

```makefile
build/shaders_vk/%.spv: $(ME3_DIR)/shaders_vk/%
	@mkdir -p $(dir $@)
	$(GLSLC) --target-env=vulkan1.3 -O $< -o $@
$(ME3_DIR)/shaders_gen/embedded_spirv.h: $(VK_SPV) $(ME3_DIR)/tools/embed_spirv.py
	python3 $(ME3_DIR)/tools/embed_spirv.py $@ $(VK_SPV)
```

The embedder rejects inputs not divisible by four and emits `uint32_t` arrays plus `find_spirv(std::string_view)`.

- [ ] **Step 4: Verify and commit**

Run:

```sh
make -C MatterViewer vulkan-smoke HAVE_CUDA=1
powershell.exe -NoProfile -Command '$env:MATTER_VK_SMOKE_MODE="transform"; & .\MatterViewer\build\windows\vulkan_smoke_tests.exe'
```

Expected: CPU/GPU values match within `1e-5`.

```sh
git add MatterEngine3/src/render/vk_resources.h MatterEngine3/src/render/vk_resources.cpp \
  MatterEngine3/src/render/vk_pipeline.h MatterEngine3/src/render/vk_pipeline.cpp \
  MatterEngine3/tools/embed_spirv.py MatterEngine3/shaders_vk/transform_probe.comp \
  MatterEngine3/Makefile MatterViewer/Makefile MatterEngine3/tests/vulkan_smoke_tests.cpp
git commit -m "feat(vulkan): add resources pipelines and SPIR-V"
```

### Task 7: Port Scene Upload and GPU Culling

**Files:**
- Create: `MatterEngine3/src/render/vk_scene_renderer.h`
- Create: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Create: `MatterEngine3/shaders_vk/cull.comp`
- Modify: `MatterEngine3/src/render/gpu_cull_types.h`, `MatterEngine3/src/matter_engine.cpp`
- Test: `MatterEngine3/tests/vulkan_smoke_tests.cpp`

**Interfaces:**
- Produces: `ensure_part`, `release_part`, `update_instances`, `dispatch_culling`, `cull_stats`, `fill_rt_instances`.

- [ ] **Step 1: Add fixed-scene CPU/GPU parity tests**

```cpp
FixedCullScene s=make_fixed_cull_scene(); CpuCullResult cpu=run_cpu_cull(s); VkCullResult gpu=run_vk_cull(*vk,s);
CHECK(gpu.emitted==cpu.emitted,"emitted parity");
CHECK(gpu.frustum_culled==cpu.frustum_culled,"culled parity");
CHECK(gpu.commands==cpu.commands,"command parity");
```

Cover front, behind-camera, near-plane intersection, far-plane rejection, and translated instances. HiZ is off.

- [ ] **Step 2: Use Vulkan command layout and descriptor sets**

```cpp
struct DrawCommand { uint32_t vertex_count,instance_count,first_vertex,first_instance; };
static_assert(sizeof(DrawCommand)==sizeof(VkDrawIndirectCommand));
```

Set 0 binding 0 is `FrameConstants`; set 1 bindings 0..4 are clusters, instances, commands, draw transforms, stats. Port old culling math unchanged except canonical matrices and Vulkan depth.

- [ ] **Step 3: Add synchronization2 barrier**

```cpp
VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
b.srcStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; b.srcAccessMask=VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
b.dstStageMask=VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT|VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
b.dstAccessMask=VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT|VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
```

- [ ] **Step 4: Verify and commit**

Run: `make -C MatterViewer vulkan-smoke HAVE_CUDA=1`, then
`powershell.exe -NoProfile -Command '$env:MATTER_VK_SMOKE_MODE="cull"; & .\MatterViewer\build\windows\vulkan_smoke_tests.exe'`.
Expected: all counts match; validation clean.

```sh
git add MatterEngine3/src/render/vk_scene_renderer.h MatterEngine3/src/render/vk_scene_renderer.cpp \
  MatterEngine3/shaders_vk/cull.comp \
  MatterEngine3/src/render/gpu_cull_types.h MatterEngine3/src/matter_engine.cpp \
  MatterEngine3/tests/vulkan_smoke_tests.cpp
git commit -m "feat(vulkan): port GPU culling and scene buffers"
```

### Task 8: Port G-buffer Rasterization and Composite

**Files:**
- Create: `MatterEngine3/shaders_vk/raster.vert`
- Create: `MatterEngine3/shaders_vk/gbuffer.frag`
- Create: `MatterEngine3/shaders_vk/composite.vert`
- Create: `MatterEngine3/shaders_vk/composite.frag`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `MatterEngine3/src/matter_engine.cpp`
- Test: `MatterEngine3/tests/vulkan_smoke_tests.cpp`

**Interfaces:**
- Produces: albedo `R8G8B8A8`, normal `R16G16B16A16`, ORM `R8G8B8A8`, depth `D32`, HDR output, attachment readback.

- [ ] **Step 1: Add attachment and known-pixel tests**

```cpp
CHECK(g.albedo.format==VK_FORMAT_R8G8B8A8_UNORM,"albedo format");
CHECK(g.normal.format==VK_FORMAT_R16G16B16A16_SFLOAT,"normal format");
CHECK(g.depth.format==VK_FORMAT_D32_SFLOAT,"depth format"); KnownPixel p=renderer.readback_known_triangle_center();
CHECK(close3(p.normal,{0,1,0},2e-3f),"known normal"); CHECK(p.depth>=0&&p.depth<=1,"Vulkan depth range");
```

- [ ] **Step 2: Port vertex transform explicitly**

```glsl
layout(set=0,binding=0,std140) uniform FrameConstants { mat4 world_to_clip; } frame;
layout(set=1,binding=3,std430) readonly buffer DrawTransforms { mat4 transforms[]; };
void main(){ mat4 model=transforms[gl_InstanceIndex]; vec4 world=model*vec4(in_position,1); gl_Position=frame.world_to_clip*world; }
```

In Vulkan, `gl_InstanceIndex` already includes `firstInstance`; adding `gl_BaseInstance` would index the transform buffer twice.

- [ ] **Step 3: Use dynamic rendering and negative viewport height**

Transition attachments explicitly, use `VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL`, and preserve top-left framebuffer convention with negative viewport height. Do not flip projection or depth reconstruction.

- [ ] **Step 4: Verify and commit**

Run: `make -C MatterViewer vulkan-smoke HAVE_CUDA=1`, then
`powershell.exe -NoProfile -Command '$env:MATTER_VK_SMOKE_MODE="raster"; & .\MatterViewer\build\windows\vulkan_smoke_tests.exe'`.
Expected: structural pixels pass, validation clean.

```sh
git add MatterEngine3/shaders_vk/{raster.vert,gbuffer.frag,composite.vert,composite.frag} \
  MatterEngine3/src/render/vk_scene_renderer.h MatterEngine3/src/render/vk_scene_renderer.cpp \
  MatterEngine3/src/matter_engine.cpp \
  MatterEngine3/tests/vulkan_smoke_tests.cpp
git commit -m "feat(vulkan): port G-buffer and composite"
```

### Task 9: Switch MatterViewer and ImGui to Vulkan

**Files:**
- Modify: `MatterViewer/main.cpp`, `ui.h`, `ui.cpp`, `Makefile`
- Modify: `MatterEngine3/include/matter/engine_context.h`, `world_session.h`, `MatterEngine3/src/matter_engine.cpp`

**Interfaces:**
- Produces: direct GLFW window, Vulkan frame loop, ImGui Vulkan rendering, Vulkan screenshot readback.

- [ ] **Step 1: Replace raylib initialization**

```cpp
glfwInit(); glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API); glfwWindowHint(GLFW_RESIZABLE,GLFW_TRUE);
GLFWwindow* window=glfwCreateWindow(1280,720,"MatterEngine3 World Viewer",nullptr,nullptr);
std::string err; auto vulkan=matter::VulkanDevice::create(window,true,err);
matter::EngineDesc desc; desc.cache_root="cache"; desc.render_device=vulkan.get();
auto engine=matter::EngineContext::create(desc,err);
```

- [ ] **Step 2: Replace BeginDrawing/EndDrawing**

```cpp
VulkanFrame frame{};
if(vulkan->begin_frame(frame,err)) {
    session->render(camera,frame.extent.width,frame.extent.height,options);
    ui.begin_frame(); ui.draw_debug_panel(stats); ui.draw_camera_panel(camera);
    ui.end_frame(frame.command_buffer); vulkan->end_frame(frame,err);
}
```

- [ ] **Step 3: Initialize ImGui GLFW/Vulkan**

Use `ImGui_ImplGlfw_InitForVulkan`, `ImGui_ImplVulkan_Init`, dynamic rendering, the device's queue, descriptor pool, swapchain format, and image counts. On resize, call `ImGui_ImplVulkan_SetMinImageCount` after swapchain recreation.

- [ ] **Step 4: Replace raylib timing/input/cursor/screenshots**

Use `std::chrono`, GLFW input/cursor APIs, and `VkSceneRenderer::readback_swapchain_rgba8`. Preserve `MATTER_WORLD`, `MATTER_CAM`, `MATTER_SCREENSHOT`, FIFO commands, and three-frame settling.

- [ ] **Step 5: Verify and commit**

Run:

```sh
make -C MatterViewer windows HAVE_CUDA=1
powershell.exe -NoProfile -Command '$env:MATTER_WORLD="CornellBox"; $env:MATTER_SCREENSHOT="MatterViewer/vk-cornell-raster.png"; & .\MatterViewer\viewer.exe'
```

Expected: screenshot written and validation clean.

```sh
git add MatterViewer/main.cpp MatterViewer/ui.h MatterViewer/ui.cpp MatterViewer/Makefile \
  MatterEngine3/include/matter/engine_context.h MatterEngine3/include/matter/world_session.h \
  MatterEngine3/src/matter_engine.cpp
git commit -m "feat(viewer): switch window UI and presentation to Vulkan"
```

### Task 10: Prove CUDA-Vulkan External Interop

**Files:**
- Create: `MatterEngine3/src/render/vk_cuda_interop.h`
- Create: `MatterEngine3/src/render/vk_cuda_interop.cpp`
- Modify: `MatterEngine3/tests/vulkan_smoke_tests.cpp`, `MatterViewer/Makefile`

**Interfaces:**
- Produces: device matching, exported shared images, imported CUDA arrays/surfaces, exported semaphores.
- Updates the feature manifest to `CUDA_ACTIVE=1` while `OPTIX_ACTIVE=0` until
  Task 11 consumes the interop path.

- [ ] **Step 1: Add an interop round-trip test before changing OptiX**

```cpp
auto io=CudaVulkanInterop::create(*vk,err); auto image=io->create_shared_image({64,64},VK_FORMAT_R16G16B16A16_SFLOAT,err);
record_vk_clear_red(frame.command_buffer,image.vk_image()); io->cuda_wait(frame.serial);
launch_cuda_invert_rg(image.cuda_surface(),64,64); io->cuda_signal(frame.serial);
record_vk_readback(frame.command_buffer,image.vk_image()); CHECK_PIXEL(readback,32,32,{0,1,1,1},1e-3f);
```

- [ ] **Step 2: Match adapters by UUID/LUID**

Compare Vulkan `VkPhysicalDeviceIDProperties` against CUDA UUID/LUID before allocation. On mismatch return both device names and IDs.

- [ ] **Step 3: Export/import Win32 memory and semaphores**

Use opaque Win32 external-memory handles, `vkGetMemoryWin32HandleKHR`, `cuImportExternalMemory`, and `cuExternalMemoryGetMappedMipmappedArray`. Create exportable Vulkan timeline semaphores, export their opaque Win32 handles, and import them using CUDA's timeline-semaphore Win32 handle type so `frame.serial` is the signal/wait value. Verify timeline export support with `vkGetPhysicalDeviceExternalSemaphoreProperties` before device creation. Close OS handles immediately after successful import. Use CUDA async wait/signal; forbid `vkDeviceWaitIdle`, `glFinish`, and CPU polling in the frame path.

- [ ] **Step 4: Stress and commit**

Run: `make -C MatterViewer vulkan-smoke HAVE_CUDA=1`, then
`powershell.exe -NoProfile -Command '$env:MATTER_VK_SMOKE_MODE="interop"; $env:MATTER_VK_SMOKE_RESIZES="100"; & .\MatterViewer\build\windows\vulkan_smoke_tests.exe'`.

Expected: 100 cycles pass, zero validation errors, process handle count returns
within two handles of baseline, and the manifest reports `CUDA_ACTIVE=1` and
`OPTIX_ACTIVE=0`.

```sh
git add MatterEngine3/src/render/vk_cuda_interop.h MatterEngine3/src/render/vk_cuda_interop.cpp \
  MatterEngine3/tests/vulkan_smoke_tests.cpp MatterViewer/Makefile
git commit -m "feat(rt): add CUDA Vulkan external interop"
```

### Task 11: Port RtLighting From CUDA-OpenGL to CUDA-Vulkan

**Files:**
- Modify: `MatterEngine3/src/render/rt_lighting.h`
- Modify: `MatterEngine3/src/render/rt_lighting.cpp`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `MatterEngine3/src/render/shaders_rt/rt_params.h`
- Modify: `MatterEngine3/src/render/shaders_rt/lighting_raygen.cu`
- Modify: `MatterEngine3/src/matter_engine.cpp`
- Delete after porting assertions: `MatterEngine3/tests/test_rt_init.cpp`
- Delete after porting assertions: `MatterEngine3/tests/test_rt_blas.cpp`
- Delete after porting assertions: `MatterEngine3/tests/test_rt_tlas.cpp`
- Test: `MatterEngine3/tests/vulkan_smoke_tests.cpp`

**Interfaces:**
- Produces: `RtLighting::init(CudaVulkanInterop&,VkSceneRenderer&,err)`, `trace_lighting(FrameMatrices,...)`, Vulkan-sampleable output.

- [ ] **Step 1: Add interface and GL-symbol regressions**

```cpp
static_assert(std::is_same_v<decltype(&RtLighting::init),bool(RtLighting::*)(CudaVulkanInterop&,VkSceneRenderer&,std::string&)>);
CHECK(rt.trace_lighting(frame_matrices,lights,frame.serial),"RT trace succeeds");
CHECK(rt.output_image().layout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,"RT output layout");
```

Port the existing init/shutdown, BLAS registration, and three-instance TLAS assertions from `test_rt_init.cpp`, `test_rt_blas.cpp`, and `test_rt_tlas.cpp` into Windows smoke modes `rt-init`, `rt-blas`, and `rt-tlas`. Delete the old standalone files only after all three modes pass; they cannot construct the newly required Vulkan/CUDA harness.

- [ ] **Step 2: Replace the public surface**

```cpp
bool init(CudaVulkanInterop&,VkSceneRenderer&,std::string&);
void resize(VkExtent2D output,VkExtent2D trace);
bool trace_lighting(const FrameMatrices&,const RtLightParams&,uint64_t frame_serial);
const VkImageResource& output_image() const;
void reset_history();
```

Delete GL IDs, `CUgraphicsResource`, G-buffer FBO ownership, GL shader compilation, GL composite, and CUDA-GL registration.

- [ ] **Step 3: Share Vulkan G-buffer and output images**

Vulkan signals after raster; CUDA waits, runs OptiX/denoiser, and signals; Vulkan waits and samples the lighting output. Recreate imports on resize in CUDA-before-Vulkan destruction order.

- [ ] **Step 4: Use canonical depth reconstruction**

Pass `clip_to_world`, `world_to_clip`, `[0,1]` depth, and viewport extent. Because the raster pass uses a negative-height viewport, reconstruct clip as `(uv.x*2-1, 1-uv.y*2, depth, 1)`. Do not also flip the projection matrix. Add three GPU world-point reconstruction cases.

- [ ] **Step 5: Disable invalid moving-camera history**

If unjittered `world_to_clip` differs from the previous frame by more than `1e-6`, clear history and denoise current samples only. Stationary accumulation may remain.

- [ ] **Step 6: Verify and commit**

Run:

```sh
make -C MatterViewer vulkan-smoke HAVE_CUDA=1
powershell.exe -NoProfile -Command 'foreach($mode in "rt-init","rt-blas","rt-tlas","rt"){$env:MATTER_VK_SMOKE_MODE=$mode; & .\MatterViewer\build\windows\vulkan_smoke_tests.exe; if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}}'
x86_64-w64-mingw32-nm MatterViewer/viewer.exe | grep -E 'glFinish|cuGraphicsGL' && exit 1 || true
```

Expected: OptiX initializes, TLAS transforms pass, Vulkan samples RT output, no
CUDA-GL symbols, and the manifest reports `CUDA_ACTIVE=1` and `OPTIX_ACTIVE=1`.

```sh
git add MatterEngine3/src/render/rt_lighting.h MatterEngine3/src/render/rt_lighting.cpp \
  MatterEngine3/src/render/vk_scene_renderer.h MatterEngine3/src/render/vk_scene_renderer.cpp \
  MatterEngine3/src/render/shaders_rt/rt_params.h MatterEngine3/src/render/shaders_rt/lighting_raygen.cu \
  MatterEngine3/src/matter_engine.cpp MatterEngine3/tests/vulkan_smoke_tests.cpp
git add -u MatterEngine3/tests/test_rt_init.cpp MatterEngine3/tests/test_rt_blas.cpp MatterEngine3/tests/test_rt_tlas.cpp
git commit -m "feat(rt): port OptiX lighting to Vulkan interop"
```

### Task 12: Remove OpenGL From the Windows Build

**Files:**
- Modify: `MatterViewer/Makefile`
- Modify: `MatterEngine3/Makefile`
- Modify: `MatterEngine3/src/shader_source.cpp`
- Modify: `MatterEngine3/tools/embed_shaders.py`

**Interfaces:**
- Produces: Windows viewer linked to Vulkan and CUDA without `opengl32` or raylib graphics.

- [ ] **Step 1: Add a build audit**

```makefile
.PHONY: audit-windows-renderer
audit-windows-renderer: viewer.exe
	@! x86_64-w64-mingw32-nm viewer.exe | grep -Eq 'gl(Create|Bind|Finish)|cuGraphicsGL'
	@! x86_64-w64-mingw32-objdump -p viewer.exe | grep -qi 'opengl32.dll'
	@grep -qx 'OPENGL=0' build/windows/build_features.txt
```

- [ ] **Step 2: Replace source/backend/library lists**

Remove `renderer.cpp`, `raster_composer.cpp`, `gpu_culler.cpp`, `probe_texture.cpp`, and `tileset_gl_ctx.cpp` from `WIN_ME3_CPP`; add the matrix and Vulkan `.cpp` files. Replace `imgui_impl_opengl3.cpp` with `imgui_impl_vulkan.cpp`. Build vendored GLFW with `_GLFW_WIN32`. Replace `-lopengl32` and `libraylib.a` with:

```makefile
GLFW_WIN_SRC = context.c egl_context.c init.c input.c monitor.c null_init.c \
               null_joystick.c null_monitor.c null_window.c osmesa_context.c \
               platform.c vulkan.c wgl_context.c win32_init.c win32_joystick.c \
               win32_module.c win32_monitor.c win32_thread.c win32_time.c \
               win32_window.c window.c
WIN_LIBS = -L$(VULKAN_LIB_DIR) -lvulkan-1 -lgdi32 -lwinmm -luser32 -lshell32 \
           $(BOX3D_DIR)/build-mingw/libbox3d.a -lwinpthread
```

Run `rg -n 'InitWindow|BeginDrawing|EndDrawing|LoadImageFromScreen|UploadMesh|gl[A-Z]|cuGraphicsGL' MatterViewer MatterEngine3/src/render MatterEngine3/src/matter_engine.cpp` and remove every Windows-compiled call. Raylib POD-only headers may remain in non-render baking files, but the final link audit is authoritative: no raylib graphics object or OpenGL import may be pulled into `viewer.exe`.

- [ ] **Step 3: Restrict Windows shader generation to SPIR-V and OptiX PTX**

Keep legacy GLSL generation only for non-Windows work; it must not be a Windows viewer prerequisite.

- [ ] **Step 4: Verify and commit**

Run:

```sh
make -C MatterViewer clean
make -C MatterViewer windows HAVE_CUDA=1
make -C MatterViewer audit-windows-renderer HAVE_CUDA=1
make -C MatterEngine3/tests run-matrix run-partv2 run-script run-comp run-viewer-logic
```

Expected: viewer imports `vulkan-1.dll` and `nvcuda.dll`, not `opengl32.dll`; headless tests pass.

```sh
git add MatterViewer/Makefile MatterEngine3/Makefile MatterEngine3/src/shader_source.cpp MatterEngine3/tools/embed_shaders.py
git commit -m "build(viewer): remove OpenGL from Windows renderer"
```

### Task 13: Add Motion Acceptance Captures and Final CUDA Build Gate

**Files:**
- Create: `MatterViewer/tools/run_vulkan_acceptance.ps1`
- Create: `MatterViewer/tests/camera_paths/forward_back.json`, `strafe.json`, `yaw_pitch.json`
- Modify: `MatterViewer/main.cpp`, `ui.cpp`, `Makefile`

**Interfaces:**
- Produces: `MATTER_CAMERA_SCRIPT`, structural debug captures, final validation log, CUDA-enabled `viewer.exe`.

- [ ] **Step 1: Add fixed-timestep camera scripts**

```json
{"world":"CornellBox","frames":[
  {"frame":0,"position":[0,1,6],"target":[0,1,0]},
  {"frame":30,"position":[0,1,4],"target":[0,1,-2]},
  {"frame":60,"position":[0,1,6],"target":[0,1,0]}],
 "captures":[0,15,30,45,60]}
```

Interpolate position/target linearly, use fixed `1/60` timestep, and disable live input during playback.

- [ ] **Step 2: Add structural capture modes**

Support `MATTER_DEBUG_VIEW=color|depth|normal|material|lighting|cull`. Write a JSON sidecar containing camera, `world_to_clip`, cull counters, validation-error count, and build features.

- [ ] **Step 3: Add the PowerShell runner**

```powershell
$ErrorActionPreference='Stop'
$root=Resolve-Path "$PSScriptRoot\..\.."; $viewer=Join-Path $root 'viewer.exe'; $out=Join-Path $root 'acceptance'
New-Item -ItemType Directory -Force $out | Out-Null
foreach($path in 'forward_back','strafe','yaw_pitch') {
  $env:MATTER_CAMERA_SCRIPT=Join-Path $root "tests\camera_paths\$path.json"
  $env:MATTER_VALIDATION='1'; $env:MATTER_CAPTURE_DIR=Join-Path $out $path
  & $viewer; if($LASTEXITCODE -ne 0){throw "$path failed"}
}
if(Select-String -Path "$out\**\*.log" -Pattern 'VALIDATION ERROR'){throw 'validation errors found'}
```

- [ ] **Step 4: Perform the mandatory clean CUDA build**

```sh
make -C MatterViewer clean
make -C MatterViewer windows HAVE_CUDA=1
cat MatterViewer/build/windows/build_features.txt
x86_64-w64-mingw32-objdump -p MatterViewer/viewer.exe | grep -Ei 'vulkan-1.dll|nvcuda.dll|opengl32.dll'
```

Expected: manifest is exactly `VULKAN=1`, `OPENGL=0`, `CUDA_AVAILABLE=1`,
`OPTIX_AVAILABLE=1`, `CUDA_ACTIVE=1`, and `OPTIX_ACTIVE=1`; Vulkan and CUDA
DLLs appear; OpenGL does not.

- [ ] **Step 5: Run the complete gate**

```sh
make -C MatterEngine3/tests run-matrix run-partv2 run-script run-comp run-viewer-logic
make -C MatterViewer vulkan-smoke HAVE_CUDA=1
powershell.exe -NoProfile -Command 'foreach($mode in "rt-init","rt-blas","rt-tlas","rt"){$env:MATTER_VK_SMOKE_MODE=$mode; & .\MatterViewer\build\windows\vulkan_smoke_tests.exe; if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}}'
powershell.exe -NoProfile -ExecutionPolicy Bypass -File MatterViewer/tools/run_vulkan_acceptance.ps1
make -C MatterViewer audit-windows-renderer HAVE_CUDA=1
```

Expected: all tests and camera paths pass, validation is clean, structural captures show correct forward/back flow, and RT lighting has no moving-camera history edges.

- [ ] **Step 6: Commit**

```sh
git add MatterViewer/tools/run_vulkan_acceptance.ps1 MatterViewer/main.cpp MatterViewer/ui.cpp \
  MatterViewer/Makefile MatterViewer/tests/camera_paths
git commit -m "test(vulkan): add motion and CUDA acceptance gates"
```

---

## Phase 1 Completion Gate

- Canonical matrix, GPU transform, and Vulkan frustum tests pass.
- Persisted transforms remain byte-compatible with translation `[3,7,11]`.
- Windows viewer uses GLFW/Vulkan/ImGui Vulkan and imports no OpenGL DLL.
- GPU culling and G-buffer structural tests pass with validation enabled.
- CUDA/Vulkan external memory and semaphore stress tests pass.
- OptiX lighting works through Vulkan and resets history during camera motion.
- Forward/back, strafe, and yaw/pitch captures show no reversed flow or invented temporal edges.
- Clean `make -C MatterViewer windows HAVE_CUDA=1` succeeds and, after the
  Task 10/11 activation work, reports `VULKAN=1`, `OPENGL=0`,
  `CUDA_AVAILABLE=1`, `OPTIX_AVAILABLE=1`, `CUDA_ACTIVE=1`, and
  `OPTIX_ACTIVE=1`.
- Relevant headless MatterEngine3 tests pass.

After this gate, create separate plans in order:

1. Phase 2: previous transforms, jitter, dense motion vectors, debug views.
2. Phase 3: reference TAA and validated history rejection.
3. Phase 4: Streamline DLSS Super Resolution/DLAA.
4. Optional Phase 5: DLSS Ray Reconstruction and Frame Generation.
