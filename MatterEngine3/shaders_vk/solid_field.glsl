#ifndef SOLID_FIELD_GLSL
#define SOLID_FIELD_GLSL
struct SolidOp {
    vec4 row0;
    vec4 row1;
    vec4 row2;
    vec4 shape;
    uvec4 kind;
    vec4 blend;
};
layout(std430, binding = 0) readonly buffer Params {
    vec4 origin;
    vec4 spacing;
    uvec4 dims;
    uvec4 limits;
};
layout(std430, binding = 1) readonly buffer Ops {
    SolidOp ops[];
};
float solidMin(float a, float b, float k) {
    if (k == 0)
        return min(a, b);
    float h = max(k - abs(a - b), 0) / k;
    return min(a, b) - h * h * k * .25;
}
float solidPrimitive(SolidOp o, vec3 p) {
    vec4 q = vec4(p, 1);
    vec3 v = vec3(dot(o.row0, q), dot(o.row1, q), dot(o.row2, q));
    if (o.kind.x <= 1u) {
        vec3 d = abs(v) - o.shape.xyz;
        return length(max(d, vec3(0))) + min(max(d.x, max(d.y, d.z)), 0) -
               (o.kind.x == 1u ? o.shape.w : 0);
    }
    if (o.kind.x == 2u)
        return length(v) - o.shape.x;
    if (o.kind.x == 3u)
        return (length(v / o.shape.xyz) - 1) *
               min(o.shape.x, min(o.shape.y, o.shape.z));
    v.y -= clamp(v.y, -o.shape.y, o.shape.y);
    return length(v) - o.shape.x;
}
float solidField(vec3 p) {
    float d = solidPrimitive(ops[0], p);
    for (uint i = 1u; i < limits.x; ++i) {
        SolidOp o = ops[i];
        float b = solidPrimitive(o, p);
        if (o.kind.y == 0u)
            d = solidMin(d, b, o.blend.x);
        else if (o.kind.y == 1u)
            d = -solidMin(-d, b, o.blend.x);
        else
            d = -solidMin(-d, -b, o.blend.x);
    }
    return d;
}
vec3 solidNormal(vec3 p, vec3 fallback) {
    float e = spacing.w;
    vec3 g = vec3(solidField(p + vec3(e, 0, 0)) - solidField(p - vec3(e, 0, 0)),
                  solidField(p + vec3(0, e, 0)) - solidField(p - vec3(0, e, 0)),
                  solidField(p + vec3(0, 0, e)) - solidField(p - vec3(0, 0, e)));
    float l = length(g);
    return l > 1e-20 ? g / l : fallback;
}
uint solidIndex(uvec3 c) {
    return c.x + dims.x * (c.y + dims.y * c.z);
}
uvec3 solidCoordinate(uint i, uvec3 d) {
    return uvec3(i % d.x, (i / d.x) % d.y, i / (d.x * d.y));
}
#endif
