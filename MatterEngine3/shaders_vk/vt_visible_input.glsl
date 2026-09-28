#ifndef MATTER_VT_VISIBLE_INPUT_GLSL
#define MATTER_VT_VISIBLE_INPUT_GLSL
// RGBA32_UINT visibility: each channel's low 16 bits carry the receiver
// request; its high 16 bits carry the independently addressed material request.
// In the low half of w, bits 0..3 hold mip, bits 4..7 hold snapshot+1.
// Bit 8 marks composed-height ORM.a transport. Zero snapshot means no captured
// inputs. Feedback keeps only the mip bits, stripping both metadata tags.
#if defined(VT_MAX_MIPS) && VT_MAX_MIPS > 16
#error "Visible VT mip field must cover every virtual mip"
#endif
const uint VT_VISIBLE_MIP_MASK = 15u;
const uint VT_VISIBLE_COMPOSED_HEIGHT = 256u;
uint vt_visible_input_word(uint mip, uint snapshot) {
    uint tag = snapshot < 8u ? snapshot + 1u : 0u;
    return (mip & 0xffff0000u) | (mip & VT_VISIBLE_MIP_MASK) | (tag << 4u);
}
// Both requests belong to the SAME depth-visible fragment. Keep the existing
// four-u16 transport for each owner without alternating pixels or frames.
uvec4 vt_pack_visible_feedback(uvec4 receiver, uvec4 material) {
    if (receiver.x == 0u) receiver = uvec4(0u);
    if (material.x == 0u) material = uvec4(0u);
    return (receiver & uvec4(0xffffu)) | (material << 16u);
}
uint vt_visible_input_snapshot(uint word) {
    uint tag = (word >> 4u) & 15u;
    return tag >= 1u && tag <= 8u ? tag - 1u : 0xffffffffu;
}
#endif
