// CPU-only compatibility surface for bake code that still uses raylib's POD
// math and allocation API.  This translation unit deliberately provides no
// window, OpenGL, pixel-format, or swap implementation.
#include "raylib.h"
#include "rlgl.h"

#include <cmath>
#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace {
#ifdef MATTER_VULKAN_COMPAT_TESTING
std::atomic<size_t> g_outstanding_allocations{0};
#endif

[[noreturn]] void unsupported_gpu_call(const char* name) {
    std::fprintf(stderr,
                 "FATAL: %s is unavailable in MATTER_VULKAN_ONLY\n", name);
    std::abort();
}

void unsupported_gpu_value(const char* name) {
    std::fprintf(stderr,
                 "ERROR: %s is unavailable in MATTER_VULKAN_ONLY\n", name);
}
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

Vector3 Vector3Add(Vector3 a, Vector3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Vector3 Vector3Subtract(Vector3 a, Vector3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
float Vector3DotProduct(Vector3 a, Vector3 b) {
    return a.x*b.x + a.y*b.y + a.z*b.z;
}
float Vector3Length(Vector3 v) { return std::sqrt(Vector3DotProduct(v, v)); }
Quaternion QuaternionIdentity(void) { return {0.0f, 0.0f, 0.0f, 1.0f}; }
Quaternion QuaternionInvert(Quaternion q) {
    const float n = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
    return n > 0.0f ? Quaternion{-q.x/n, -q.y/n, -q.z/n, q.w/n}
                    : QuaternionIdentity();
}
Vector3 Vector3RotateByQuaternion(Vector3 v, Quaternion q) {
    const Vector3 u{q.x, q.y, q.z};
    const float uv = Vector3DotProduct(u, v);
    const float uu = Vector3DotProduct(u, u);
    const Vector3 cross{u.y*v.z-u.z*v.y, u.z*v.x-u.x*v.z,
                        u.x*v.y-u.y*v.x};
    return {2.0f*uv*u.x + (q.w*q.w-uu)*v.x + 2.0f*q.w*cross.x,
            2.0f*uv*u.y + (q.w*q.w-uu)*v.y + 2.0f*q.w*cross.y,
            2.0f*uv*u.z + (q.w*q.w-uu)*v.z + 2.0f*q.w*cross.z};
}
Matrix MatrixTranslate(float x, float y, float z) {
    Matrix m{};
    m.m0 = m.m5 = m.m10 = m.m15 = 1.0f;
    m.m12 = x; m.m13 = y; m.m14 = z;
    return m;
}

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

// These definitions only satisfy legacy objects that remain linked for their
// CPU bake code. An accidental Vulkan-only GPU call is explicit and invalid;
// it must never look like a successful upload, draw, texture, or shader bind.
void UploadMesh(Mesh*, bool) { unsupported_gpu_call("UploadMesh"); }
Mesh GenMeshSphere(float, int, int) {
    unsupported_gpu_call("GenMeshSphere");
}
Model LoadModelFromMesh(Mesh) {
    unsupported_gpu_call("LoadModelFromMesh");
}
void UnloadModel(Model model) {
    if (model.meshCount != 0 || model.materialCount != 0)
        unsupported_gpu_call("UnloadModel");
}
void DrawMesh(Mesh, Material, Matrix) { unsupported_gpu_call("DrawMesh"); }
void DrawMeshInstanced(Mesh, Material, const Matrix*, int) {
    unsupported_gpu_call("DrawMeshInstanced");
}
void DrawCubeWires(Vector3, float, float, float, Color) {
    unsupported_gpu_call("DrawCubeWires");
}
void DrawSphere(Vector3, float, Color) { unsupported_gpu_call("DrawSphere"); }
void rlEnableWireMode(void) { unsupported_gpu_call("rlEnableWireMode"); }
void rlDisableWireMode(void) { unsupported_gpu_call("rlDisableWireMode"); }

void UnloadTexture(Texture2D texture) {
    if (texture.id != 0) unsupported_gpu_call("UnloadTexture");
}
Texture2D LoadTextureFromImage(Image) {
    unsupported_gpu_value("LoadTextureFromImage");
    return Texture2D{};
}
void UpdateTexture(Texture2D, const void*) {
    unsupported_gpu_call("UpdateTexture");
}
void SetTextureFilter(Texture2D, int) {
    unsupported_gpu_call("SetTextureFilter");
}
int GetShaderLocation(Shader, const char*) {
    unsupported_gpu_value("GetShaderLocation");
    return -1;
}
void SetShaderValue(Shader, int, const void*, int) {
    unsupported_gpu_call("SetShaderValue");
}
void SetShaderValueTexture(Shader, int, Texture2D) {
    unsupported_gpu_call("SetShaderValueTexture");
}

}
