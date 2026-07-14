#include "raylib.h"

#include <cstdio>

extern "C" size_t MatterVulkanCompatOutstandingAllocations(void);

int main() {
    Mesh mesh{};
#define ALLOC_FIELD(field, bytes, type) \
    mesh.field = static_cast<type*>(MemAlloc(bytes))
    ALLOC_FIELD(vertices, 12, float);
    ALLOC_FIELD(texcoords, 8, float);
    ALLOC_FIELD(texcoords2, 8, float);
    ALLOC_FIELD(normals, 12, float);
    ALLOC_FIELD(tangents, 16, float);
    ALLOC_FIELD(colors, 4, unsigned char);
    ALLOC_FIELD(indices, 6, unsigned short);
    ALLOC_FIELD(animVertices, 12, float);
    ALLOC_FIELD(animNormals, 12, float);
    ALLOC_FIELD(boneIds, 4, unsigned char);
    ALLOC_FIELD(boneWeights, 16, float);
    ALLOC_FIELD(boneMatrices, sizeof(Matrix), Matrix);
    ALLOC_FIELD(vboId, 7 * sizeof(unsigned int), unsigned int);
#undef ALLOC_FIELD
    if (MatterVulkanCompatOutstandingAllocations() != 13) return 1;
    UnloadMesh(mesh);
    if (MatterVulkanCompatOutstandingAllocations() != 0) return 2;
    std::puts("vulkan compat CPU cleanup: PASS");
    return 0;
}
