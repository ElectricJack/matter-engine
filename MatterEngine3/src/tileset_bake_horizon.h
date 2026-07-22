#pragma once
// tileset_bake_horizon.h — horizon-map compute pass driver.
//
// Consumes the full-res height channel already produced by bake_primary
// (re-uploaded to a plain R16 image; no BVH/TLAS needed for this pass) and
// produces two quarter-resolution RGBA8 channels packing 8 azimuthal
// horizon-elevation-sin values per texel. See tileset_bake_horizon.comp for
// the exact sampling contract (kHorizonScanRadiusM = 0.30 m, 24 radial
// samples per azimuth, torus wrap addressing across the full atlas).

#include <cstdint>
#include <string>
#include <vector>

namespace tileset {

// full_w/full_h: full-res atlas dimensions (must match the height buffer's
// size, full_w*full_h). texels_per_meter: full-res texels/meter.
// height_min/height_max: world-space height range the R16 buffer was
// normalized against (same values passed to bake_primary).
//
// horizon_w_out/horizon_h_out are set to full_w/4, full_h/4 (integer
// division — flagged by the caller if not exact). horizon_a_rgba8_out /
// horizon_b_rgba8_out are sized horizon_w_out*horizon_h_out*4 on success.
bool bake_horizon(uint32_t program,
                  const std::vector<uint16_t>& height_r16,
                  int full_w, int full_h,
                  int texels_per_meter,
                  float height_min, float height_max,
                  std::vector<uint8_t>& horizon_a_rgba8_out,
                  std::vector<uint8_t>& horizon_b_rgba8_out,
                  int& horizon_w_out, int& horizon_h_out,
                  std::string& err);

} // namespace tileset
