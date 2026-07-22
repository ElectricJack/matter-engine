// tileset_slicer.cpp — CPU atlas->layers slicer + per-layer mip builder.
// See tileset_slicer.h for the contract. Pure CPU: no GL/VK/raylib includes.

#include "tileset_slicer.h"

#include <algorithm>
#include <cstring>

namespace tileset {

namespace {

constexpr int kAtlasTiles = 4;  // .gtex atlas is always a fixed 4x4 tile grid.

// Reads a little-endian uint16 pixel value at byte offset `idx` (idx points
// at the low byte).
inline uint32_t read_u16le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

inline void write_u16le(uint8_t* p, uint32_t value) {
    p[0] = (uint8_t)(value & 0xFFu);
    p[1] = (uint8_t)((value >> 8) & 0xFFu);
}

// One 2x2 box-filter mip step. src is src_w x src_h x bpp tightly packed;
// dst is allocated dst_w x dst_h x bpp (dst_w/dst_h are floor(src/2), always
// >= 1 and always such that 2*dst <= src, so the (2x,2y)-(2x+1,2y+1) block
// is always fully inside the source — a trailing odd row/column is simply
// never read, i.e. dropped rather than blended or clamped).
void box_filter_mip(const std::vector<uint8_t>& src, int src_w, int /*src_h*/,
                     int bpp, bool as_u16,
                     std::vector<uint8_t>& dst, int dst_w, int dst_h) {
    dst.assign((size_t)dst_w * (size_t)dst_h * (size_t)bpp, 0);

    if (as_u16) {
        for (int y = 0; y < dst_h; ++y) {
            const int sy0 = y * 2;
            const int sy1 = sy0 + 1;
            for (int x = 0; x < dst_w; ++x) {
                const int sx0 = x * 2;
                const int sx1 = sx0 + 1;
                const uint8_t* p00 = src.data() + ((size_t)sy0 * src_w + sx0) * 2;
                const uint8_t* p10 = src.data() + ((size_t)sy0 * src_w + sx1) * 2;
                const uint8_t* p01 = src.data() + ((size_t)sy1 * src_w + sx0) * 2;
                const uint8_t* p11 = src.data() + ((size_t)sy1 * src_w + sx1) * 2;
                const uint32_t sum = read_u16le(p00) + read_u16le(p10) +
                                      read_u16le(p01) + read_u16le(p11);
                write_u16le(dst.data() + ((size_t)y * dst_w + x) * 2, sum / 4u);
            }
        }
        return;
    }

    for (int y = 0; y < dst_h; ++y) {
        const int sy0 = y * 2;
        const int sy1 = sy0 + 1;
        for (int x = 0; x < dst_w; ++x) {
            const int sx0 = x * 2;
            const int sx1 = sx0 + 1;
            const uint8_t* p00 = src.data() + ((size_t)sy0 * src_w + sx0) * bpp;
            const uint8_t* p10 = src.data() + ((size_t)sy0 * src_w + sx1) * bpp;
            const uint8_t* p01 = src.data() + ((size_t)sy1 * src_w + sx0) * bpp;
            const uint8_t* p11 = src.data() + ((size_t)sy1 * src_w + sx1) * bpp;
            uint8_t* d = dst.data() + ((size_t)y * dst_w + x) * bpp;
            for (int c = 0; c < bpp; ++c) {
                const uint32_t sum = (uint32_t)p00[c] + (uint32_t)p10[c] +
                                      (uint32_t)p01[c] + (uint32_t)p11[c];
                d[c] = (uint8_t)(sum / 4u);
            }
        }
    }
}

}  // namespace

bool slice_channel(const uint8_t* atlas, int atlas_w, int atlas_h,
                   int bytes_per_pixel, bool expand_rgb_to_rgba,
                   bool filter_as_u16,
                   SlicedChannel& out, std::string& err) {
    out = SlicedChannel{};

    if (!atlas) {
        err = "slice_channel: null atlas buffer";
        return false;
    }
    if (atlas_w <= 0 || atlas_h <= 0) {
        err = "slice_channel: atlas dimensions must be positive";
        return false;
    }
    if (atlas_w != atlas_h) {
        err = "slice_channel: atlas must be square (W == H)";
        return false;
    }
    if (atlas_w % kAtlasTiles != 0) {
        err = "slice_channel: atlas dimension must be divisible by 4";
        return false;
    }
    if (bytes_per_pixel != 1 && bytes_per_pixel != 2 && bytes_per_pixel != 3 &&
        bytes_per_pixel != 4) {
        err = "slice_channel: unsupported bytes_per_pixel (must be 1, 2, 3, or 4)";
        return false;
    }
    if (filter_as_u16 && bytes_per_pixel != 2) {
        err = "slice_channel: filter_as_u16 requires bytes_per_pixel == 2";
        return false;
    }
    if (expand_rgb_to_rgba && bytes_per_pixel != 3) {
        err = "slice_channel: expand_rgb_to_rgba requires bytes_per_pixel == 3";
        return false;
    }
    if (expand_rgb_to_rgba && filter_as_u16) {
        err = "slice_channel: expand_rgb_to_rgba and filter_as_u16 are mutually exclusive";
        return false;
    }

    const int tile_px = atlas_w / kAtlasTiles;
    if (tile_px <= 0) {
        err = "slice_channel: derived tile size must be positive";
        return false;
    }

    const int out_bpp = expand_rgb_to_rgba ? 4 : bytes_per_pixel;

    out.tile_px = tile_px;
    out.bytes_per_pixel = out_bpp;
    out.layers.assign((size_t)kAtlasTiles * kAtlasTiles, {});

    for (int row = 0; row < kAtlasTiles; ++row) {
        for (int col = 0; col < kAtlasTiles; ++col) {
            const int layer = row * kAtlasTiles + col;

            // mip 0: row-wise memcpy (or expand) out of the atlas rect for
            // this tile.
            std::vector<uint8_t> mip0((size_t)tile_px * tile_px * out_bpp, 0);
            for (int ty = 0; ty < tile_px; ++ty) {
                const int src_y = row * tile_px + ty;
                const uint8_t* src_row = atlas +
                    ((size_t)src_y * atlas_w + (size_t)col * tile_px) * bytes_per_pixel;
                uint8_t* dst_row = mip0.data() + (size_t)ty * tile_px * out_bpp;
                if (!expand_rgb_to_rgba) {
                    std::memcpy(dst_row, src_row, (size_t)tile_px * bytes_per_pixel);
                } else {
                    for (int tx = 0; tx < tile_px; ++tx) {
                        dst_row[tx * 4 + 0] = src_row[tx * 3 + 0];
                        dst_row[tx * 4 + 1] = src_row[tx * 3 + 1];
                        dst_row[tx * 4 + 2] = src_row[tx * 3 + 2];
                        dst_row[tx * 4 + 3] = 255;
                    }
                }
            }

            std::vector<std::vector<uint8_t>> mips;
            mips.push_back(std::move(mip0));

            int cur_w = tile_px, cur_h = tile_px;
            while (cur_w > 1 || cur_h > 1) {
                const int next_w = std::max(1, cur_w / 2);
                const int next_h = std::max(1, cur_h / 2);
                std::vector<uint8_t> next;
                box_filter_mip(mips.back(), cur_w, cur_h, out_bpp, filter_as_u16,
                                next, next_w, next_h);
                mips.push_back(std::move(next));
                cur_w = next_w;
                cur_h = next_h;
            }

            out.layers[layer] = std::move(mips);
        }
    }

    out.mip_count = (int)out.layers[0].size();
    return true;
}

void mean_rgb(const uint8_t* atlas, int atlas_w, int atlas_h,
              int bytes_per_pixel, float out_rgb[3]) {
    out_rgb[0] = out_rgb[1] = out_rgb[2] = 0.0f;
    if (!atlas || atlas_w <= 0 || atlas_h <= 0 || bytes_per_pixel < 3) {
        return;  // fail closed: degenerate input yields a defined zero mean.
    }

    const size_t n = (size_t)atlas_w * (size_t)atlas_h;
    double sum[3] = {0.0, 0.0, 0.0};
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* px = atlas + i * (size_t)bytes_per_pixel;
        sum[0] += px[0];
        sum[1] += px[1];
        sum[2] += px[2];
    }
    for (int c = 0; c < 3; ++c) {
        out_rgb[c] = (float)((sum[c] / (double)n) / 255.0);
    }
}

}  // namespace tileset
