#pragma once

// MatterEngine3/include/matter/math_types.h
//
// The engine's public POD math types. Deliberately minimal: plain structs
// with default member initializers, no operators, no methods, no includes at
// all. They exist so public headers (matter/ecs.h,
// matter/cloud_shadow_settings.h, matter/streaming.h, ...) can pass vectors
// and matrices across the API without dragging in a math library or raylib.
//
// Do not confuse these with the repo's two other vector families:
//   * libs/MathLib (`mm::Vec3`, `mm::Mat4`, ...) is the canonical math
//     library, with real operators — use it to COMPUTE;
//   * libs/SpatialQueryLib's precomp.h `float3`/`float4` are the aligned SIMD
//     types the BVH/Tri code interchanges.
// The types here are the plain interchange layer between them and callers.
//
// Conventions:
//   * These carry no units of their own; the field that holds one documents
//     what it means (the engine's world units are metres).
//   * `Float2/3/4` default to all-zero and `Quaternion` to identity, so a
//     default-constructed value is always a usable value.
//   * ecs::CoreModule (MatterEngine3/src/ecs/ecs_runtime.cpp) registers these
//     for Flecs reflection member by member, so their layout is part of the
//     component ABI — adding or reordering a member means updating that
//     registration too.

namespace matter {

struct Float2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Float3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Float4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
};

// Component order is (x, y, z, w) with the scalar LAST, and the default is
// the identity rotation. Expected to be unit length; nothing here normalizes.
struct Quaternion {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

// Row-major storage with column-vector algebra.
// Concretely: m[0..3] is the first ROW, and the translation lands in m[3],
// m[7] and m[11] — see the world_to_uvw construction in
// matter/cloud_shadow_settings.h for a worked example. Anything that expects
// column-major storage (GLSL's default mat4 layout, and libraries following
// it) needs a transpose on the way in or out.
struct Mat4f {
    float m[16] = {};
};

} // namespace matter
