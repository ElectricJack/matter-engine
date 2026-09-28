#pragma once

// gi_bake_image.h — image encoders for the GI lightmap bake (docs/bake-gi.md).
//
// Three outputs per lightmap, all written by this module with no third-party
// dependency (stb_image_write cannot write 16-bit PNG, and the RGBE writer has
// to emit the new-style RLE scanlines three.js's RGBELoader insists on):
//
//   * Radiance .hdr (RGBE)  — the canonical linear HDR lightmap.
//   * 16-bit PNG            — linear, value / kPng16Scale, for pipelines that
//                             cannot read .hdr. The scale is recorded in the
//                             bake manifest next to the file.
//   * 8-bit PNG             — tone-mapped (see tonemap_ldr) and sRGB-encoded,
//                             the "looks right in an image viewer / map_Ka"
//                             variant.
//
// PNG encoding is zlib "stored" (uncompressed deflate blocks) so the bytes are
// a pure function of the pixels: no compressor version can change them, which
// is what keeps the determinism test byte-exact across builds.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gi_bake_image {

// Linear value that maps to 65535 in the 16-bit PNG.
constexpr float kPng16Scale = 8.0f;

uint32_t crc32(const uint8_t* data, size_t len, uint32_t seed = 0u);
uint32_t adler32(const uint8_t* data, size_t len);

// Linear [0,1] -> sRGB [0,1].
float linear_to_srgb(float v);

// Reinhard with a white point (W = 4): v * (1 + v/W^2) / (1 + v), so 1.0 maps
// to ~0.53 and 4.0 to 1.0. Output is clamped to [0, 1]; the caller encodes it
// with linear_to_srgb for the 8-bit PNG.
float tonemap_ldr(float v);

// Encode an 8-bit or 16-bit RGB PNG into `out`. `pixels` holds w*h*3 samples;
// 16-bit samples are host uint16 values (the encoder writes them big-endian as
// PNG requires). Returns false only on a degenerate size.
bool encode_png_rgb8 (uint32_t w, uint32_t h, const uint8_t*  pixels, std::vector<uint8_t>& out);
bool encode_png_rgb16(uint32_t w, uint32_t h, const uint16_t* pixels, std::vector<uint8_t>& out);

// Encode a Radiance RGBE image (new-style RLE scanlines, "-Y h +X w").
// `rgb` holds w*h*3 linear floats; negatives clamp to 0, NaN/inf to 0.
bool encode_hdr(uint32_t w, uint32_t h, const float* rgb, std::vector<uint8_t>& out);

// Decode helpers used by the tests (and by the cache round-trip): RGBE back to
// float, both scanline encodings. Returns false on a malformed stream.
bool decode_hdr(const uint8_t* data, size_t len, uint32_t& w, uint32_t& h,
                std::vector<float>& rgb);

// Convert a linear float lightmap into the two PNG payloads.
void to_rgb16(const float* rgb, size_t texels, std::vector<uint16_t>& out);
void to_rgb8_tonemapped(const float* rgb, size_t texels, std::vector<uint8_t>& out);

// Write bytes atomically-ish (temp file + rename). Returns false on failure.
bool write_file(const std::string& path, const std::vector<uint8_t>& bytes);

} // namespace gi_bake_image
