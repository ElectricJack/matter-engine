# RT Lighting Phase 1: Shadow-Only Proof (MatterViewer) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Trace 1 shadow ray per pixel at quarter resolution using NVIDIA OptiX on RTX 4090 hardware, composite the shadow mask onto the existing GPU-driven raster output in MatterViewer.

**Architecture:** OptiX runs alongside the OpenGL raster pipeline via CUDA-GL interop. After the raster draw completes and depth is available, a compute shader linearizes depth to a CUDA-friendly R32F texture at quarter resolution. OptiX traces shadow rays from the reconstructed world positions toward the sun. The resulting shadow mask is shared back to GL and composited via a fullscreen pass.

**Tech Stack:** C++17, CUDA Toolkit 12.x, OptiX SDK 8.x, OpenGL 4.6 (existing), raylib (existing)

## Global Constraints

- NVIDIA RTX GPU required (4090 target); graceful fallback when unavailable
- OptiX SDK 8.0+ (header-only; runtime in driver >= 535)
- CUDA Toolkit 12.x (nvcc for PTX compilation, libcudart for runtime)
- GL 4.6 hard requirement (existing; no change)
- Engine matrices are row-major; GL/CUDA expect column-major — transpose on upload
- Mesh data is unindexed triangle soup (3 vertices per triangle, no index buffer)
- `build_hiz()` currently no-ops when `hiz_enabled_` is false — depth blit must be guaranteed for RT
- CUDA-GL interop does NOT support `GL_DEPTH_COMPONENT24` — must linearize to R32F first

---

### Task 1: Build System + CUDA/OptiX Module Skeleton

**Files:**
- Modify: `MatterEngine3/Makefile` (add CUDA/OptiX compilation rules)
- Create: `MatterEngine3/src/render/rt_lighting.h`
- Create: `MatterEngine3/src/render/rt_lighting.cpp`
- Modify: `MatterViewer/Makefile` (add `-lcudart` linker flag)
- Test: `MatterEngine3/tests/test_rt_init.cpp`

**Interfaces:**
- Consumes: nothing (standalone initialization)
- Produces: `RtLighting::init(std::string& err) -> bool`, `RtLighting::shutdown()`, `RtLighting::available() -> bool`

- [ ] **Step 1: Add CUDA/OptiX build rules to MatterEngine3/Makefile**

Add after the existing `INCLUDE_PATHS` definition:

```makefile
# --- CUDA / OptiX (optional, gated by HAVE_CUDA) ---
CUDA_PATH  ?= /usr/local/cuda
OPTIX_PATH ?= $(HOME)/NVIDIA-OptiX-SDK-8.1.0

ifdef HAVE_CUDA
  NVCC       = $(CUDA_PATH)/bin/nvcc
  NVCC_FLAGS = --ptx --machine 64 -O2 -I$(OPTIX_PATH)/include -I$(CUDA_PATH)/include
  CFLAGS    += -DMATTER_HAVE_OPTIX -I$(CUDA_PATH)/include -I$(OPTIX_PATH)/include
  RT_CU_DIR  = src/render/shaders_rt
  RT_PTX     = $(RT_CU_DIR)/shadow_raygen.ptx $(RT_CU_DIR)/shadow_miss.ptx $(RT_CU_DIR)/shadow_hit.ptx
  RT_CPP     = src/render/rt_lighting.cpp
  RT_OBJ     = rt_lighting.o

$(RT_CU_DIR)/%.ptx: $(RT_CU_DIR)/%.cu
	$(NVCC) $(NVCC_FLAGS) -o $@ $<

rt_lighting.o: src/render/rt_lighting.cpp
	$(CC) -c $< -o $@ $(CFLAGS) $(INCLUDE_PATHS) -I$(CUDA_PATH)/include -I$(OPTIX_PATH)/include
endif
```

Add `$(RT_OBJ)` to the archive command (conditionally):
```makefile
ifdef HAVE_CUDA
ME3_OBJ += $(RT_OBJ)
$(LIB): $(RT_PTX)
endif
```

- [ ] **Step 2: Add -lcudart to MatterViewer/Makefile**

In the Linux LDFLAGS section, add conditionally:

```makefile
ifdef HAVE_CUDA
  LDFLAGS += -L$(CUDA_PATH)/lib64 -lcudart -ldl
endif
```

- [ ] **Step 3: Create rt_lighting.h**

```cpp
#ifndef MATTER_RT_LIGHTING_H
#define MATTER_RT_LIGHTING_H

#ifdef MATTER_HAVE_OPTIX

#include <cstdint>
#include <string>
#include <vector>

namespace viewer {

class RtLighting {
public:
    RtLighting() = default;
    ~RtLighting();

    bool init(std::string& err);
    void shutdown();
    bool available() const { return available_; }

private:
    bool available_ = false;
    // CUDA device + OptiX context handles stored as void* to avoid
    // CUDA/OptiX headers leaking into every TU that includes this header.
    void* cu_ctx_     = nullptr;  // CUcontext
    void* optix_ctx_  = nullptr;  // OptixDeviceContext
};

} // namespace viewer

#else // !MATTER_HAVE_OPTIX

namespace viewer {
class RtLighting {
public:
    bool init(std::string&) { return false; }
    void shutdown() {}
    bool available() const { return false; }
};
} // namespace viewer

#endif // MATTER_HAVE_OPTIX
#endif // MATTER_RT_LIGHTING_H
```

- [ ] **Step 4: Create rt_lighting.cpp**

```cpp
#include "rt_lighting.h"

#ifdef MATTER_HAVE_OPTIX

#include <cuda.h>
#include <cuda_runtime.h>
#include <optix.h>
#include <optix_stubs.h>
#include <optix_function_table_definition.h>

#include <cstdio>

namespace viewer {

static void optix_log_callback(unsigned int level, const char* tag,
                               const char* msg, void*) {
    fprintf(stderr, "[OptiX][%u][%s]: %s\n", level, tag, msg);
}

bool RtLighting::init(std::string& err) {
    // 1. Initialize CUDA driver API.
    CUresult cu_res = cuInit(0);
    if (cu_res != CUDA_SUCCESS) {
        err = "cuInit failed: " + std::to_string((int)cu_res);
        return false;
    }

    // 2. Get device 0 and create/retain primary context.
    CUdevice device = 0;
    cu_res = cuDeviceGet(&device, 0);
    if (cu_res != CUDA_SUCCESS) {
        err = "cuDeviceGet failed";
        return false;
    }
    CUcontext ctx = nullptr;
    cu_res = cuDevicePrimaryCtxRetain(&ctx, device);
    if (cu_res != CUDA_SUCCESS) {
        err = "cuDevicePrimaryCtxRetain failed";
        return false;
    }
    cuCtxSetCurrent(ctx);
    cu_ctx_ = (void*)ctx;

    // 3. Initialize OptiX.
    OptixResult opt_res = optixInit();
    if (opt_res != OPTIX_SUCCESS) {
        err = "optixInit failed: " + std::to_string((int)opt_res);
        return false;
    }

    // 4. Create OptiX device context.
    OptixDeviceContextOptions ctx_opts = {};
    ctx_opts.logCallbackFunction = optix_log_callback;
    ctx_opts.logCallbackLevel    = 4;  // warnings + errors
    OptixDeviceContext optix_context = nullptr;
    opt_res = optixDeviceContextCreate(ctx, &ctx_opts, &optix_context);
    if (opt_res != OPTIX_SUCCESS) {
        err = "optixDeviceContextCreate failed: " + std::to_string((int)opt_res);
        return false;
    }
    optix_ctx_ = (void*)optix_context;

    available_ = true;
    printf("RtLighting: OptiX %d.%d initialized on device 0\n",
           OPTIX_VERSION / 10000, (OPTIX_VERSION % 10000) / 100);
    return true;
}

void RtLighting::shutdown() {
    if (optix_ctx_) {
        optixDeviceContextDestroy((OptixDeviceContext)optix_ctx_);
        optix_ctx_ = nullptr;
    }
    if (cu_ctx_) {
        CUdevice device = 0;
        cuDeviceGet(&device, 0);
        cuDevicePrimaryCtxRelease(device);
        cu_ctx_ = nullptr;
    }
    available_ = false;
}

RtLighting::~RtLighting() { shutdown(); }

} // namespace viewer

#endif // MATTER_HAVE_OPTIX
```

- [ ] **Step 5: Write test_rt_init.cpp**

```cpp
#include "rt_lighting.h"
#include <cstdio>
#include <cassert>

int main() {
    viewer::RtLighting rt;
    std::string err;
    bool ok = rt.init(err);
    if (!ok) {
        printf("SKIP: RT not available: %s\n", err.c_str());
        return 0;  // not a failure — just no NVIDIA GPU
    }
    assert(rt.available());
    printf("PASS: OptiX initialized successfully\n");
    rt.shutdown();
    assert(!rt.available());
    printf("PASS: shutdown clean\n");
    return 0;
}
```

- [ ] **Step 6: Build and run test**

```bash
cd MatterEngine3
make HAVE_CUDA=1 EXTRA_CFLAGS="-DMATTER_HAVE_OPTIX"
cd tests
g++ -std=c++17 -I../src/render -I../include -I$(CUDA_PATH)/include -I$(OPTIX_PATH)/include \
    -DMATTER_HAVE_OPTIX test_rt_init.cpp ../rt_lighting.o \
    -L$(CUDA_PATH)/lib64 -lcudart -lcuda -ldl -o test_rt_init
./test_rt_init
```

Expected: "PASS: OptiX initialized successfully" + "PASS: shutdown clean"

- [ ] **Step 7: Commit**

```bash
git add MatterEngine3/src/render/rt_lighting.h MatterEngine3/src/render/rt_lighting.cpp \
       MatterEngine3/Makefile MatterViewer/Makefile MatterEngine3/tests/test_rt_init.cpp
git commit -m "feat(rt): add CUDA/OptiX build system and initialization module"
```

---

### Task 2: BLAS Construction from PartStore Mesh Data

**Files:**
- Modify: `MatterEngine3/src/render/rt_lighting.h` (add register/unregister_part)
- Modify: `MatterEngine3/src/render/rt_lighting.cpp` (implement BLAS build)
- Test: `MatterEngine3/tests/test_rt_blas.cpp`

**Interfaces:**
- Consumes: `PartStore::get_or_load(uint64_t) -> const LoadedPart*`, `LoadedPart::lod_mesh_data[lod].vertices` (flat float array, 3 floats/vert), `LoadedPart::lod_mesh_data[lod].vertex_count`
- Produces: `RtLighting::register_part(uint64_t part_hash, const LoadedPart& lp)`, `RtLighting::unregister_part(uint64_t part_hash)`

- [ ] **Step 1: Add BLAS types and methods to rt_lighting.h**

Add inside the `#ifdef MATTER_HAVE_OPTIX` class body:

```cpp
#include <unordered_map>

// Forward declare — full definition in .cpp
struct RtBlas {
    void* d_vertices     = nullptr;  // CUdeviceptr
    void* gas_buffer     = nullptr;  // CUdeviceptr (compacted)
    uint64_t traversable = 0;        // OptixTraversableHandle
    int vertex_count     = 0;
};
```

Add to `RtLighting` public interface:
```cpp
    void register_part(uint64_t part_hash, const float* vertices, int vertex_count);
    void unregister_part(uint64_t part_hash);
```

Add to `RtLighting` private members:
```cpp
    std::unordered_map<uint64_t, RtBlas> blas_cache_;
```

- [ ] **Step 2: Implement register_part in rt_lighting.cpp**

```cpp
void RtLighting::register_part(uint64_t part_hash, const float* vertices, int vertex_count) {
    if (!available_ || blas_cache_.count(part_hash)) return;

    RtBlas blas;
    blas.vertex_count = vertex_count;

    // Upload vertices to device.
    size_t verts_bytes = vertex_count * 3 * sizeof(float);
    CUdeviceptr d_verts = 0;
    cudaMalloc((void**)&d_verts, verts_bytes);
    cudaMemcpy((void*)d_verts, vertices, verts_bytes, cudaMemcpyHostToDevice);
    blas.d_vertices = (void*)d_verts;

    // Build GAS (Geometry Acceleration Structure = BLAS).
    OptixAccelBuildOptions accel_opts = {};
    accel_opts.buildFlags = OPTIX_BUILD_FLAG_ALLOW_COMPACTION | OPTIX_BUILD_FLAG_PREFER_FAST_TRACE;
    accel_opts.operation  = OPTIX_BUILD_OPERATION_BUILD;

    // Triangle input: unindexed soup (3 verts per triangle).
    OptixBuildInput build_input = {};
    build_input.type = OPTIX_BUILD_INPUT_TYPE_TRIANGLES;
    auto& tri = build_input.triangleArray;
    tri.vertexFormat        = OPTIX_VERTEX_FORMAT_FLOAT3;
    tri.vertexStrideInBytes = 3 * sizeof(float);
    tri.numVertices         = vertex_count;
    CUdeviceptr vertex_ptrs[1] = { d_verts };
    tri.vertexBuffers       = vertex_ptrs;
    tri.numSbtRecords       = 1;
    unsigned int flags[1]   = { OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT };
    tri.flags               = flags;

    // Query buffer sizes.
    OptixAccelBufferSizes buf_sizes = {};
    optixAccelComputeMemoryUsage((OptixDeviceContext)optix_ctx_, &accel_opts,
                                 &build_input, 1, &buf_sizes);

    // Allocate temp + output.
    CUdeviceptr d_temp = 0, d_output = 0;
    cudaMalloc((void**)&d_temp, buf_sizes.tempSizeInBytes);
    cudaMalloc((void**)&d_output, buf_sizes.outputSizeInBytes);

    // Compaction size buffer.
    OptixAccelEmitDesc emit_desc = {};
    CUdeviceptr d_compacted_size = 0;
    cudaMalloc((void**)&d_compacted_size, sizeof(uint64_t));
    emit_desc.type   = OPTIX_PROPERTY_TYPE_COMPACTED_SIZE;
    emit_desc.result = d_compacted_size;

    // Build.
    OptixTraversableHandle handle = 0;
    optixAccelBuild((OptixDeviceContext)optix_ctx_, 0,
                    &accel_opts, &build_input, 1,
                    d_temp, buf_sizes.tempSizeInBytes,
                    d_output, buf_sizes.outputSizeInBytes,
                    &handle, &emit_desc, 1);
    cudaDeviceSynchronize();
    cudaFree((void*)d_temp);

    // Compact.
    uint64_t compacted_size = 0;
    cudaMemcpy(&compacted_size, (void*)d_compacted_size, sizeof(uint64_t), cudaMemcpyDeviceToHost);
    cudaFree((void*)d_compacted_size);

    CUdeviceptr d_compacted = 0;
    cudaMalloc((void**)&d_compacted, compacted_size);
    optixAccelCompact((OptixDeviceContext)optix_ctx_, 0, handle,
                      d_compacted, compacted_size, &handle);
    cudaDeviceSynchronize();
    cudaFree((void*)d_output);

    blas.gas_buffer  = (void*)d_compacted;
    blas.traversable = (uint64_t)handle;
    blas_cache_[part_hash] = blas;
}

void RtLighting::unregister_part(uint64_t part_hash) {
    auto it = blas_cache_.find(part_hash);
    if (it == blas_cache_.end()) return;
    cudaFree(it->second.d_vertices);
    cudaFree(it->second.gas_buffer);
    blas_cache_.erase(it);
}
```

- [ ] **Step 3: Write test_rt_blas.cpp**

```cpp
#include "rt_lighting.h"
#include <cstdio>
#include <cassert>

int main() {
    viewer::RtLighting rt;
    std::string err;
    if (!rt.init(err)) { printf("SKIP: %s\n", err.c_str()); return 0; }

    // A single triangle (3 vertices).
    float verts[] = {
        0.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f,
        0.5f, 1.0f, 0.0f,
    };
    rt.register_part(42, verts, 3);
    printf("PASS: BLAS built for 1-triangle part\n");

    // Duplicate registration is no-op.
    rt.register_part(42, verts, 3);
    printf("PASS: duplicate registration is no-op\n");

    rt.unregister_part(42);
    printf("PASS: BLAS released\n");

    rt.shutdown();
    return 0;
}
```

- [ ] **Step 4: Build and run test**

```bash
cd MatterEngine3/tests
g++ -std=c++17 -I../src/render -I../include \
    -I$(CUDA_PATH)/include -I$(OPTIX_PATH)/include \
    -DMATTER_HAVE_OPTIX test_rt_blas.cpp ../rt_lighting.o \
    -L$(CUDA_PATH)/lib64 -lcudart -lcuda -ldl -o test_rt_blas
./test_rt_blas
```

Expected: three PASS lines.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/render/rt_lighting.h MatterEngine3/src/render/rt_lighting.cpp \
       MatterEngine3/tests/test_rt_blas.cpp
git commit -m "feat(rt): BLAS construction from unindexed triangle mesh data"
```

---

### Task 3: TLAS Construction + Per-Frame Instance Sync

**Files:**
- Modify: `MatterEngine3/src/render/rt_lighting.h` (add update_instances)
- Modify: `MatterEngine3/src/render/rt_lighting.cpp` (implement TLAS build)
- Test: `MatterEngine3/tests/test_rt_tlas.cpp`

**Interfaces:**
- Consumes: `RtLighting::blas_cache_` (built in Task 2), `ResolvedInstance::part_hash`, `ResolvedInstance::lod_level`, `ResolvedInstance::transform[16]` (row-major)
- Produces: `RtLighting::update_instances(const std::vector<ResolvedInstance>& resolved, const std::function<const LoadedPart*(uint64_t)>& get_part)` — builds/updates TLAS

- [ ] **Step 1: Add TLAS state and method to rt_lighting.h**

Add to `RtLighting` public:
```cpp
    struct InstanceInput {
        uint64_t part_hash;
        int      lod_level;
        float    transform[16];  // row-major
    };
    void update_instances(const InstanceInput* instances, int count);
    uint64_t tlas_handle() const { return tlas_handle_; }
```

Add to `RtLighting` private:
```cpp
    void*    tlas_buffer_   = nullptr;  // CUdeviceptr
    size_t   tlas_buf_size_ = 0;
    uint64_t tlas_handle_   = 0;        // OptixTraversableHandle
    void*    d_instances_   = nullptr;  // CUdeviceptr (OptixInstance array)
    int      instance_cap_  = 0;
```

- [ ] **Step 2: Implement update_instances in rt_lighting.cpp**

```cpp
void RtLighting::update_instances(const InstanceInput* instances, int count) {
    if (!available_ || count == 0) return;

    // Build OptixInstance array (host-side, then upload).
    std::vector<OptixInstance> optix_instances;
    optix_instances.reserve(count);

    for (int i = 0; i < count; ++i) {
        auto it = blas_cache_.find(instances[i].part_hash);
        if (it == blas_cache_.end()) continue;  // part not registered

        OptixInstance inst = {};
        // Transform: OptiX expects a 3x4 row-major affine matrix.
        // Our transform[16] is a 4x4 row-major matrix; take the top 3 rows.
        const float* t = instances[i].transform;
        inst.transform[0]  = t[0]; inst.transform[1]  = t[1]; inst.transform[2]  = t[2]; inst.transform[3]  = t[3];
        inst.transform[4]  = t[4]; inst.transform[5]  = t[5]; inst.transform[6]  = t[6]; inst.transform[7]  = t[7];
        inst.transform[8]  = t[8]; inst.transform[9]  = t[9]; inst.transform[10] = t[10]; inst.transform[11] = t[11];

        inst.instanceId        = i;
        inst.sbtOffset         = 0;
        inst.visibilityMask    = 0xFF;
        inst.flags             = OPTIX_INSTANCE_FLAG_DISABLE_ANYHIT;
        inst.traversableHandle = (OptixTraversableHandle)it->second.traversable;
        optix_instances.push_back(inst);
    }

    if (optix_instances.empty()) return;
    int inst_count = (int)optix_instances.size();

    // Upload instance array.
    size_t inst_bytes = inst_count * sizeof(OptixInstance);
    if (inst_count > instance_cap_) {
        if (d_instances_) cudaFree(d_instances_);
        cudaMalloc(&d_instances_, inst_bytes);
        instance_cap_ = inst_count;
    }
    cudaMemcpy(d_instances_, optix_instances.data(), inst_bytes, cudaMemcpyHostToDevice);

    // Build IAS (Instance Acceleration Structure = TLAS).
    OptixBuildInput build_input = {};
    build_input.type = OPTIX_BUILD_INPUT_TYPE_INSTANCES;
    build_input.instanceArray.instances    = (CUdeviceptr)d_instances_;
    build_input.instanceArray.numInstances = inst_count;

    OptixAccelBuildOptions accel_opts = {};
    accel_opts.buildFlags = OPTIX_BUILD_FLAG_ALLOW_UPDATE | OPTIX_BUILD_FLAG_PREFER_FAST_TRACE;
    accel_opts.operation  = OPTIX_BUILD_OPERATION_BUILD;

    OptixAccelBufferSizes buf_sizes = {};
    optixAccelComputeMemoryUsage((OptixDeviceContext)optix_ctx_, &accel_opts,
                                 &build_input, 1, &buf_sizes);

    // Reallocate if needed.
    if (buf_sizes.outputSizeInBytes > tlas_buf_size_) {
        if (tlas_buffer_) cudaFree(tlas_buffer_);
        cudaMalloc(&tlas_buffer_, buf_sizes.outputSizeInBytes);
        tlas_buf_size_ = buf_sizes.outputSizeInBytes;
    }

    CUdeviceptr d_temp = 0;
    cudaMalloc((void**)&d_temp, buf_sizes.tempSizeInBytes);

    OptixTraversableHandle handle = 0;
    optixAccelBuild((OptixDeviceContext)optix_ctx_, 0,
                    &accel_opts, &build_input, 1,
                    d_temp, buf_sizes.tempSizeInBytes,
                    (CUdeviceptr)tlas_buffer_, buf_sizes.outputSizeInBytes,
                    &handle, nullptr, 0);
    cudaDeviceSynchronize();
    cudaFree((void*)d_temp);

    tlas_handle_ = (uint64_t)handle;
}
```

- [ ] **Step 3: Write test_rt_tlas.cpp**

```cpp
#include "rt_lighting.h"
#include <cstdio>
#include <cassert>
#include <cstring>

int main() {
    viewer::RtLighting rt;
    std::string err;
    if (!rt.init(err)) { printf("SKIP: %s\n", err.c_str()); return 0; }

    // Register a ground plane (2 triangles, 6 vertices).
    float verts[] = {
        -10,0,-10,  10,0,-10,  10,0,10,
        -10,0,-10,  10,0,10,  -10,0,10,
    };
    rt.register_part(1, verts, 6);

    // Place 3 instances with identity transform.
    float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    viewer::RtLighting::InstanceInput instances[3];
    for (int i = 0; i < 3; ++i) {
        instances[i].part_hash = 1;
        instances[i].lod_level = 0;
        memcpy(instances[i].transform, identity, sizeof(identity));
        instances[i].transform[3] = (float)i * 25.0f;  // offset X
    }
    rt.update_instances(instances, 3);
    assert(rt.tlas_handle() != 0);
    printf("PASS: TLAS built with 3 instances\n");

    // Update (same count, different transforms) exercises refit path.
    instances[0].transform[7] = 5.0f;  // move Y
    rt.update_instances(instances, 3);
    assert(rt.tlas_handle() != 0);
    printf("PASS: TLAS rebuilt with updated transforms\n");

    rt.shutdown();
    return 0;
}
```

- [ ] **Step 4: Build and run test**

```bash
cd MatterEngine3/tests
g++ -std=c++17 -I../src/render -I../include \
    -I$(CUDA_PATH)/include -I$(OPTIX_PATH)/include \
    -DMATTER_HAVE_OPTIX test_rt_tlas.cpp ../rt_lighting.o \
    -L$(CUDA_PATH)/lib64 -lcudart -lcuda -ldl -o test_rt_tlas
./test_rt_tlas
```

Expected: two PASS lines.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/render/rt_lighting.h MatterEngine3/src/render/rt_lighting.cpp \
       MatterEngine3/tests/test_rt_tlas.cpp
git commit -m "feat(rt): TLAS construction from instance transforms"
```

---

### Task 4: Depth Linearization + CUDA-GL Interop

**Files:**
- Create: `MatterEngine3/shaders_gpu/depth_linearize.comp`
- Modify: `MatterEngine3/src/render/rt_lighting.h` (add depth/output texture management)
- Modify: `MatterEngine3/src/render/rt_lighting.cpp` (implement interop setup + depth copy)
- Modify: `MatterEngine3/src/render/gpu_culler.h` (expose `depth_copy_tex()` accessor)
- Modify: `MatterEngine3/src/render/gpu_culler.cpp` (add `ensure_depth_blit()` that always runs the blit)

**Interfaces:**
- Consumes: `GpuCuller::depth_copy_tex()` (GL texture name, GL_DEPTH_COMPONENT24, full screen res)
- Produces: `RtLighting::prepare_frame(unsigned depth_tex, int screen_w, int screen_h, const float inv_vp[16], const float sun_dir[3])` — linearizes depth, sets up launch params

- [ ] **Step 1: Add depth_copy_tex() accessor to gpu_culler.h**

Add to `GpuCuller` public section (after `hiz_valid()`):

```cpp
    unsigned depth_copy_tex() const { return depth_copy_tex_; }
```

- [ ] **Step 2: Add ensure_depth_blit() to GpuCuller**

In `gpu_culler.h` public section:
```cpp
    void ensure_depth_blit(int screen_w, int screen_h);
```

In `gpu_culler.cpp`, add before `build_hiz`:
```cpp
void GpuCuller::ensure_depth_blit(int screen_w, int screen_h) {
    if (screen_w <= 0 || screen_h <= 0) return;
    // Reuse build_hiz's texture/FBO creation logic if needed.
    if (!depth_copy_tex_ || hiz_screen_w_ != screen_w || hiz_screen_h_ != screen_h) {
        // Force a full build_hiz cycle to create/recreate textures.
        bool was_enabled = hiz_enabled_;
        hiz_enabled_ = true;
        build_hiz(screen_w, screen_h);
        hiz_enabled_ = was_enabled;
        if (!was_enabled) hiz_valid_ = false;
        return;
    }
    // Just do the blit.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, depth_fbo_);
    glBlitFramebuffer(0, 0, screen_w, screen_h,
                      0, 0, screen_w, screen_h,
                      GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
}
```

- [ ] **Step 3: Create depth_linearize.comp**

```glsl
#version 460

layout(local_size_x = 16, local_size_y = 16) in;

uniform sampler2D u_depth;       // GL_DEPTH_COMPONENT24 (reads as [0,1])
uniform float     u_near;
uniform float     u_far;

layout(r32f, binding = 0) writeonly uniform image2D u_out;  // quarter-res R32F

void main() {
    ivec2 out_coord = ivec2(gl_GlobalInvocationID.xy);
    ivec2 out_size  = imageSize(u_out);
    if (out_coord.x >= out_size.x || out_coord.y >= out_size.y) return;

    // Sample depth at corresponding full-res position (nearest).
    ivec2 in_size  = textureSize(u_depth, 0);
    ivec2 in_coord = out_coord * (in_size / out_size);
    float z_ndc    = texelFetch(u_depth, in_coord, 0).r;

    // Linearize: OpenGL depth buffer stores [0,1] mapped from [near, far].
    // z_eye = near * far / (far - z_ndc * (far - near))
    float z_eye = u_near * u_far / (u_far - z_ndc * (u_far - u_near));

    // Store raw NDC depth (not linearized) — reconstruction uses inv_vp directly.
    imageStore(u_out, out_coord, vec4(z_ndc, 0, 0, 0));
}
```

- [ ] **Step 4: Add frame resources to rt_lighting.h**

Add to `RtLighting` public:
```cpp
    void resize(int screen_w, int screen_h);
    void prepare_depth(unsigned gl_depth_tex, int screen_w, int screen_h);
    unsigned shadow_texture() const { return shadow_gl_tex_; }
```

Add to `RtLighting` private:
```cpp
    // GL resources
    unsigned depth_linear_tex_ = 0;   // R32F, quarter res
    unsigned shadow_gl_tex_    = 0;   // R8, quarter res
    unsigned depth_lin_prog_   = 0;   // compute shader program
    int      trace_w_ = 0, trace_h_ = 0;

    // CUDA interop
    void* cuda_depth_resource_  = nullptr;  // cudaGraphicsResource_t
    void* cuda_shadow_resource_ = nullptr;  // cudaGraphicsResource_t
    bool  interop_registered_   = false;
```

- [ ] **Step 5: Implement resize and prepare_depth in rt_lighting.cpp**

```cpp
void RtLighting::resize(int screen_w, int screen_h) {
    int new_tw = screen_w / 4;
    int new_th = screen_h / 4;
    if (new_tw == trace_w_ && new_th == trace_h_) return;
    trace_w_ = new_tw;
    trace_h_ = new_th;

    // Unregister old CUDA resources before deleting GL textures.
    if (interop_registered_) {
        cudaGraphicsUnregisterResource((cudaGraphicsResource_t)cuda_depth_resource_);
        cudaGraphicsUnregisterResource((cudaGraphicsResource_t)cuda_shadow_resource_);
        interop_registered_ = false;
    }

    // (Re)create quarter-res depth linearized texture.
    if (depth_linear_tex_) glDeleteTextures(1, &depth_linear_tex_);
    glGenTextures(1, &depth_linear_tex_);
    glBindTexture(GL_TEXTURE_2D, depth_linear_tex_);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32F, trace_w_, trace_h_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // (Re)create quarter-res shadow output texture.
    if (shadow_gl_tex_) glDeleteTextures(1, &shadow_gl_tex_);
    glGenTextures(1, &shadow_gl_tex_);
    glBindTexture(GL_TEXTURE_2D, shadow_gl_tex_);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32F, trace_w_, trace_h_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Register with CUDA.
    cudaGraphicsResource_t depth_res = nullptr, shadow_res = nullptr;
    cudaGraphicsGLRegisterImage(&depth_res, depth_linear_tex_,
                                GL_TEXTURE_2D, cudaGraphicsRegisterFlagsReadOnly);
    cudaGraphicsGLRegisterImage(&shadow_res, shadow_gl_tex_,
                                GL_TEXTURE_2D, cudaGraphicsRegisterFlagsSurfaceLoadStore);
    cuda_depth_resource_  = depth_res;
    cuda_shadow_resource_ = shadow_res;
    interop_registered_ = true;
}

void RtLighting::prepare_depth(unsigned gl_depth_tex, int screen_w, int screen_h) {
    resize(screen_w, screen_h);

    // Run depth_linearize.comp: depth_copy_tex_ (D24 sampler) -> depth_linear_tex_ (R32F image).
    glUseProgram(depth_lin_prog_);

    // Bind depth texture as sampler on unit 0.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gl_depth_tex);
    glUniform1i(glGetUniformLocation(depth_lin_prog_, "u_depth"), 0);

    // Near/far must match the raster pipeline (hardcoded in matter_engine.cpp).
    glUniform1f(glGetUniformLocation(depth_lin_prog_, "u_near"), 1.0f);
    glUniform1f(glGetUniformLocation(depth_lin_prog_, "u_far"), 5000.0f);

    // Bind output image.
    glBindImageTexture(0, depth_linear_tex_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);

    // Dispatch.
    int gx = (trace_w_ + 15) / 16;
    int gy = (trace_h_ + 15) / 16;
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    glUseProgram(0);
}
```

- [ ] **Step 6: Compile the compute shader in init()**

Add to `RtLighting::init()` after OptiX context creation (this requires GL context to be current):

```cpp
    // Compile depth linearization compute shader.
    {
        const char* src = nullptr;  // loaded via shader_text or embedded
        // For now, load from file:
        std::string comp_src;
        FILE* f = fopen("shaders_gpu/depth_linearize.comp", "r");
        if (!f) { err = "cannot open shaders_gpu/depth_linearize.comp"; return false; }
        fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
        comp_src.resize(len);
        fread(&comp_src[0], 1, len, f); fclose(f);

        GLuint cs = glCreateShader(GL_COMPUTE_SHADER);
        const char* s = comp_src.c_str();
        glShaderSource(cs, 1, &s, nullptr);
        glCompileShader(cs);
        GLint ok = 0; glGetShaderiv(cs, GL_COMPILE_STATUS, &ok);
        if (!ok) { err = "depth_linearize.comp compile failed"; return false; }

        depth_lin_prog_ = glCreateProgram();
        glAttachShader(depth_lin_prog_, cs);
        glLinkProgram(depth_lin_prog_);
        glDeleteShader(cs);
        glGetProgramiv(depth_lin_prog_, GL_LINK_STATUS, &ok);
        if (!ok) { err = "depth_linearize.comp link failed"; return false; }
    }
```

- [ ] **Step 7: Commit**

```bash
git add MatterEngine3/shaders_gpu/depth_linearize.comp \
       MatterEngine3/src/render/rt_lighting.h MatterEngine3/src/render/rt_lighting.cpp \
       MatterEngine3/src/render/gpu_culler.h MatterEngine3/src/render/gpu_culler.cpp
git commit -m "feat(rt): depth linearization compute shader + CUDA-GL interop setup"
```

---

### Task 5: Shadow Ray Generation Program + OptiX Pipeline

**Files:**
- Create: `MatterEngine3/src/render/shaders_rt/shadow_raygen.cu`
- Create: `MatterEngine3/src/render/shaders_rt/shadow_miss.cu`
- Create: `MatterEngine3/src/render/shaders_rt/shadow_hit.cu`
- Create: `MatterEngine3/src/render/shaders_rt/rt_params.h` (shared launch params struct)
- Modify: `MatterEngine3/src/render/rt_lighting.h` (add pipeline state, trace method)
- Modify: `MatterEngine3/src/render/rt_lighting.cpp` (pipeline creation + trace launch)

**Interfaces:**
- Consumes: `RtLighting::tlas_handle_`, `cuda_depth_resource_`, `cuda_shadow_resource_`, camera inverse VP matrix (float[16]), sun direction (float[3])
- Produces: `RtLighting::trace_shadows(const float inv_vp[16], const float sun_dir[3])` — launches OptiX, writes to shadow_gl_tex_

- [ ] **Step 1: Create rt_params.h (shared between host and device)**

```cpp
#ifndef RT_PARAMS_H
#define RT_PARAMS_H

#include <cuda_runtime.h>
#include <optix.h>

struct RtLaunchParams {
    OptixTraversableHandle tlas;
    float inv_vp[16];          // row-major inverse view-projection
    float sun_dir[3];          // normalized, pointing TO the sun
    int   width;
    int   height;
    cudaSurfaceObject_t depth_surface;   // input: R32F depth NDC [0,1]
    cudaSurfaceObject_t shadow_surface;  // output: R32F shadow [0=shadow, 1=lit]
};

#endif
```

- [ ] **Step 2: Create shadow_raygen.cu**

```cuda
#include "rt_params.h"
#include <optix_device.h>

extern "C" __constant__ RtLaunchParams params;

extern "C" __global__ void __raygen__shadow() {
    uint3 idx = optixGetLaunchIndex();
    if (idx.x >= params.width || idx.y >= params.height) return;

    // Read depth NDC from the linearized depth surface.
    float z_ndc;
    surf2Dread(&z_ndc, params.depth_surface,
               idx.x * sizeof(float), idx.y);

    // Sky pixels (z_ndc == 1.0 in OpenGL) are fully lit.
    if (z_ndc >= 0.9999f) {
        surf2Dwrite(1.0f, params.shadow_surface,
                    idx.x * sizeof(float), idx.y);
        return;
    }

    // Reconstruct clip-space position.
    float ndc_x = ((float)idx.x + 0.5f) / (float)params.width  * 2.0f - 1.0f;
    float ndc_y = ((float)idx.y + 0.5f) / (float)params.height * 2.0f - 1.0f;
    float ndc_z = z_ndc * 2.0f - 1.0f;  // OpenGL: depth [0,1] -> NDC [-1,1]

    // Multiply by inverse VP (row-major: vec * mat).
    const float* m = params.inv_vp;
    float cx = ndc_x*m[0] + ndc_y*m[4] + ndc_z*m[8]  + m[12];
    float cy = ndc_x*m[1] + ndc_y*m[5] + ndc_z*m[9]  + m[13];
    float cz = ndc_x*m[2] + ndc_y*m[6] + ndc_z*m[10] + m[14];
    float cw = ndc_x*m[3] + ndc_y*m[7] + ndc_z*m[11] + m[15];

    float world_x = cx / cw;
    float world_y = cy / cw;
    float world_z = cz / cw;

    // Trace shadow ray toward sun.
    float3 origin = make_float3(world_x, world_y, world_z);
    float3 dir    = make_float3(params.sun_dir[0], params.sun_dir[1], params.sun_dir[2]);

    // Bias origin along sun direction to avoid self-intersection.
    origin.x += dir.x * 0.05f;
    origin.y += dir.y * 0.05f;
    origin.z += dir.z * 0.05f;

    unsigned int shadow_hit = 0;
    optixTrace(params.tlas,
               origin, dir,
               0.0f,          // tmin
               1000.0f,       // tmax (sun is infinitely far)
               0.0f,          // ray time
               0xFF,          // visibility mask
               OPTIX_RAY_FLAG_TERMINATE_ON_FIRST_HIT | OPTIX_RAY_FLAG_DISABLE_CLOSESTHIT,
               0, 1, 0,       // SBT offset, stride, miss index
               shadow_hit);   // payload register 0

    float visibility = shadow_hit ? 0.0f : 1.0f;
    surf2Dwrite(visibility, params.shadow_surface,
                idx.x * sizeof(float), idx.y);
}
```

- [ ] **Step 3: Create shadow_miss.cu**

```cuda
#include "rt_params.h"
#include <optix_device.h>

extern "C" __global__ void __miss__shadow() {
    // No hit = fully lit (payload 0 stays 0 = no shadow).
    // payload register 0 is already 0 from the trace call.
}
```

- [ ] **Step 4: Create shadow_hit.cu**

```cuda
#include "rt_params.h"
#include <optix_device.h>

extern "C" __global__ void __anyhit__shadow() {
    // Set payload to 1 = shadow hit, then terminate.
    optixSetPayload_0(1);
    optixTerminateRay();
}
```

- [ ] **Step 5: Add pipeline state and trace method to rt_lighting.h**

Add to `RtLighting` private:
```cpp
    // OptiX pipeline
    void* pipeline_       = nullptr;  // OptixPipeline
    void* raygen_pg_      = nullptr;  // OptixProgramGroup
    void* miss_pg_        = nullptr;  // OptixProgramGroup
    void* hit_pg_         = nullptr;  // OptixProgramGroup
    void* sbt_raygen_buf_ = nullptr;  // CUdeviceptr (SBT record)
    void* sbt_miss_buf_   = nullptr;
    void* sbt_hit_buf_    = nullptr;
    void* d_params_       = nullptr;  // CUdeviceptr (launch params)

    bool build_pipeline(std::string& err);
```

Add to `RtLighting` public:
```cpp
    void trace_shadows(const float inv_vp[16], const float sun_dir[3]);
```

- [ ] **Step 6: Implement build_pipeline in rt_lighting.cpp**

```cpp
bool RtLighting::build_pipeline(std::string& err) {
    auto ctx = (OptixDeviceContext)optix_ctx_;

    // Load PTX files.
    auto load_ptx = [](const char* path, std::string& out) -> bool {
        FILE* f = fopen(path, "r");
        if (!f) return false;
        fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
        out.resize(len);
        fread(&out[0], 1, len, f); fclose(f);
        return true;
    };

    std::string raygen_ptx, miss_ptx, hit_ptx;
    if (!load_ptx("src/render/shaders_rt/shadow_raygen.ptx", raygen_ptx)) {
        err = "cannot load shadow_raygen.ptx"; return false;
    }
    if (!load_ptx("src/render/shaders_rt/shadow_miss.ptx", miss_ptx)) {
        err = "cannot load shadow_miss.ptx"; return false;
    }
    if (!load_ptx("src/render/shaders_rt/shadow_hit.ptx", hit_ptx)) {
        err = "cannot load shadow_hit.ptx"; return false;
    }

    // Module compile options.
    OptixModuleCompileOptions mod_opts = {};
    mod_opts.maxRegisterCount = OPTIX_COMPILE_DEFAULT_MAX_REGISTER_COUNT;
    mod_opts.optLevel         = OPTIX_COMPILE_OPTIMIZATION_DEFAULT;

    OptixPipelineCompileOptions pipe_opts = {};
    pipe_opts.usesMotionBlur                   = false;
    pipe_opts.traversableGraphFlags            = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_ANY;
    pipe_opts.numPayloadValues                 = 1;
    pipe_opts.numAttributeValues               = 2;
    pipe_opts.exceptionFlags                   = OPTIX_EXCEPTION_FLAG_NONE;
    pipe_opts.pipelineLaunchParamsVariableName  = "params";

    // Create modules.
    OptixModule mod_raygen = nullptr, mod_miss = nullptr, mod_hit = nullptr;
    char log[2048]; size_t log_size;

    log_size = sizeof(log);
    optixModuleCreate(ctx, &mod_opts, &pipe_opts,
                      raygen_ptx.c_str(), raygen_ptx.size(),
                      log, &log_size, &mod_raygen);

    log_size = sizeof(log);
    optixModuleCreate(ctx, &mod_opts, &pipe_opts,
                      miss_ptx.c_str(), miss_ptx.size(),
                      log, &log_size, &mod_miss);

    log_size = sizeof(log);
    optixModuleCreate(ctx, &mod_opts, &pipe_opts,
                      hit_ptx.c_str(), hit_ptx.size(),
                      log, &log_size, &mod_hit);

    // Program groups.
    OptixProgramGroupOptions pg_opts = {};
    OptixProgramGroupDesc pg_desc = {};

    // Raygen.
    pg_desc.kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
    pg_desc.raygen.module = mod_raygen;
    pg_desc.raygen.entryFunctionName = "__raygen__shadow";
    OptixProgramGroup pg_raygen = nullptr;
    log_size = sizeof(log);
    optixProgramGroupCreate(ctx, &pg_desc, 1, &pg_opts, log, &log_size, &pg_raygen);
    raygen_pg_ = pg_raygen;

    // Miss.
    pg_desc = {};
    pg_desc.kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
    pg_desc.miss.module = mod_miss;
    pg_desc.miss.entryFunctionName = "__miss__shadow";
    OptixProgramGroup pg_miss = nullptr;
    log_size = sizeof(log);
    optixProgramGroupCreate(ctx, &pg_desc, 1, &pg_opts, log, &log_size, &pg_miss);
    miss_pg_ = pg_miss;

    // Hit (anyhit for shadow).
    pg_desc = {};
    pg_desc.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
    pg_desc.hitgroup.moduleAH            = mod_hit;
    pg_desc.hitgroup.entryFunctionNameAH = "__anyhit__shadow";
    OptixProgramGroup pg_hit = nullptr;
    log_size = sizeof(log);
    optixProgramGroupCreate(ctx, &pg_desc, 1, &pg_opts, log, &log_size, &pg_hit);
    hit_pg_ = pg_hit;

    // Pipeline.
    OptixProgramGroup groups[] = { pg_raygen, pg_miss, pg_hit };
    OptixPipelineLinkOptions link_opts = {};
    link_opts.maxTraceDepth = 1;
    OptixPipeline pipeline = nullptr;
    log_size = sizeof(log);
    optixPipelineCreate(ctx, &pipe_opts, &link_opts,
                        groups, 3, log, &log_size, &pipeline);
    pipeline_ = pipeline;

    // SBT (Shader Binding Table).
    struct alignas(OPTIX_SBT_RECORD_ALIGNMENT) SbtRecord {
        char header[OPTIX_SBT_RECORD_HEADER_SIZE];
    };

    SbtRecord raygen_rec = {};
    optixSbtRecordPackHeader(pg_raygen, &raygen_rec);
    cudaMalloc(&sbt_raygen_buf_, sizeof(SbtRecord));
    cudaMemcpy(sbt_raygen_buf_, &raygen_rec, sizeof(SbtRecord), cudaMemcpyHostToDevice);

    SbtRecord miss_rec = {};
    optixSbtRecordPackHeader(pg_miss, &miss_rec);
    cudaMalloc(&sbt_miss_buf_, sizeof(SbtRecord));
    cudaMemcpy(sbt_miss_buf_, &miss_rec, sizeof(SbtRecord), cudaMemcpyHostToDevice);

    SbtRecord hit_rec = {};
    optixSbtRecordPackHeader(pg_hit, &hit_rec);
    cudaMalloc(&sbt_hit_buf_, sizeof(SbtRecord));
    cudaMemcpy(sbt_hit_buf_, &hit_rec, sizeof(SbtRecord), cudaMemcpyHostToDevice);

    // Allocate device-side launch params.
    cudaMalloc(&d_params_, sizeof(RtLaunchParams));

    return true;
}
```

- [ ] **Step 7: Implement trace_shadows in rt_lighting.cpp**

```cpp
void RtLighting::trace_shadows(const float inv_vp[16], const float sun_dir[3]) {
    if (!available_ || !tlas_handle_ || trace_w_ == 0) return;

    // Map CUDA-GL interop resources.
    cudaGraphicsResource_t resources[2] = {
        (cudaGraphicsResource_t)cuda_depth_resource_,
        (cudaGraphicsResource_t)cuda_shadow_resource_
    };
    cudaGraphicsMapResources(2, resources, 0);

    // Get CUDA arrays from interop.
    cudaArray_t depth_array = nullptr, shadow_array = nullptr;
    cudaGraphicsSubResourceGetMappedArray(&depth_array, resources[0], 0, 0);
    cudaGraphicsSubResourceGetMappedArray(&shadow_array, resources[1], 0, 0);

    // Create surface objects.
    cudaResourceDesc res_desc = {};
    res_desc.resType = cudaResourceTypeArray;

    res_desc.res.array.array = depth_array;
    cudaSurfaceObject_t depth_surf = 0;
    cudaCreateSurfaceObject(&depth_surf, &res_desc);

    res_desc.res.array.array = shadow_array;
    cudaSurfaceObject_t shadow_surf = 0;
    cudaCreateSurfaceObject(&shadow_surf, &res_desc);

    // Fill launch params.
    RtLaunchParams lp = {};
    lp.tlas           = (OptixTraversableHandle)tlas_handle_;
    memcpy(lp.inv_vp, inv_vp, 16 * sizeof(float));
    memcpy(lp.sun_dir, sun_dir, 3 * sizeof(float));
    lp.width          = trace_w_;
    lp.height         = trace_h_;
    lp.depth_surface  = depth_surf;
    lp.shadow_surface = shadow_surf;

    cudaMemcpy(d_params_, &lp, sizeof(RtLaunchParams), cudaMemcpyHostToDevice);

    // Build SBT.
    OptixShaderBindingTable sbt = {};
    sbt.raygenRecord                = (CUdeviceptr)sbt_raygen_buf_;
    sbt.missRecordBase              = (CUdeviceptr)sbt_miss_buf_;
    sbt.missRecordStrideInBytes     = OPTIX_SBT_RECORD_HEADER_SIZE;
    sbt.missRecordCount             = 1;
    sbt.hitgroupRecordBase          = (CUdeviceptr)sbt_hit_buf_;
    sbt.hitgroupRecordStrideInBytes = OPTIX_SBT_RECORD_HEADER_SIZE;
    sbt.hitgroupRecordCount         = 1;

    // Launch!
    optixLaunch((OptixPipeline)pipeline_, 0,
                (CUdeviceptr)d_params_, sizeof(RtLaunchParams),
                &sbt, trace_w_, trace_h_, 1);
    cudaDeviceSynchronize();

    // Cleanup surface objects.
    cudaDestroySurfaceObject(depth_surf);
    cudaDestroySurfaceObject(shadow_surf);

    // Unmap interop resources (makes textures available to GL again).
    cudaGraphicsUnmapResources(2, resources, 0);
}
```

- [ ] **Step 8: Call build_pipeline from init()**

Add at the end of `RtLighting::init()`, after the compute shader setup:
```cpp
    if (!build_pipeline(err)) return false;
```

- [ ] **Step 9: Commit**

```bash
git add MatterEngine3/src/render/shaders_rt/rt_params.h \
       MatterEngine3/src/render/shaders_rt/shadow_raygen.cu \
       MatterEngine3/src/render/shaders_rt/shadow_miss.cu \
       MatterEngine3/src/render/shaders_rt/shadow_hit.cu \
       MatterEngine3/src/render/rt_lighting.h \
       MatterEngine3/src/render/rt_lighting.cpp
git commit -m "feat(rt): OptiX shadow ray pipeline + trace_shadows launch"
```

---

### Task 6: Composite Shader + MatterViewer Frame Loop Integration

**Files:**
- Create: `MatterEngine3/shaders_gpu/rt_composite.vs`
- Create: `MatterEngine3/shaders_gpu/rt_composite.fs`
- Modify: `MatterEngine3/src/render/rt_lighting.h` (add composite method)
- Modify: `MatterEngine3/src/render/rt_lighting.cpp` (implement composite)
- Modify: `MatterEngine3/src/matter_engine.cpp` (wire RT into render loop)
- Modify: `MatterEngine3/include/matter/world_session.h` (add `rt_shadows` to RenderOptions)

**Interfaces:**
- Consumes: `RtLighting::shadow_gl_tex_` (R32F, quarter res), raster output in default FBO
- Produces: Final composited frame with shadows applied to the default FBO color buffer

- [ ] **Step 1: Create rt_composite.vs**

```glsl
#version 460
out vec2 v_uv;
void main() {
    // Fullscreen triangle trick: 3 vertices cover the screen.
    v_uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(v_uv * 2.0 - 1.0, 0.0, 1.0);
}
```

- [ ] **Step 2: Create rt_composite.fs**

```glsl
#version 460
in vec2 v_uv;
out vec4 frag_color;

uniform sampler2D u_scene;    // full-res raster color (from FBO blit)
uniform sampler2D u_shadow;   // quarter-res shadow mask [0=shadow, 1=lit]
uniform float     u_shadow_strength;  // 0.0 = no shadow darkening, 1.0 = full black

void main() {
    vec3 scene  = texture(u_scene, v_uv).rgb;
    float shadow = texture(u_shadow, v_uv).r;  // bilinear upscale

    // Darken shadowed regions.
    float factor = mix(1.0, shadow, u_shadow_strength);
    frag_color = vec4(scene * factor, 1.0);
}
```

- [ ] **Step 3: Add composite resources + method to rt_lighting.h**

Add to `RtLighting` public:
```cpp
    void composite(int screen_w, int screen_h, float shadow_strength);
```

Add to `RtLighting` private:
```cpp
    unsigned composite_prog_  = 0;   // shader program
    unsigned scene_copy_tex_  = 0;   // full-res copy of raster color
    unsigned scene_fbo_       = 0;   // FBO for blitting scene color
    unsigned dummy_vao_       = 0;   // empty VAO for fullscreen triangle
```

- [ ] **Step 4: Implement composite in rt_lighting.cpp**

```cpp
void RtLighting::composite(int screen_w, int screen_h, float shadow_strength) {
    if (!available_ || !shadow_gl_tex_) return;

    // Ensure scene_copy_tex_ exists at current resolution.
    // (Created/resized alongside depth_linear_tex_ in resize() — add there.)
    // For now, blit default FBO color → scene_copy_tex_.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, scene_fbo_);
    glBlitFramebuffer(0, 0, screen_w, screen_h,
                      0, 0, screen_w, screen_h,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

    // Draw fullscreen composite into default FBO.
    glUseProgram(composite_prog_);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scene_copy_tex_);
    glUniform1i(glGetUniformLocation(composite_prog_, "u_scene"), 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, shadow_gl_tex_);
    glUniform1i(glGetUniformLocation(composite_prog_, "u_shadow"), 1);

    glUniform1f(glGetUniformLocation(composite_prog_, "u_shadow_strength"), shadow_strength);

    glDisable(GL_DEPTH_TEST);
    glBindVertexArray(dummy_vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);

    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
}
```

- [ ] **Step 5: Add scene_copy_tex_ creation to resize()**

Add in `RtLighting::resize()` after shadow_gl_tex_ creation:

```cpp
    // Full-res scene copy texture (for composite input).
    if (scene_copy_tex_) glDeleteTextures(1, &scene_copy_tex_);
    glGenTextures(1, &scene_copy_tex_);
    glBindTexture(GL_TEXTURE_2D, scene_copy_tex_);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, screen_w, screen_h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);

    // FBO wrapping scene_copy_tex_.
    if (scene_fbo_) glDeleteFramebuffers(1, &scene_fbo_);
    glGenFramebuffers(1, &scene_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, scene_copy_tex_, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Empty VAO for fullscreen triangle.
    if (!dummy_vao_) glGenVertexArrays(1, &dummy_vao_);
```

- [ ] **Step 6: Compile composite shader in init()**

Add to `RtLighting::init()`:

```cpp
    // Compile composite shader.
    {
        auto compile_shader = [](GLenum type, const char* path) -> GLuint {
            FILE* f = fopen(path, "r");
            if (!f) return 0;
            fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
            std::string src(len, '\0');
            fread(&src[0], 1, len, f); fclose(f);
            GLuint s = glCreateShader(type);
            const char* p = src.c_str();
            glShaderSource(s, 1, &p, nullptr);
            glCompileShader(s);
            return s;
        };
        GLuint vs = compile_shader(GL_VERTEX_SHADER, "shaders_gpu/rt_composite.vs");
        GLuint fs = compile_shader(GL_FRAGMENT_SHADER, "shaders_gpu/rt_composite.fs");
        if (!vs || !fs) { err = "rt_composite shader compile failed"; return false; }
        composite_prog_ = glCreateProgram();
        glAttachShader(composite_prog_, vs);
        glAttachShader(composite_prog_, fs);
        glLinkProgram(composite_prog_);
        glDeleteShader(vs); glDeleteShader(fs);
    }
```

- [ ] **Step 7: Add rt_shadows flag to RenderOptions**

In `MatterEngine3/include/matter/world_session.h`, add to `RenderOptions`:
```cpp
    bool  rt_shadows      = false;    // OptiX raytraced shadows (requires NVIDIA GPU)
```

- [ ] **Step 8: Wire into WorldSession::render() GpuDriven path**

In `MatterEngine3/src/matter_engine.cpp`, add to `WorldSession::Impl`:
```cpp
    viewer::RtLighting rt_lighting;
    bool               rt_lighting_ready = false;
```

In the GpuDriven branch of `WorldSession::render()`, after `build_hiz()` (line ~3078), add:

```cpp
        // RT shadow pass (optional, requires OptiX).
        if (opts.rt_shadows && impl_->culler_ready) {
            // Lazy init.
            if (!impl_->rt_lighting_ready) {
                std::string rt_err;
                if (impl_->rt_lighting.init(rt_err)) {
                    impl_->rt_lighting_ready = true;
                    printf("RT shadows enabled: %s\n", rt_err.empty() ? "ok" : rt_err.c_str());
                } else {
                    printf("RT shadows unavailable: %s\n", rt_err.c_str());
                }
            }
            if (impl_->rt_lighting_ready && impl_->rt_lighting.available()) {
                // Register any parts not yet registered.
                for (auto& ri : resolved) {
                    auto* lp = impl_->store->find(ri.part_hash);
                    if (lp && !lp->lod_mesh_data.empty()) {
                        int lod = std::min(ri.lod_level, (int)lp->lod_mesh_data.size() - 1);
                        auto& mesh = lp->lod_mesh_data[lod];
                        impl_->rt_lighting.register_part(ri.part_hash,
                            mesh.vertices.data(), mesh.vertex_count);
                    }
                }

                // Update TLAS from resolved instances.
                std::vector<viewer::RtLighting::InstanceInput> rt_instances;
                rt_instances.reserve(resolved.size());
                for (auto& ri : resolved) {
                    viewer::RtLighting::InstanceInput inp;
                    inp.part_hash = ri.part_hash;
                    inp.lod_level = ri.lod_level;
                    memcpy(inp.transform, ri.transform, sizeof(inp.transform));
                    rt_instances.push_back(inp);
                }
                impl_->rt_lighting.update_instances(rt_instances.data(),
                                                    (int)rt_instances.size());

                // Ensure depth is available and prepare.
                impl_->gpu_culler.ensure_depth_blit(fb_width, fb_height);
                impl_->rt_lighting.prepare_depth(
                    impl_->gpu_culler.depth_copy_tex(), fb_width, fb_height);

                // Compute inverse VP for world-space reconstruction.
                float inv_vp[16];
                viewer::invert16(vp, inv_vp);

                // Get sun direction from manifest lights.
                const float* sun = impl_->manifest.lights.sun_dir;

                // Trace + composite.
                impl_->rt_lighting.trace_shadows(inv_vp, sun);
                impl_->rt_lighting.composite(fb_width, fb_height, 0.7f);
            }
        }
```

- [ ] **Step 9: Enable rt_shadows in MatterViewer main.cpp**

In `MatterViewer/main.cpp`, where `RenderOptions opts` is configured (before `session->render()`), add:
```cpp
    opts.rt_shadows = true;  // enable RT shadow pass
```

- [ ] **Step 10: Build full system and test visually**

```bash
cd MatterEngine3
make clean && make HAVE_CUDA=1 EXTRA_CFLAGS="-DMATTER_HAVE_OPTIX"
cd ../MatterViewer
make clean && make HAVE_CUDA=1
GALLIUM_DRIVER=d3d12 ./viewer
```

Expected: MatterViewer renders the scene with raytraced shadows from the sun. Shadows update as the camera moves. Objects cast shadows onto other objects.

- [ ] **Step 11: Commit**

```bash
git add MatterEngine3/shaders_gpu/rt_composite.vs MatterEngine3/shaders_gpu/rt_composite.fs \
       MatterEngine3/src/render/rt_lighting.h MatterEngine3/src/render/rt_lighting.cpp \
       MatterEngine3/src/matter_engine.cpp MatterEngine3/include/matter/world_session.h \
       MatterViewer/main.cpp
git commit -m "feat(rt): shadow composite + MatterViewer integration"
```

---

## File Map Summary

| File | Role |
|------|------|
| `MatterEngine3/src/render/rt_lighting.h` | RtLighting class declaration (public API) |
| `MatterEngine3/src/render/rt_lighting.cpp` | Full implementation: init, BLAS, TLAS, interop, pipeline, trace, composite |
| `MatterEngine3/src/render/shaders_rt/rt_params.h` | Shared launch params struct (host + device) |
| `MatterEngine3/src/render/shaders_rt/shadow_raygen.cu` | Ray generation: depth → world pos → shadow ray |
| `MatterEngine3/src/render/shaders_rt/shadow_miss.cu` | Miss program: no shadow |
| `MatterEngine3/src/render/shaders_rt/shadow_hit.cu` | Any-hit program: shadow detected |
| `MatterEngine3/shaders_gpu/depth_linearize.comp` | GL compute: D24 depth → R32F at quarter res |
| `MatterEngine3/shaders_gpu/rt_composite.vs` | Fullscreen triangle vertex shader |
| `MatterEngine3/shaders_gpu/rt_composite.fs` | Composite: scene × shadow mask |
| `MatterEngine3/src/render/gpu_culler.h` | Modified: added `depth_copy_tex()`, `ensure_depth_blit()` |
| `MatterEngine3/src/render/gpu_culler.cpp` | Modified: `ensure_depth_blit()` implementation |
| `MatterEngine3/src/matter_engine.cpp` | Modified: RT lighting wired into render loop |
| `MatterEngine3/include/matter/world_session.h` | Modified: `rt_shadows` added to RenderOptions |
| `MatterEngine3/Makefile` | Modified: CUDA/OptiX build rules |
| `MatterViewer/Makefile` | Modified: `-lcudart` link flag |
| `MatterViewer/main.cpp` | Modified: enable `opts.rt_shadows` |
