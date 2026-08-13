#version 460

// THE OCCLUSION ID PASS, vertex half (M4).
//
// Renders the candidate set into a small target that carries nothing but
// "which instance owns this pixel", with depth testing on. The set of distinct
// ids left in it after the pass IS the visible set -- exactly, per pixel,
// through cracks, with no bounding box anywhere in the argument.
//
// This is why it beats the alternatives it replaced. An HZB asks "is this box
// definitely hidden", which on streamed terrain means testing a box the size of
// a whole cube tile and failing open almost every time. Reading the G-buffer's
// identity attachment answers the right question but only about what was
// already DRAWN, which is circular: cull something and it can never report
// itself visible again. This pass breaks the circularity by rendering the whole
// candidate set every frame -- a hidden sector is still TESTED, it is only
// excluded from the expensive pass.
//
// Deliberately the narrowest vertex shader in the engine: position in, clip
// position and a flat token out. No normal, no tint, no surface, no material,
// no VT slot, no warp field, no velocity, no previous transform. The whole
// point is that this pass costs almost nothing, and every varying is a cost
// paid per vertex for an answer nobody reads.

// The vertex layout is the static VkRasterVertex, so the same vertex buffer
// binds unchanged. Only location 0 is consumed; the rest are declared because
// the pipeline's input state describes all of them and a shader that omits an
// attribute is fine, but keeping the numbering documented here stops the next
// person wondering whether this is a different vertex format. It is not.
layout(location = 0) in vec3 in_position;

layout(location = 0) flat out uint out_instance_token;

layout(set = 0, binding = 0, std140) uniform FrameConstants {
    mat4 world_to_clip;
    mat4 previous_world_to_clip;
    vec4 frustum_planes[6];
    vec4 camera_eye_pixel_budget;
    uvec4 counts;
    uvec4 capacities;
    uvec4 temporal;
    uvec4 vis_params;
    // The CULL camera's projection -- equal to world_to_clip normally, pinned
    // when the cull camera is frozen. This pass must use it and not
    // world_to_clip, which follows the live camera so the frame can be drawn
    // from wherever you fly. Rasterising the live view would mean the mask
    // describes what is visible from the INSPECTION viewpoint, so freezing the
    // cull and flying out to look at the result made everything visible again
    // and the cull stopped rejecting anything -- reported as "culling doesn't
    // hide anything", and correctly so.
    mat4 cull_world_to_clip;
} frame;

// Binding 18, NOT the 3 that raster.vert reads. That is the whole point: 3
// holds the list the mask has already filtered, and rasterising it would mean
// deciding visibility from evidence visibility itself produced -- cull one
// sector and it is absent from the pass that could bring it back. 18 is the
// unfiltered list, written by the same cull invocation before it consults the
// mask.
//
// Reusing scene set 1 rather than a set of its own is what this binding buys:
// the ID pass binds exactly what the gbuffer pass binds.
struct DrawTransform {
    mat4 current;
    mat4 previous;
    uint history_valid;
    uint instance_token;
    uint vt_slot;
    uint selected_lod;
};
layout(set = 1, binding = 17, std430) readonly buffer VisDrawTransforms {
    DrawTransform transforms[];
};

void main() {
    // gl_InstanceIndex already includes VkDrawIndirectCommand::firstInstance,
    // exactly as raster.vert documents at its own lookup.
    const DrawTransform draw = transforms[gl_InstanceIndex];
    out_instance_token = draw.instance_token;
    gl_Position =
        frame.cull_world_to_clip * (draw.current * vec4(in_position, 1.0));
}
