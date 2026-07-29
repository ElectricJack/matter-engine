// bc_encode.cpp — CPU BC7/BC5/BC4 block compression. See bc_encode.h for the
// contract (determinism, edge-strip invariant, fail-closed rules).
//
// Vendored backends (third_party/bc7enc, MIT or public domain):
//   BC7      -> bc7enc_compress_block()   (modes 1 and 6)
//   BC5/BC4  -> rgbcx::encode_bc5/bc4()
// Both are block-local and table-driven; neither allocates, randomizes, or
// keeps state between blocks after its one-time init.

#include "bc_encode.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <thread>

#include "bc7enc.h"
#include "rgbcx.h"

namespace tileset {

namespace {

// One-time table construction for both backends. Neither init is thread-safe
// (they write file-static tables), so both go behind one call_once; every
// encode entry point calls this first.
std::once_flag g_backend_init_once;

void ensure_backends_initialized() {
    std::call_once(g_backend_init_once, [] {
        bc7enc_compress_block_init();
        rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal);
    });
}

// BC7 encoder settings. Deliberately fixed constants, never tuned per call:
// the compressed bytes are part of cache identity downstream, so a settings
// change must be a visible source edit, not a runtime knob.
//
//  * LINEAR weights, not perceptual. Only the albedo channel is colour;
//    ORM (occlusion/roughness/metallic) packs three unrelated scalars into
//    RGB and a YCbCr error metric would happily trade roughness precision for
//    "chroma" that means nothing. One setting for both keeps the encoder
//    channel-agnostic.
//  * m_max_partitions 16 (of 64) + the mode-1/7 estimation filterbank: the
//    dominant cost in bc7enc is the mode-1 partition search. 16 costs
//    ~0.1 dB against 64 on tileset content and is ~3x faster, which matters
//    because a 4096-atlas slot is ~44 Mtexel of BC7 per load.
//  * m_uber_level 0: uber levels re-run the whole search with perturbed
//    endpoints; not worth several extra seconds per slot load.
bc7enc_compress_block_params bc7_params() {
    bc7enc_compress_block_params p;
    bc7enc_compress_block_params_init(&p);
    bc7enc_compress_block_params_init_linear_weights(&p);
    p.m_max_partitions = 16;
    p.m_uber_level = 0;
    p.m_mode17_partition_estimation_filterbank = true;
    return p;
}

// Gather one 4x4 block out of a tightly packed w*h*bpt image into `dst`
// (16 texels, `bpt` bytes each, raster order). Texels past the right/bottom
// edge clamp to the last real row/column — see bc_encode.h on why not zeros.
// Fully-interior blocks (the common case, and every block the edge-strip
// invariant depends on) take the memcpy path and never clamp.
inline void gather_block(const uint8_t* src, int w, int h, int bpt,
                         int bx, int by, uint8_t* dst) {
    const int x0 = bx * 4;
    const int y0 = by * 4;
    if (x0 + 4 <= w && y0 + 4 <= h) {
        for (int r = 0; r < 4; ++r) {
            std::memcpy(dst + (size_t)r * 4 * bpt,
                        src + ((size_t)(y0 + r) * w + x0) * bpt,
                        (size_t)4 * bpt);
        }
        return;
    }
    for (int r = 0; r < 4; ++r) {
        const int sy = std::min(y0 + r, h - 1);
        for (int c = 0; c < 4; ++c) {
            const int sx = std::min(x0 + c, w - 1);
            std::memcpy(dst + ((size_t)r * 4 + c) * bpt,
                        src + ((size_t)sy * w + sx) * bpt, (size_t)bpt);
        }
    }
}

// Resolve the worker count for `blocks_y` block rows. 0 = auto. Capped so tiny
// mip levels never pay thread-spawn cost, and so a 96-core host does not spawn
// 96 threads for a 4-row image.
unsigned worker_count(unsigned max_threads, int blocks_y) {
    if (max_threads == 1) return 1;
    unsigned n = max_threads;
    if (n == 0) {
        n = std::thread::hardware_concurrency();
        if (n == 0) n = 1;
        n = std::min(n, 16u);
    }
    // At least 8 block rows per worker, else stay serial.
    const unsigned by_rows = (unsigned)std::max(1, blocks_y / 8);
    return std::max(1u, std::min(n, by_rows));
}

// Shared driver: validate, size the output, then run `encode_block` over every
// block. Workers own disjoint block-row ranges and disjoint output byte
// ranges, so the result does not depend on the thread count or on scheduling.
template <typename EncodeBlockFn>
bool encode_blocks(const uint8_t* src, int w, int h, BcFormat format,
                   std::vector<uint8_t>& out, std::string& err,
                   unsigned max_threads, EncodeBlockFn encode_block) {
    out.clear();
    if (!src) {
        err = "bc_encode: null source buffer";
        return false;
    }
    if (w <= 0 || h <= 0) {
        err = "bc_encode: extents must be positive";
        return false;
    }
    const size_t total = bc_encoded_size(format, w, h);
    if (total == 0) {
        err = "bc_encode: computed zero output size";
        return false;
    }

    ensure_backends_initialized();

    const int bpt = bc_source_bytes_per_texel(format);
    const int block_bytes = bc_block_bytes(format);
    const int blocks_x = bc_blocks_across(w);
    const int blocks_y = bc_blocks_across(h);
    out.assign(total, 0);

    auto run_rows = [&](int y_begin, int y_end) {
        uint8_t block[16 * 4];  // 16 texels, at most 4 bytes each
        for (int by = y_begin; by < y_end; ++by) {
            uint8_t* row = out.data() +
                           (size_t)by * blocks_x * (size_t)block_bytes;
            for (int bx = 0; bx < blocks_x; ++bx) {
                gather_block(src, w, h, bpt, bx, by, block);
                encode_block(row + (size_t)bx * block_bytes, block);
            }
        }
    };

    const unsigned workers = worker_count(max_threads, blocks_y);
    if (workers <= 1) {
        run_rows(0, blocks_y);
        return true;
    }
    std::vector<std::thread> pool;
    pool.reserve(workers - 1);
    const int per = (blocks_y + (int)workers - 1) / (int)workers;
    for (unsigned i = 1; i < workers; ++i) {
        const int begin = std::min((int)i * per, blocks_y);
        const int end = std::min(begin + per, blocks_y);
        if (begin >= end) break;
        pool.emplace_back([&run_rows, begin, end] { run_rows(begin, end); });
    }
    run_rows(0, std::min(per, blocks_y));
    for (auto& t : pool) t.join();
    return true;
}

}  // namespace

size_t bc_encoded_size(BcFormat format, int w, int h) {
    if (w <= 0 || h <= 0) return 0;
    return (size_t)bc_blocks_across(w) * (size_t)bc_blocks_across(h) *
           (size_t)bc_block_bytes(format);
}

const char* bc_format_name(BcFormat format) {
    switch (format) {
        case BcFormat::kBc7: return "BC7_UNORM_BLOCK";
        case BcFormat::kBc5: return "BC5_UNORM_BLOCK";
        case BcFormat::kBc4: return "BC4_UNORM_BLOCK";
    }
    return "BC_UNKNOWN";
}

bool encode_bc7(const uint8_t* src_rgba8, int w, int h,
                std::vector<uint8_t>& out, std::string& err,
                unsigned max_threads) {
    const bc7enc_compress_block_params params = bc7_params();
    return encode_blocks(src_rgba8, w, h, BcFormat::kBc7, out, err, max_threads,
                         [&params](uint8_t* dst, const uint8_t* block) {
                             bc7enc_compress_block(dst, block, &params);
                         });
}

bool encode_bc5(const uint8_t* src_rg8, int w, int h,
                std::vector<uint8_t>& out, std::string& err,
                unsigned max_threads) {
    // stride 2: the gathered block is 16 tightly packed RG pairs, channel 0
    // -> BC5 red half, channel 1 -> BC5 green half.
    return encode_blocks(src_rg8, w, h, BcFormat::kBc5, out, err, max_threads,
                         [](uint8_t* dst, const uint8_t* block) {
                             rgbcx::encode_bc5(dst, block, 0, 1, 2);
                         });
}

bool encode_bc4(const uint8_t* src_r8, int w, int h,
                std::vector<uint8_t>& out, std::string& err,
                unsigned max_threads) {
    return encode_blocks(src_r8, w, h, BcFormat::kBc4, out, err, max_threads,
                         [](uint8_t* dst, const uint8_t* block) {
                             rgbcx::encode_bc4(dst, block, 1);
                         });
}

void requantize_r16le_to_r8(const uint8_t* src_r16le, size_t texel_count,
                            std::vector<uint8_t>& out) {
    out.assign(texel_count, 0);
    if (!src_r16le) return;
    for (size_t i = 0; i < texel_count; ++i) {
        const uint32_t v = (uint32_t)src_r16le[i * 2] |
                           ((uint32_t)src_r16le[i * 2 + 1] << 8);
        // Round-to-nearest unorm16 -> unorm8: (v*255 + 32767) / 65535. Exact
        // at both endpoints (0 -> 0, 65535 -> 255), no floating point.
        out[i] = (uint8_t)((v * 255u + 32767u) / 65535u);
    }
}

bool compress_sliced_channel(const SlicedChannel& src, BcFormat format,
                             bool src_is_r16le, CompressedChannel& out,
                             std::string& err, unsigned max_threads) {
    out = CompressedChannel{};

    if (src.tile_px <= 0 || src.mip_count <= 0 || src.layers.empty()) {
        err = "compress_sliced_channel: empty/degenerate sliced channel";
        return false;
    }
    if (src_is_r16le && format != BcFormat::kBc4) {
        err = "compress_sliced_channel: src_is_r16le is only valid for BC4";
        return false;
    }
    const int expect_bpp =
        src_is_r16le ? 2 : bc_source_bytes_per_texel(format);
    if (src.bytes_per_pixel != expect_bpp) {
        err = std::string("compress_sliced_channel: ") + bc_format_name(format) +
              " needs bytes_per_pixel " + std::to_string(expect_bpp) +
              ", sliced channel has " + std::to_string(src.bytes_per_pixel);
        return false;
    }

    out.tile_px = src.tile_px;
    out.mip_count = src.mip_count;
    out.format = format;
    out.layers.assign(src.layers.size(), {});

    std::vector<uint8_t> requantized;
    for (size_t layer = 0; layer < src.layers.size(); ++layer) {
        const auto& src_mips = src.layers[layer];
        if ((int)src_mips.size() != src.mip_count) {
            err = "compress_sliced_channel: ragged mip chain on layer " +
                  std::to_string(layer);
            return false;
        }
        out.layers[layer].resize((size_t)src.mip_count);

        int dim = src.tile_px;
        for (int mip = 0; mip < src.mip_count; ++mip) {
            const std::vector<uint8_t>& mip_bytes = src_mips[(size_t)mip];
            const size_t expected =
                (size_t)dim * (size_t)dim * (size_t)src.bytes_per_pixel;
            if (mip_bytes.size() != expected) {
                err = "compress_sliced_channel: mip byte-size mismatch (layer " +
                      std::to_string(layer) + " mip " + std::to_string(mip) +
                      "): got " + std::to_string(mip_bytes.size()) +
                      ", expected " + std::to_string(expected);
                return false;
            }

            const uint8_t* pixels = mip_bytes.data();
            if (src_is_r16le) {
                requantize_r16le_to_r8(mip_bytes.data(),
                                       (size_t)dim * (size_t)dim, requantized);
                pixels = requantized.data();
            }

            bool ok = false;
            switch (format) {
                case BcFormat::kBc7:
                    ok = encode_bc7(pixels, dim, dim,
                                    out.layers[layer][(size_t)mip], err,
                                    max_threads);
                    break;
                case BcFormat::kBc5:
                    ok = encode_bc5(pixels, dim, dim,
                                    out.layers[layer][(size_t)mip], err,
                                    max_threads);
                    break;
                case BcFormat::kBc4:
                    ok = encode_bc4(pixels, dim, dim,
                                    out.layers[layer][(size_t)mip], err,
                                    max_threads);
                    break;
            }
            if (!ok) {
                out = CompressedChannel{};
                return false;
            }
            dim = std::max(1, dim / 2);
        }
    }
    return true;
}

size_t compressed_channel_bytes(const CompressedChannel& c) {
    size_t total = 0;
    for (const auto& layer : c.layers)
        for (const auto& mip : layer) total += mip.size();
    return total;
}

size_t sliced_channel_bytes(const SlicedChannel& c) {
    size_t total = 0;
    for (const auto& layer : c.layers)
        for (const auto& mip : layer) total += mip.size();
    return total;
}

}  // namespace tileset
