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

// ---------------------------------------------------------------------------
// THE SUN-RAY SKIP for impostor fragments.
//
// An impostor used to be forced fully lit -- rt_shadow.rgen stored 1.0 and
// returned. The reason was real: the card is a plane through the depicted
// object's CENTRE, so the world position reconstructed from its depth lies
// INSIDE the volume, and a shadow ray from there hits the object's own mesh
// (which is still in the TLAS for the caster tier). That measured 48.5 % lit
// against 100 % for the mesh rung, i.e. a card that shadowed itself.
//
// But "do not trace from inside the object" was never a reason to skip
// EXTERNAL occlusion, and forcing 1.0 threw both away. At a 5 degree sun the
// result is a forest where every mesh tree is correctly in shade and every
// impostor behind the handover is blazing lit -- the defect this fixes.
//
// So the ray now starts beyond the object's own bound. The fragment sits at
// most r from the centre (r = the card's world half-extent), so along any
// direction the bound is cleared by 2r; that is the skip. What the ray then
// answers is exactly the right question -- does anything OTHER than me block
// the sun -- while the object's own occlusion keeps coming from the atlas's
// baked AO, so nothing is double-counted.
//
// THE TRADE, stated plainly: occluders nearer than 2r are skipped, so
// near-neighbour tree-on-tree shadowing is not captured. Terrain, ridges and
// mountains -- the dominant term at a low sun, and the whole of the reported
// defect -- are. Tightening this needs the card's CENTRE in the G-buffer, not
// just its radius.
//
// TRANSPORT. out_orm.a is R8G8B8A8_UNORM and normally carries
// horizon_sun_visibility, which is provably 1.0 for an impostor (the branch
// returns before every writer). rt_shadow.rgen is the ONLY reader of that
// channel and is also the shader that tests the impostor bit, so the overload
// is gated by the same predicate that needs it. rt_lighting.rgen and
// composite.frag read .xyz only -- verified, not assumed.
//
// The +1/255 bias makes 8-bit quantisation round the skip UP. Rounding DOWN
// would leave the ray starting inside the volume, which is the self-shadowing
// this exists to avoid -- so the error is spent in the safe direction.
#define IMPOSTOR_SUN_SKIP_MAX_M 64.0

float impostor_sun_skip_encode(float radius_m) {
    return min(1.0, radius_m * (1.0 / IMPOSTOR_SUN_SKIP_MAX_M) + (1.0 / 255.0));
}

float impostor_sun_skip_decode(float encoded) {
    return encoded * IMPOSTOR_SUN_SKIP_MAX_M;
}

#endif
