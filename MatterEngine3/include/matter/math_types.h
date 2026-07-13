#pragma once

namespace matter {

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

// Row-major storage with column-vector algebra.
struct Mat4f {
    float m[16] = {};
};

} // namespace matter
