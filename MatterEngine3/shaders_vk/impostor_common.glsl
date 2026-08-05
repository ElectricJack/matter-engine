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

#endif
