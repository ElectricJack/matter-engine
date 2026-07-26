// CPU-only compatibility surface for bake code that still uses raylib's
// allocation API and Mesh type. This translation unit deliberately provides
// no window, OpenGL, pixel-format, or swap implementation.
//
// Phase 4 (Step 5) of docs/superpowers/plans/2026-07-25-mathlib-and-raylib-removal.md
// shrank this file: MatterSurfaceLib's cell.cpp/cluster.cpp (the only callers
// of Vector3Add/Subtract/DotProduct/Length, QuaternionIdentity/Invert, and
// Vector3RotateByQuaternion) moved onto MathLib's mm:: equivalents in Phase 4
// Steps 3-4, and MatrixTranslate's one caller (cluster.cpp's visit_cells) went
// with it -- all eight are gone. The 17 GPU-call abort() stubs this file used
// to carry (UploadMesh, GenMeshSphere, LoadModelFromMesh, UnloadModel,
// DrawMesh/DrawMeshInstanced, DrawCubeWires, DrawSphere, rlEnableWireMode/
// rlDisableWireMode, UnloadTexture, LoadTextureFromImage, UpdateTexture,
// SetTextureFilter, GetShaderLocation, SetShaderValue, SetShaderValueTexture)
// were already dead by the time Phase 5a deleted their only call sites
// (renderer.cpp, raster_composer.cpp, gpu_culler.cpp, bvh_visualizer.*); a
// repo-wide grep across MatterEngine3/src, libs/MatterSurfaceLib/src+include
// and MatterEditor/src confirmed zero remaining callers of any of them, so
// they are deleted here rather than carried forward as decoration.
//
// What is left is genuinely load-bearing: MemAlloc/MemRealloc/MemFree back
// mesh_simplifier.cpp's and mesh_build_utils.cpp's heap allocations, and
// UnloadMesh is cell.cpp's UNCONDITIONAL (not #ifndef MATTER_VULKAN_ONLY
// gated) CPU-side mesh free -- Cell::clear_meshes calls it on every mesh in
// material_meshes regardless of platform, so removing it would leak.
#include "raylib.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace {
#ifdef MATTER_VULKAN_COMPAT_TESTING
std::atomic<size_t> g_outstanding_allocations{0};
#endif
}

extern "C" {

void* MemAlloc(unsigned int size) {
    void* ptr = std::malloc(size);
#ifdef MATTER_VULKAN_COMPAT_TESTING
    if (ptr) ++g_outstanding_allocations;
#endif
    return ptr;
}
void* MemRealloc(void* ptr, unsigned int size) {
    if (!ptr) return MemAlloc(size);
    if (size == 0) {
        MemFree(ptr);
        return nullptr;
    }
    return std::realloc(ptr, size);
}
void MemFree(void* ptr) {
    if (!ptr) return;
    std::free(ptr);
#ifdef MATTER_VULKAN_COMPAT_TESTING
    --g_outstanding_allocations;
#endif
}
#ifdef MATTER_VULKAN_COMPAT_TESTING
size_t MatterVulkanCompatOutstandingAllocations(void) {
    return g_outstanding_allocations.load();
}
#endif

void UnloadMesh(Mesh mesh) {
    MemFree(mesh.vertices);
    MemFree(mesh.texcoords);
    MemFree(mesh.texcoords2);
    MemFree(mesh.normals);
    MemFree(mesh.tangents);
    MemFree(mesh.colors);
    MemFree(mesh.indices);
    MemFree(mesh.animVertices);
    MemFree(mesh.animNormals);
    MemFree(mesh.boneIds);
    MemFree(mesh.boneWeights);
    MemFree(mesh.boneMatrices);
    MemFree(mesh.vboId);
}

}
