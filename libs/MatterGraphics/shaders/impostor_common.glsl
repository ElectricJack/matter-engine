// M2.5 impostor atlas layout — the ONE place the view grid is written down.
//
// The C++ side asserts these against impostor::kViews / kGridDim / kCellPx
// (see MatterEngine3/src/render/vk_scene_renderer.cpp), so a change to the
// bake that is not mirrored here fails the build rather than producing a
// silently mis-sampled atlas.
//
// 16 azimuth views x 3 elevation rings = 48 views in an 8x8 grid of 16x16
// cells, two RGBA8 layers per part impostor (shade, then tint) = 128 KiB. The
// derivation of every number is in MatterEngine3/src/impostor_bake.h; the
// rings cost nothing because the cell resolution was never the binding
// constraint and paid for them.
#ifndef IMPOSTOR_COMMON_GLSL
#define IMPOSTOR_COMMON_GLSL

#define IMPOSTOR_VIEWS 48u
#define IMPOSTOR_GRID_DIM 8u
#define IMPOSTOR_AZIMUTHS 16u
#define IMPOSTOR_ELEVATIONS 3u
// Radians between elevation rings (30 degrees). Ring r is baked r steps above
// the equator and the NEAREST is picked, so boundaries fall halfway between.
#define IMPOSTOR_ELEV_STEP 0.52359877559829887308

// The sentinel the terminal billboard rung carries in surface.x. Chosen far
// above any legitimate chart UV (which lives in [0,1]) so the test is a plain
// comparison with no extra channel.
const float kImpostorMarker = 1.0e29;

// ---------------------------------------------------------------------------
// ALPHA CUTOUT for the baked shade layer.
//
// This is NOT the usual "0.5 because that is what everyone uses". At kCellPx =
// 16 the cell's texels are COARSE relative to the features a tree has, and a
// 0.5 cutout deletes every feature narrower than one texel -- coverage of a
// half-texel-wide trunk can never reach 0.5 no matter where the texel lands.
//
// Measured with the real bake_cluster against a synthetic spruce (true height
// 3.64 m, 3.55 m of it inside the impostor's bound), equator-ring cell, where
// one texel is 0.274 m and the trunk is 0.21 m across:
//
//     any coverage (alpha > 0)   3.56 m tall
//     alpha >= 0.5               1.92 m tall, 1.09 m wide   <-- what was drawn
//     alpha >= 0.25              ~3.56 m tall, trunk included
//
// So the old threshold drew 53 % of the tree's height: the trunk and the crown
// fringe were gone and what remained was a blob centred on the card, which is
// exactly the "tall trees SHRINK at the impostor switch" report. The card's
// geometry and centre were verified correct first; this is the cutout alone.
//
// Why 0.25 is safe against the neighbouring view. Lowering the cutout grows
// the silhouette by at most half a texel (the partially-covered rim), and
// impostor_bake.cpp's guard band (kGuardBand = 1.20) already holds the
// silhouette 1.33 texels clear of the cell border -- see the static_assert
// there, which exists to keep that margin above one texel. 1.33 > 0.5, so no
// azimuth neighbour can bleed in. Recheck this if kCellPx or kGuardBand moves.
//
// Runtime-only: the bake stores alpha, not a cutout decision, so nothing here
// touches a baked byte, a content hash or a cache key. No rebake.
const float kImpostorAlphaCutout = 0.25;

// ---------------------------------------------------------------------------
// The impostor bit in the identity (material/instance) G-buffer attachment.
//
// out_material_instance.x is R32_UINT carrying a MATERIAL INDEX, bounded by
// the material count (a few thousand at the extreme), so the top bit is free
// and is the cheapest place to tell a deferred consumer "this fragment is a
// flat card standing in for a volume". rt_shadow.rgen needs that: an impostor
// must not RECEIVE a traced sun shadow, because the card's reconstructed
// world position sits inside the volume the shadow would come from (the same
// pathology measured in VkSceneRenderer::build_ray_geometry).
//
// EVERY other reader of .x must mask the bit off before using it as an index.
// The failure mode of forgetting is quiet and plausible rather than loud: the
// bounds test `identity.x < materials.length()` simply fails for an impostor
// pixel, so it silently loses subsurface, emission, transmission and metal
// response and still renders. Use these two helpers rather than open-coding
// the mask.
#define IMPOSTOR_IDENTITY_BIT 0x80000000u

uint impostor_identity_material(uint identity_x) {
    return identity_x & ~IMPOSTOR_IDENTITY_BIT;
}

bool impostor_identity_is_card(uint identity_x) {
    return (identity_x & IMPOSTOR_IDENTITY_BIT) != 0u;
}

// The sun-ray skip that used to live here is gone. It encoded the card's
// radius through out_orm.a so rt_shadow.rgen could start its ray outside the
// object, which was only ever necessary because an impostor pixel reported the
// plane through the object's centre. The atlas now carries per-texel DEPTH and
// gbuffer.frag writes the real surface, so the ray starts on the tree like any
// other surface and the whole mechanism is unnecessary.

#endif
