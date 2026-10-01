#ifndef VT_FILL_REQUEST_GLSL
#define VT_FILL_REQUEST_GLSL

// Shared CPU/GPU request layout for compositing and height-gradient resolve.
struct GpuFillRequest {
    uvec4 a;             // x page_x, y page_y, z mip, w out_layer (intermediate)
    uvec4 b;             // x cand_offset, y cand_count, z weight_mode,
                         // w debug materials (mat_a | mat_b << 16)
    vec4 debug_params;   // weight_mode 1: x = blend start (plane U, meters),
                         //                y = blend width (meters)
    uvec4 tape;          // weight modes 2/3: x = tape material ids for columns
                         // 0-3 (u8 each), y = ids for columns 4-7, z = count,
                         // w = field-lane count (mode 3)
    // P2 (weight-seam mode 3): the part's packed tape in the shared op arena
    // plus its world transform.
    uvec4 tape2;         // x = op offset into tape_ops[], y = op count,
                         // z = weight registers for columns 0-3 (u8 each),
                         // w = weight registers for columns 4-7
    vec4 xform0;         // local_to_world rows (row-major 4x3); identity for
    vec4 xform1;         // non-world-anchored parts (never read there: the
    vec4 xform2;         // packer pre-resolved their world ops to consts)
    // P3 (appearance lanes, mode 3): the tape registers the output directives
    // read. x = tint regs (r | g << 8 | b << 16), y = roughbias reg,
    // z = wetness reg, w = pad. Each byte VT_APP_NO_REG (0xFF) when the tape
    // declares no such directive.
    uvec4 app;
    uvec4 coat;
    uvec4 source_a; // RGB and roughness registers
    uvec4 source_b; // metallic, AO, height registers, source version
    vec4 source_range; // minimum/maximum height in metres
    vec4 height_output; // composed min/max, finite source count, reserved
    vec4 material_origin, material_du, material_dv, material_normal;
    uvec4 canonical;
    uvec4 periodic; // logical width/height, producer version, coverage-only flag
    uvec4 export_data; // optional output address in xy
};

#endif
