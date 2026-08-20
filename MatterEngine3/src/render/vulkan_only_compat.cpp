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
// What is left is genuinely load-bearing: MemAlloc backs
// mesh_simplifier.cpp's output arrays and MemFree backs
// mesh_build_utils.cpp's mesh teardown, and UnloadMesh is cell.cpp's
// UNCONDITIONAL (not #ifndef MATTER_VULKAN_ONLY gated) CPU-side mesh free --
// Cell::clear_meshes calls it on every mesh in material_meshes regardless of
// platform, so removing it would leak.
//
// There is deliberately NO MemRealloc here: raylib declares one, but a
// repo-wide grep finds no caller in MatterEngine3, MatterEditor or
// libs/MatterSurfaceLib, so the link stays clean without it. If a future
// caller appears the link will fail loudly rather than silently mixing
// allocators -- add it here beside MemAlloc/MemFree at that point.
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
void MemFree(void* ptr) {
    if (!ptr) return;
    std::free(ptr);
#ifdef MATTER_VULKAN_COMPAT_TESTING
    --g_outstanding_allocations;
#endif
}
// Test-build-only leak probe: the count of MemAlloc calls not yet matched by a
// MemFree. Compiled in (along with the counter increments above) only under
// MATTER_VULKAN_COMPAT_TESTING, so the shipping build pays nothing.
#ifdef MATTER_VULKAN_COMPAT_TESTING
size_t MatterVulkanCompatOutstandingAllocations(void) {
    return g_outstanding_allocations.load();
}
#endif

// Frees every host array a raylib Mesh owns. This is cell.cpp's unconditional
// CPU-side mesh free (Cell::clear_meshes), so it must stay even in the
// Vulkan-only build. It is a pure host free: upstream raylib would first delete
// the mesh's GL buffer objects, but no GL objects exist here, so `vboId` is
// just another host array to release. Passing a Mesh whose pointers were not
// allocated through MemAlloc is undefined, and double-free is not guarded
// against — raylib's convention is that the caller zeroes or drops the Mesh.
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
