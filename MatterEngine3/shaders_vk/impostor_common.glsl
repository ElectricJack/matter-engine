// M2.5 impostor atlas layout — the ONE place the view grid is written down.
//
// The C++ side asserts these against impostor::kViews / kGridDim / kCellPx
// (see MatterEngine3/src/render/vk_scene_renderer.cpp), so a change to the
// bake that is not mirrored here fails the build rather than producing a
// silently mis-sampled atlas.
//
// 16 azimuth views in a 4x4 grid of 32x32 cells, two RGBA8 layers per part
// impostor (shade, then tint) = 128 KiB. The derivation of both numbers is in
// MatterEngine3/src/impostor_bake.h.
#ifndef IMPOSTOR_COMMON_GLSL
#define IMPOSTOR_COMMON_GLSL

#define IMPOSTOR_VIEWS 16u
#define IMPOSTOR_GRID_DIM 4u

// The sentinel the terminal billboard rung carries in surface.x. Chosen far
// above any legitimate chart UV (which lives in [0,1]) so the test is a plain
// comparison with no extra channel.
const float kImpostorMarker = 1.0e29;

#endif
