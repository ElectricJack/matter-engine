// Diagnostic microbenchmark of existing production CPU texture stages.
// No Vulkan, no cache writes, and no changes to the algorithms under review.
// Build/run instructions and limitations are in the companion review.
#include "tileset_gtex.h"
#include "render/tileset_slicer.h"
#include "render/bc_encode.h"
#include "render/tileset_bake_vk.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>

using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
static void check(bool ok, const std::string& error) {
    if (!ok) { std::fprintf(stderr, "%s\n", error.c_str()); std::exit(1); }
}
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    for (int asset = 1; asset < argc; ++asset) {
        for (int run = 1; run <= 3; ++run) {
            std::string err;
            tileset::GTexHeader hdr;
            std::vector<uint8_t> a, n, o, ha, hb;
            std::vector<uint16_t> h;
            auto begin = Clock::now();
            check(tileset::load_gtex(argv[asset], hdr, a, n, o, h, ha, hb, err), err);
            const double decode_ms = ms(begin);
            const int dim = int(std::sqrt(double(h.size())));
            tileset::SlicedChannel slices[6];
            const uint8_t* data[6] = {a.data(), n.data(), o.data(),
                reinterpret_cast<const uint8_t*>(h.data()), ha.data(), hb.data()};
            const int bpp[6] = {3, 2, 3, 2, 4, 4};
            begin = Clock::now();
            for (int c = 0; c < 6; ++c) {
                if (c >= 4 && ha.empty()) continue;
                const int d = c < 4 ? dim : int(hdr.horizon_w_px);
                check(tileset::slice_channel(data[c], d, d, bpp[c],
                    c == 0 || c == 2, c == 3, slices[c], err), err);
            }
            const double slice_ms = ms(begin);
            const tileset::BcFormat formats[4] = {tileset::BcFormat::kBc7,
                tileset::BcFormat::kBc5, tileset::BcFormat::kBc7, tileset::BcFormat::kBc4};
            tileset::CompressedChannel compressed[4];
            double encode_ms[4];
            size_t bytes = 0;
            for (int c = 0; c < 4; ++c) {
                begin = Clock::now();
                check(tileset::compress_sliced_channel(slices[c], formats[c],
                    c == 3, compressed[c], err), err);
                encode_ms[c] = ms(begin);
                bytes += tileset::compressed_channel_bytes(compressed[c]);
                slices[c] = {};
            }
            for (int c = 4; c < 6; ++c) bytes += tileset::sliced_channel_bytes(slices[c]);
            std::printf("asset=%s run=%d atlas=%d tile_m=%.4f tpm=%u content=%016llx decode_ms=%.3f slice_mips_ms=%.3f bc7_albedo_ms=%.3f bc5_normal_ms=%.3f bc7_orm_ms=%.3f bc4_height_ms=%.3f cpu_load_ms=%.3f upload_bytes=%zu\n",
                argv[asset], run, dim, hdr.tile_size_m, hdr.texels_per_meter,
                static_cast<unsigned long long>(hdr.content_hash), decode_ms, slice_ms,
                encode_ms[0], encode_ms[1], encode_ms[2], encode_ms[3],
                decode_ms + slice_ms + encode_ms[0] + encode_ms[1] + encode_ms[2] + encode_ms[3], bytes);
            std::fflush(stdout);
            if (run == 3 && !ha.empty()) {
                std::vector<uint8_t> out_a, out_b;
                int qw, qh;
                begin = Clock::now();
                tileset::gtex_bake_horizon_cpu(h, dim, dim, hdr.texels_per_meter,
                    hdr.height_min, hdr.height_max, out_a, out_b, qw, qh);
                const double horizon_ms = ms(begin);
                size_t nonzero = 0;
                for (auto v : out_a) nonzero += v != 0;
                for (auto v : out_b) nonzero += v != 0;
                std::printf("asset=%s horizon_ms=%.3f horizon_samples=%llu nonzero_bytes=%zu\n",
                    argv[asset], horizon_ms, static_cast<unsigned long long>(qw) * qh * 8 * 24, nonzero);
                std::fflush(stdout);
            }
        }
    }
}
