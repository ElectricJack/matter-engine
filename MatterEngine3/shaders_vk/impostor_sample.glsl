#ifndef MATTER_IMPOSTOR_SAMPLE_GLSL
#define MATTER_IMPOSTOR_SAMPLE_GLSL
#include "material_common.glsl"
#include "impostor_common.glsl"

// Production cutout, view-safe mip sampling, tint, normal and depth decoding.
// Atlas preparation remains external. This function only samples its product.
struct ImpostorSample { vec3 color; vec3 normal; float depth; };
ImpostorSample sample_impostor(sampler2DArray impostorAtlas, MaterialGpu material,
    vec4 in_tint, vec4 in_surface, vec4 in_impostor_parallax,
    vec3 in_model_basis_x, vec3 in_model_basis_y, vec3 in_world_pos,
    vec3 camera_eye, mat4 world_to_clip, uint parallax_enabled, uint wireframe_enabled) {
    vec3 base_color, shading_normal;
    float impostor_frag_depth = -1.0;
    const uint slot = uint(in_tint.x + 0.5) | (uint(in_tint.y + 0.5) << 8);
    const uint view = uint(in_tint.z + 0.5);
    const vec2 cell = vec2(float(view % IMPOSTOR_GRID_DIM),
                           float(view / IMPOSTOR_GRID_DIM));
    // Clamp inside the cell so a bilinear tap at the quad's edge cannot
    // reach the neighbouring view even before the bake's guard band.
    // ---- PARALLAX -------------------------------------------------
    //
    // The cell was baked from a QUANTISED direction; the card faces the
    // real eye. raster.vert hands over the cell-uv displacement per unit
    // of stored depth, so a surface d behind the card is found at
    // base_uv + parallax * d. Two taps: one to read the depth under the
    // fragment, one to re-read it at the corrected position. That second
    // step is what stops a silhouette from smearing when the first sample
    // lands on background -- one step alone is visibly wrong at grazing
    // angles, and a full march costs 8-32 taps for a rung whose entire
    // purpose is to be cheap.
    //
    // CLAMPED TO THE CELL, and that clamp is load-bearing: cells are
    // packed edge to edge with only a two-texel guard band, while a
    // parallax offset can reach ~20 % of a cell at the widest intra-cell
    // angle. Unclamped it would sample the neighbouring azimuth outright.
    // The cost of clamping is a smear where the offset would have left the
    // cell, which is bounded and local; the cost of not clamping is
    // another view's geometry welded to this one.
    //
    // TOGGLE (parallax_enabled, default 1). Off skips
    // both depth taps and samples the cell straight -- the card reads as a
    // flat billboard of its baked view. That is the honest comparison for
    // deciding whether the two taps earn their cost; it is NOT a correctness
    // fix, and off is visibly flatter at grazing angles.
    const vec2 base_uv = clamp(in_surface.yz, 0.0, 1.0);
    const float cell_pixels = float(textureSize(impostorAtlas, 0).x) / float(IMPOSTOR_GRID_DIM);
    const float footprint = max(length(dFdx(base_uv)), length(dFdy(base_uv))) * cell_pixels;
    const float atlas_lod = clamp(log2(max(footprint, 1.0)), 0.0,
                                  float(textureQueryLevels(impostorAtlas) - 1));
    // The coarser level of a trilinear tap must stay inside this view.
    const vec2 inset = vec2(0.5 * exp2(ceil(atlas_lod)) / cell_pixels);
    const vec2 parallax = parallax_enabled != 0u
                              ? in_impostor_parallax.xy
                              : vec2(0.0);
    vec2 local = base_uv;
    if (parallax_enabled != 0u) {
        float probe = textureLod(impostorAtlas,
                              vec3((cell + clamp(local, inset, 1.0-inset)) / float(IMPOSTOR_GRID_DIM),
                                   float(slot * 2u)), atlas_lod).b;
        local = clamp(base_uv + parallax * probe, 0.0, 1.0);
        probe = textureLod(impostorAtlas,
                        vec3((cell + clamp(local, inset, 1.0-inset)) / float(IMPOSTOR_GRID_DIM),
                             float(slot * 2u)), atlas_lod).b;
        local = clamp(base_uv + parallax * probe, 0.0, 1.0);
    }
    const vec2 uv = (cell + clamp(local, inset, 1.0-inset)) / float(IMPOSTOR_GRID_DIM);
    const vec4 shade = textureLod(impostorAtlas, vec3(uv, float(slot * 2u)), atlas_lod);
    // Alpha cutout. gl_FragDepth is already written by this shader, so
    // early depth WRITE was off regardless; discard costs the depth TEST
    // nothing extra here.
    //
    // kImpostorAlphaCutout is 0.25, not the customary 0.5, and
    // impostor_common.glsl carries the measurement: at a 16 px cell a 0.5
    // cutout erased the trunk and crown fringe of a spruce and drew 53 %
    // of its height, which is what made tall trees appear to SHRINK at the
    // switch. Read that comment before touching this number.
    //
    // The WIREFRAME view is exempt, and that exemption is a diagnostic
    // rather than a nicety: wireframe exists to answer "what geometry is
    // being drawn", and a billboard whose every fragment discards is
    // indistinguishable from a rung that was never selected. Keeping the
    // quad's edges visible under wireframe separates "the tier did not
    // draw" from "the tier drew nothing" -- two failures that look
    // identical on screen and have nothing else in common.
    if (shade.a < kImpostorAlphaCutout &&
        wireframe_enabled == 0u) discard;
    const vec4 baked_tint = textureLod(impostorAtlas, vec3(uv, float(slot * 2u + 1u)), atlas_lod);
    base_color = resolveBaseColor(material, baked_tint);
    // The B channel is DEPTH now, not AO (it was a constant 1.0 for every
    // DSL-built part). Leaving `ao` at the vertex default keeps ambient
    // exactly where it was rather than multiplying it by a depth.

    // ---- CONSERVATIVE DEPTH ----------------------------------------
    //
    // Every impostor pixel used to report the card's plane. That single
    // fact is why a shadow ray from one started INSIDE the volume, why
    // trunks did not meet the ground, and why anything depth-derived
    // treated a tree as a sheet. The card now sits at the near bound and
    // this pushes each pixel back to the surface it actually depicts.
    //
    // Only ever backwards: the depth is measured from the near bound the
    // card sits on, so this satisfies `layout(depth_less)` by
    // construction rather than by clamping.
    const vec3 view_ray = in_world_pos - camera_eye;
    const float view_len = length(view_ray);
    if (view_len > 1e-5) {
        const vec3 surface_world =
            in_world_pos + (view_ray / view_len) *
                               (shade.b * in_impostor_parallax.z);
        const vec4 clip = world_to_clip * vec4(surface_world, 1.0);
        if (clip.w > 0.0) impostor_frag_depth = clip.z / clip.w;
    }
    // Octahedral decode, the exact inverse of impostor_bake.cpp's encode.
    vec2 e = shade.rg * 2.0 - 1.0;
    vec3 n = vec3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0,
                                        n.y >= 0.0 ? 1.0 : -1.0);
    n = normalize(n);
    const vec3 bz = cross(in_model_basis_x, in_model_basis_y);
    shading_normal = normalize(in_model_basis_x * n.x +
                               in_model_basis_y * n.y + bz * n.z);
    return ImpostorSample(base_color, shading_normal, impostor_frag_depth);
}
#endif
