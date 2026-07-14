// CPU-only compatibility surface for bake code that still uses raylib's POD
// math and allocation API.  This translation unit deliberately provides no
// window, OpenGL, pixel-format, or swap implementation.
#include "raylib.h"
#include "rlgl.h"

#include <cmath>
#include <cstdlib>

extern "C" {

void* MemAlloc(unsigned int size) { return std::malloc(size); }
void* MemRealloc(void* ptr, unsigned int size) { return std::realloc(ptr, size); }
void MemFree(void* ptr) { std::free(ptr); }

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

void UploadMesh(Mesh*, bool) {}
void UnloadMesh(Mesh) {}
Mesh GenMeshSphere(float, int, int) { return Mesh{}; }
Model LoadModelFromMesh(Mesh mesh) { Model model{}; model.meshes = nullptr; model.meshCount = 0; (void)mesh; return model; }
void UnloadModel(Model) {}
void DrawMesh(Mesh, Material, Matrix) {}
void DrawMeshInstanced(Mesh, Material, const Matrix*, int) {}
void DrawCubeWires(Vector3, float, float, float, Color) {}
void DrawSphere(Vector3, float, Color) {}
void rlEnableWireMode(void) {}
void rlDisableWireMode(void) {}

void UnloadTexture(Texture2D) {}
Texture2D LoadTextureFromImage(Image) { return Texture2D{}; }
void UpdateTexture(Texture2D, const void*) {}
void SetTextureFilter(Texture2D, int) {}
int GetShaderLocation(Shader, const char*) { return -1; }
void SetShaderValue(Shader, int, const void*, int) {}
void SetShaderValueTexture(Shader, int, Texture2D) {}

}
