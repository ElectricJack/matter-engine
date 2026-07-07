// tileset_bake_gpu.cpp — orchestrates the .gtex bake.

#include "tileset_bake_gpu.h"
#include "tileset_bake.h"          // SettledTorus, BakeInputs, SettledInstance
#include "tileset_spec.h"          // TileConfig
#include "tileset_layout.h"        // kTorusN
#include "tileset_gtex.h"          // save_gtex, load_gtex, gtex_cache_hit, gtex_content_hash, GTexHeader
#include "tileset_torus_bvh.h"     // assemble_torus_bvh
#include "tileset_bake_primary.h"  // bake_primary
#include "tileset_bake_ao.h"       // bake_ao (material-table overload)
#include "tileset_gl_ctx.h"        // tileset_gl_init, compile_compute_program, load_compute_source

#include "blas_manager.hpp"
#include "tlas_manager.hpp"
#include "material_registry.h"

// stb_image_write.h is included here for stbi_write_png. The single-TU
// implementation (STB_IMAGE_WRITE_IMPLEMENTATION) lives in tileset_gtex.cpp.
// We only need the declarations here.
#include "external/stb_image_write.h"

extern "C" { }  // material_registry.h is already extern "C" guarded

#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>       // std::bad_alloc
#include <string>
#include <vector>

namespace tileset {

// -----------------------------------------------------------------------------
// Height range from the base field + a coarse instance-y estimate.
// -----------------------------------------------------------------------------
// compute_height_range also returns the instance top envelope for ray_y.
// max_instance_top is the highest point any instance can reach: pose_y + 2*scale
// (generous bounding-sphere assumption). The caller floors ray_y at hmax+2.
static void compute_height_range(const SettledTorus& st,
                                  float& hmin, float& hmax,
                                  float& max_instance_top) {
    hmin = 1e30f; hmax = -1e30f;
    max_instance_top = -1e30f;
    for (float y : st.base.heights) {
        if (y < hmin) hmin = y;
        if (y > hmax) hmax = y;
    }
    for (const SettledInstance& si : st.instances) {
        float y  = si.pose.py;
        // Coarse envelope: ±0.5 * scale metres around the instance origin.
        float lo = y - 0.5f * si.scale;
        float hi = y + 0.5f * si.scale;
        if (lo < hmin) hmin = lo;
        if (hi > hmax) hmax = hi;
        // Conservative instance top: pose_y + 2*scale (bounding-sphere radius
        // is at most scale, plus a 1*scale safety margin).
        float top = y + 2.0f * si.scale;
        if (top > max_instance_top) max_instance_top = top;
    }
    if (hmin > hmax) { hmin = 0.0f; hmax = 1.0f; }
    if (max_instance_top < hmax) max_instance_top = hmax;
    // Pad so R16 quantization has headroom.
    hmax += 0.05f;
    hmin -= 0.05f;
}

// -----------------------------------------------------------------------------
// Merge ORM: overwrite the .r channel (AO) with the ao_r8 buffer.
// -----------------------------------------------------------------------------
static void pack_orm_ao(std::vector<uint8_t>& orm_rgb8, const std::vector<uint8_t>& ao_r8) {
    const size_t n = ao_r8.size();
    for (size_t i = 0; i < n; ++i) orm_rgb8[i * 3 + 0] = ao_r8[i];
}

// -----------------------------------------------------------------------------
// Loose PNG dumps.
// -----------------------------------------------------------------------------
static bool dump_pngs(const std::string& base,
                      int W, int H,
                      const std::vector<uint8_t>&  a,
                      const std::vector<uint8_t>&  n,
                      const std::vector<uint8_t>&  o,
                      const std::vector<uint16_t>& h)
{
    std::string p;
    p = base + "-albedo.png";
    if (!stbi_write_png(p.c_str(), W, H, 3, a.data(), W * 3)) return false;
    p = base + "-normal.png";
    if (!stbi_write_png(p.c_str(), W, H, 2, n.data(), W * 2)) return false;
    p = base + "-orm.png";
    if (!stbi_write_png(p.c_str(), W, H, 3, o.data(), W * 3)) return false;
    // Height as 8-bit approximation (top byte of R16) for eyeballing only.
    std::vector<uint8_t> h8(h.size());
    for (size_t i = 0; i < h.size(); ++i) h8[i] = (uint8_t)(h[i] >> 8);
    p = base + "-height.png";
    if (!stbi_write_png(p.c_str(), W, H, 1, h8.data(), W)) return false;
    return true;
}

bool bake_tileset_gpu(const SettledTorus& settled,
                      uint64_t script_source_hash,
                      const std::string& out_gtex_path,
                      const BakeInputs& inputs,
                      bool force_rebake,
                      bool dump_png,
                      std::string& err)
{
    try {
        // ------------------------------------------------------------------
        // 1. Cache-hit check.
        // ------------------------------------------------------------------
        const uint64_t expected = gtex_content_hash(
            settled.report.pose_hash, script_source_hash,
            kEngineBakeVersion, kBox3dVersion);

        if (!force_rebake && gtex_cache_hit(out_gtex_path, expected)) {
            return true;
        }

        // ------------------------------------------------------------------
        // 2. GL prerequisites (caller owns the window).
        // ------------------------------------------------------------------
        if (!tileset_gl_init(err)) return false;

        // ------------------------------------------------------------------
        // 3. Assemble BVH (CPU).
        // ------------------------------------------------------------------
        BLASManager blas;
        // Size the TLAS to accommodate the settled instance count plus the base
        // heightfield instance, with 25% headroom for any drop/instance the
        // orchestrator hasn't accounted for.  A too-small cap silently drops
        // instances (producing an atlas where only strip-boundary content is
        // present because interior placements arrived last and got refused).
        const int n_settled = (int)settled.instances.size();
        const int tlas_cap  = (n_settled + 1) * 5 / 4 + 32;
        TLASManager tlas(tlas_cap);
        if (!assemble_torus_bvh(settled, inputs, blas, tlas, err)) return false;

        // ------------------------------------------------------------------
        // 4. Load + compile shaders.
        // ------------------------------------------------------------------
        std::string src_p, src_ao;
        if (!load_compute_source("shaders_gpu/tileset_bake_primary.comp",
                                  "shaders", src_p, err)) return false;
        if (!load_compute_source("shaders_gpu/tileset_bake_ao.comp",
                                  "shaders", src_ao, err)) return false;
        GLuint prog_p  = compile_compute_program(src_p,  err);
        if (!prog_p) return false;
        GLuint prog_ao = compile_compute_program(src_ao, err);
        if (!prog_ao) { glDeleteProgram(prog_p); return false; }

        // ------------------------------------------------------------------
        // 5. Material table snapshot.
        //    The base heightfield carries real per-vertex normals now
        //    (build_base_blas central-difference sampling), so no per-material
        //    flatShading patch is required — both primary and AO passes read
        //    the canonical MaterialRegistry as-is.
        // ------------------------------------------------------------------
        const int n_mat = MaterialRegistryCount();
        std::vector<MaterialDef> mats((size_t)n_mat);
        for (int i = 0; i < n_mat; ++i) mats[i] = *MaterialRegistryGet(i);

        // ------------------------------------------------------------------
        // 6. Height range + ray origin.
        //    ray_y is floored at hmax+2 so it always clears the quantization-
        //    padded top, then raised further if any instance top extends above
        //    that floor (e.g. scale=5 → top≈10m, hmax+2 would miss it).
        // ------------------------------------------------------------------
        float hmin = 0.0f, hmax = 1.0f, max_instance_top = 1.0f;
        compute_height_range(settled, hmin, hmax, max_instance_top);
        const float ray_y_floor    = hmax + 2.0f;
        const float ray_y_envelope = max_instance_top + 0.5f;
        const float ray_y          = ray_y_floor > ray_y_envelope
                                         ? ray_y_floor : ray_y_envelope;

        // ------------------------------------------------------------------
        // 7. Primary bake pass.
        // ------------------------------------------------------------------
        std::vector<uint8_t>  albedo, normal_rg, orm;
        std::vector<uint16_t> height;
        if (!bake_primary(prog_p, blas, tlas, mats, settled.cfg,
                          ray_y, hmin, hmax,
                          albedo, normal_rg, orm, height, err))
        {
            glDeleteProgram(prog_p); glDeleteProgram(prog_ao);
            return false;
        }

        // ------------------------------------------------------------------
        // 8. AO bake pass (real material table).
        // ------------------------------------------------------------------
        std::vector<uint8_t> ao;
        if (!bake_ao(prog_ao, blas, tlas, mats, settled.cfg,
                     ray_y, hmin, hmax,
                     (uint32_t)settled.cfg.seed,
                     ao, err))
        {
            glDeleteProgram(prog_p); glDeleteProgram(prog_ao);
            return false;
        }

        // ------------------------------------------------------------------
        // 9. Pack ORM: R = AO, G = roughness (primary), B = metallic (primary).
        // ------------------------------------------------------------------
        pack_orm_ao(orm, ao);

        // ------------------------------------------------------------------
        // 10. Write .gtex.
        // ------------------------------------------------------------------
        const int W = kTorusN * (int)settled.cfg.size * settled.cfg.texels_per_meter;
        const int H = W;

        GTexHeader hdr{};
        hdr.tile_size_m         = settled.cfg.size;
        hdr.texels_per_meter    = settled.cfg.texels_per_meter;
        hdr.atlas_tiles_x       = kTorusN;
        hdr.atlas_tiles_y       = kTorusN;
        hdr.height_min          = hmin;
        hdr.height_max          = hmax;
        hdr.content_hash        = expected;
        hdr.box3d_version       = kBox3dVersion;
        hdr.engine_bake_version = kEngineBakeVersion;

        if (!save_gtex(out_gtex_path, hdr, W, H,
                       albedo.data(), normal_rg.data(), orm.data(), height.data(), err))
        {
            glDeleteProgram(prog_p); glDeleteProgram(prog_ao);
            return false;
        }

        // ------------------------------------------------------------------
        // 11. Optional PNG dump — strip ".gtex" for the loose base name.
        // ------------------------------------------------------------------
        if (dump_png) {
            std::string base = out_gtex_path;
            if (base.size() >= 5 && base.compare(base.size() - 5, 5, ".gtex") == 0)
                base.resize(base.size() - 5);
            if (!dump_pngs(base, W, H, albedo, normal_rg, orm, height)) {
                err = "bake_tileset_gpu: --dump-png emit failed near " + base;
                glDeleteProgram(prog_p); glDeleteProgram(prog_ao);
                return false;
            }
        }

        glDeleteProgram(prog_p);
        glDeleteProgram(prog_ao);
        return true;

    } catch (const std::bad_alloc&) {
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "OOM in bake_tileset_gpu (pose_hash=%016llx)",
                      (unsigned long long)settled.report.pose_hash);
        err = buf;
        return false;
    }
}

} // namespace tileset
