#ifndef MATTER_GEOMETRY_CUT_GLSL
#define MATTER_GEOMETRY_CUT_GLSL
// Binary replacement hierarchy. Descriptor indices are snapshot-local; only
// nodes with raster AND RT resources published may carry READY. Consumer owns
// node/root buffers and emission/request functions. Never visit source leaves
// below a selected parent. Work/output exhaustion preserves parent coverage.
struct GeometryCutNode {
    vec4 bounds_min_error;
    vec4 bounds_max;
    uvec4 links; // child 0, child 1, ready, renderer payload
};
GeometryCutNode geometry_node(uint index);
vec4 geometry_plane(uint index);
// Transform the plane into local space, then test the AABB support point.
// This remains conservative under nonuniform scale, reflection and shear.
bool geometry_visible(GeometryCutNode node, mat4 transform) {
    vec3 centre=(node.bounds_min_error.xyz+node.bounds_max.xyz)*0.5;
    vec3 extent=(node.bounds_max.xyz-node.bounds_min_error.xyz)*0.5;
    for(uint i=0u;i<6u;++i){
        vec4 plane=transpose(transform)*geometry_plane(i);
        float distance=dot(plane.xyz,centre)+plane.w;
        float radius=dot(abs(plane.xyz),extent);
        float tolerance=1e-4*(1.0+abs(distance)+radius);
        if(distance+radius < -tolerance)return false;
    }
    return true;
}
const uint GEOMETRY_NO_CHILD = 0xffffffffu;

// Error-authored distance form of lod_distance.h. The host supplies a
// conservative transform scale (including shear) and pixel angle, once per
// instance. This is the same normalized switch-distance rule as mesh LODs.
bool geometry_refine(GeometryCutNode node, mat4 transform, vec3 eye,
                     float error_reach) {
    vec3 centre = (transform * vec4((node.bounds_min_error.xyz +
                                    node.bounds_max.xyz) * 0.5, 1.0)).xyz;
    float radius = length(node.bounds_max.xyz - node.bounds_min_error.xyz) * 0.5;
    float distance_to_eye = max(0.01, distance(centre, eye) - radius * geometry_scale());
    return node.bounds_min_error.w > 0.0 &&
           distance_to_eye < node.bounds_min_error.w * error_reach;
}

void geometry_select(uint root_count, uint max_nodes, uint max_selected,
                     mat4 transform, vec3 eye, float error_reach) {
    if (root_count == 0u || root_count > max_selected || max_nodes == 0u) {
        geometry_invalid(); return;
    }
    // Mandatory admission is transactional: no partial root set is emitted.
    for (uint root = 0u; root < root_count; ++root)
        if (!geometry_ready(geometry_root(root))) { geometry_invalid(); return; }
    uint stack_node[65];
    uint stack_depth[65];
    uint visited = 0u, selected = 0u;
    for (uint root = 0u; root < root_count; ++root) {
        uint pending = 1u;
        stack_node[0] = geometry_root(root); stack_depth[0] = 0u;
        while (pending != 0u) {
            --pending;
            uint index = stack_node[pending], depth = stack_depth[pending];
            GeometryCutNode node = geometry_node(index);
            if (!geometry_visible(node, transform)) continue;
            bool inspect = visited < max_nodes;
            if (inspect) { ++visited; geometry_visit(); }
            bool replace = false;
            // Exhausted traversal/depth still emits coverage, but cannot
            // truthfully report that the requested visible detail is ready.
            if ((!inspect || depth >= 64u) && node.links.x != GEOMETRY_NO_CHILD &&
                geometry_refine(node, transform, eye, error_reach)) geometry_fallback();
            if (inspect && node.links.x != GEOMETRY_NO_CHILD && depth < 64u &&
                geometry_refine(node, transform, eye, error_reach)) {
                uint outstanding_roots = root_count - root - 1u;
                bool room = outstanding_roots + pending + selected + 2u <= max_selected;
                bool children_ready = room && geometry_ready(node.links.x) && geometry_ready(node.links.y);
                if (children_ready && pending + 2u <= 65u) {
                    stack_node[pending] = node.links.y; stack_depth[pending++] = depth + 1u;
                    stack_node[pending] = node.links.x; stack_depth[pending++] = depth + 1u;
                    replace = true;
                } else {
                    geometry_fallback();
                    if (room) {
                        if (!geometry_ready(node.links.x)) geometry_request(node.links.x);
                        if (!geometry_ready(node.links.y)) geometry_request(node.links.y);
                    }
                }
            }
            if (!replace) { geometry_emit(index); ++selected; }
        }
    }
}
#endif
