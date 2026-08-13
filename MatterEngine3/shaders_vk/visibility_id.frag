#version 460

// THE OCCLUSION ID PASS, fragment half (M4). See visibility_id.vert.
//
// One store. No lighting, no material lookup, no virtual texture, no alpha
// test, no tileset parallax, no discard -- the depth test alone decides which
// instance owns the pixel, which is the entire question this pass exists to
// answer.
//
// The target is R32G32_UINT with the token in .y, matching the G-buffer's
// identity attachment even though .x is unused here. That is not laziness: it
// lets visible_ids.comp reduce EITHER source with no variant and no branch, so
// the mask this pass produces and the mask the G-buffer produces are built by
// the same code and can be compared for equality. A separate single-channel
// format would have meant a second reduce shader, and then the comparison
// would be testing two implementations rather than two inputs.

layout(location = 0) flat in uint in_instance_token;

layout(location = 0) out uvec2 out_id;

void main() {
    out_id = uvec2(0u, in_instance_token);
}
