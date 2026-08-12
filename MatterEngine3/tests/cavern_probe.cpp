// MatterEngine3/tests/cavern_probe.cpp — measure the shape of a volumetric world.
//
// NOT a test. A measuring instrument, and it exists because "the caverns are too
// small to fly through" is a question about metres and the only two ways to
// answer it are to fly the editor or to read the density field directly. This
// reads the field.
//
// Usage:
//   ./build/cavern_probe <scene.js> [yTop] [yBottom]
//
// What it reports, and why each number is the one that matters:
//
//   air fraction        what proportion of the sampled slab is open. Rises fast
//                       when you widen a network and is the first thing to
//                       overshoot -- past ~15% a "cavern world" is a lattice of
//                       rock splinters, not a cave.
//
//   clearance histogram for each air sample, the distance to the nearest solid
//                       along the six axes, taken as min. That is a LOWER BOUND
//                       on the inscribed sphere -- a passage whose min-axis
//                       clearance is 12 m admits a camera that needs 12 m, and
//                       may well be wider. Under-reporting is the right error
//                       to make for "can I fly through it".
//
//   reachable depth     a 6-connected flood fill through air, seeded from every
//                       air cell in the top row of the sample grid. This is what
//                       separates "there is air down there" from "you can GET
//                       down there", and they are not the same question -- the
//                       scene comment records a previous measurement where the
//                       network kept producing passages to the floor that no
//                       connected path reached.
//
//   FLYABLE depth       the same flood fill, restricted to cells whose clearance
//                       is at least the camera radius. THIS IS THE HEADLINE, and
//                       it is a different number from reachable depth for the
//                       reason the histogram shows: a network can be connected
//                       to the floor entirely through gaps too tight to fly. It
//                       answers the actual question -- can a ball of radius R
//                       travel from open sky to the bottom -- and no summary
//                       statistic over widths can, because a mean is carried by
//                       chambers and the thing that stops you is the pinch.
//
// The flood fill is on the sample lattice, so its connectivity is the lattice's,
// not the field's: two chambers joined by a passage narrower than the step read
// as disconnected. Step size is therefore a claim about what counts as passable,
// and 8 m is deliberately near the width a camera needs.

#include "../src/script_host.h"
#include "../src/terrain_field.h"
#include "../src/script/world_definition_loader.h"
#include "material_registry.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace terrain_field;

namespace {

std::string read_file(const char* path, bool& ok) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { ok = false; return {}; }
    std::ostringstream text;
    text << in.rdbuf();
    ok = true;
    return text.str();
}

// March from an air sample along one axis until the field turns solid, or until
// `limit` metres have passed. Returns the distance travelled -- `limit` meaning
// "still open at the limit", which the histogram lumps into its top bucket.
float clearance_along(const FieldRuntime& f, float x, float y, float z,
                      float dx, float dy, float dz, float step, float limit) {
    for (float d = step; d <= limit; d += step) {
        if (f.density_at(x + dx * d, y + dy * d, z + dz * d) > 0.0f)
            return d - step;
    }
    return limit;
}

}  // namespace

int main(int argc, char** argv) {
    const char* scene = argc > 1
        ? argv[1]
        : "../../projects/world_demo/scenes/StreamCaverns/StreamCaverns.js";
    bool ok = false;
    const std::string source = read_file(scene, ok);
    if (!ok) {
        std::fprintf(stderr, "cavern_probe: cannot read %s\n", scene);
        return 1;
    }

    // load_world_definition first, and not for its result: a scene declares its
    // materials with defineMaterial and only the LOADER assigns their handles.
    // Skip it and eval_world fails with "not registered" -- the same trap
    // sector_scatter_profile.cpp documents.
    {
        matter::WorldLoadDesc desc;
        desc.world_path = scene;
        desc.objects_dir = "../../projects/world_demo/objects";
        desc.project_shared_lib_dir = "../../projects/world_demo/shared-lib";
        desc.engine_shared_lib_dir = "../shared-lib";
        matter::WorldDefinition definition;
        matter::WorldLoadError lerr;
        if (!matter::load_world_definition(desc, definition, lerr)) {
            std::fprintf(stderr, "cavern_probe: load_world_definition: %s\n",
                         lerr.message.c_str());
            return 1;
        }
    }

    script_host::ScriptHost host;
    host.set_shared_lib_roots(
        {"../../projects/world_demo/shared-lib", "../shared-lib"});
    script_host::WorldEvalResult r = host.eval_world(source, "{}");
    if (!r.ok) {
        std::fprintf(stderr, "cavern_probe: eval_world failed: %s\n",
                     r.message.c_str());
        return 1;
    }
    FieldProgram program;
    std::string err;
    if (!FieldProgram::parse(r.field_program, program, err)) {
        std::fprintf(stderr, "cavern_probe: field program parse failed: %s\n",
                     err.c_str());
        return 1;
    }
    FieldRuntime field(std::move(program));

    // The sample box. X/Z span is a few tunnel wavelengths so the statistics are
    // over the network rather than over one chamber; Y defaults to the world's
    // authored slab, which is what the mesher walks.
    const float y_top    = argc > 2 ? float(std::atof(argv[2])) : r.y_max;
    const float y_bottom = argc > 3 ? float(std::atof(argv[3])) : r.y_min;
    const float extent   = 1024.0f;   // half-width in X and Z about the origin
    const float step     = 8.0f;      // lattice pitch, and the passability claim

    const int nx = int(2.0f * extent / step);
    const int nz = nx;
    const int ny = int((y_top - y_bottom) / step);
    if (nx <= 0 || ny <= 0) {
        std::fprintf(stderr, "cavern_probe: empty sample box\n");
        return 1;
    }

    std::printf("scene            %s\n", scene);
    std::printf("sector size      %.1f m   slab [%.1f, %.1f]\n",
                r.sector_size, r.y_min, r.y_max);
    std::printf("sample box       %.0f m cube in XZ, y in [%.0f, %.0f], "
                "%d x %d x %d at %.0f m\n",
                2.0f * extent, y_bottom, y_top, nx, ny, nz, step);

    // ---- occupancy + flood fill ------------------------------------------
    // One byte per cell: 0 solid, 1 air, 2 air-and-reached. 128^3 at these
    // defaults, so a flat vector is fine and a sparse structure would only
    // obscure the indexing.
    std::vector<unsigned char> cell(size_t(nx) * ny * nz, 0);
    const auto at = [nx, ny, nz](int ix, int iy, int iz) -> size_t {
        (void)ny;
        return (size_t(iy) * nz + iz) * nx + ix;
    };
    const auto pos = [&](int ix, int iy, int iz, float& x, float& y, float& z) {
        x = -extent + (float(ix) + 0.5f) * step;
        z = -extent + (float(iz) + 0.5f) * step;
        // iy 0 is the TOP row, so the flood seed is "the sky" without a search.
        y = y_top - (float(iy) + 0.5f) * step;
    };

    size_t air = 0;
    for (int iy = 0; iy < ny; ++iy)
        for (int iz = 0; iz < nz; ++iz)
            for (int ix = 0; ix < nx; ++ix) {
                float x, y, z;
                pos(ix, iy, iz, x, y, z);
                if (field.density_at(x, y, z) <= 0.0f) {
                    cell[at(ix, iy, iz)] = 1;
                    ++air;
                }
            }
    const size_t total = size_t(nx) * ny * nz;
    std::printf("air fraction     %.2f%%  (%zu of %zu cells)\n",
                100.0 * double(air) / double(total), air, total);

    std::deque<int> queue;   // packed ix + nx*(iz + nz*iy)
    const auto push = [&](int ix, int iy, int iz) {
        if (ix < 0 || iy < 0 || iz < 0 || ix >= nx || iy >= ny || iz >= nz)
            return;
        unsigned char& c = cell[at(ix, iy, iz)];
        if (c != 1) return;
        c = 2;
        queue.push_back(ix + nx * (iz + nz * iy));
    };
    for (int iz = 0; iz < nz; ++iz)
        for (int ix = 0; ix < nx; ++ix) push(ix, 0, iz);
    int deepest = 0;
    while (!queue.empty()) {
        const int packed = queue.front();
        queue.pop_front();
        const int ix = packed % nx;
        const int iz = (packed / nx) % nz;
        const int iy = packed / (nx * nz);
        if (iy > deepest) deepest = iy;
        push(ix + 1, iy, iz); push(ix - 1, iy, iz);
        push(ix, iy + 1, iz); push(ix, iy - 1, iz);
        push(ix, iy, iz + 1); push(ix, iy, iz - 1);
    }
    std::printf("reachable depth  y = %.0f m  (%d rows of %d below the top)\n",
                y_top - (float(deepest) + 0.5f) * step, deepest, ny);

    // ---- clearance --------------------------------------------------------
    // Measured only on REACHED air: clearance inside a sealed pocket is not a
    // number about flying anywhere. Stored per cell (rounded to metres, capped
    // at 255) because the flyable flood below needs it as a passability test,
    // not just as a histogram.
    const float limit = 96.0f;
    std::vector<unsigned char> clear(cell.size(), 0);
    static const int kBuckets = 8;
    const float edges[kBuckets] = {4, 8, 12, 16, 24, 32, 48, 96};
    size_t hist[kBuckets] = {0};
    size_t measured = 0;
    double sum = 0.0;
    float best = 0.0f;
    float best_at[3] = {0, 0, 0};
    for (int iy = 0; iy < ny; ++iy)
        for (int iz = 0; iz < nz; ++iz)
            for (int ix = 0; ix < nx; ++ix) {
                if (cell[at(ix, iy, iz)] != 2) continue;
                float x, y, z;
                pos(ix, iy, iz, x, y, z);
                // Open sky: a sample above the terrain has unbounded clearance
                // upward and would swamp the histogram with a number that has
                // nothing to do with caves. It is still passable for the flood
                // below -- flying over the plateau is not the hard part -- so
                // it takes the cap rather than a measurement.
                if (y > field.height_at(x, z)) {
                    clear[at(ix, iy, iz)] = 255;
                    continue;
                }
                float c = limit;
                c = std::fmin(c, clearance_along(field, x, y, z,  1, 0, 0, 2.0f, limit));
                c = std::fmin(c, clearance_along(field, x, y, z, -1, 0, 0, 2.0f, limit));
                c = std::fmin(c, clearance_along(field, x, y, z, 0,  1, 0, 2.0f, limit));
                c = std::fmin(c, clearance_along(field, x, y, z, 0, -1, 0, 2.0f, limit));
                c = std::fmin(c, clearance_along(field, x, y, z, 0, 0,  1, 2.0f, limit));
                c = std::fmin(c, clearance_along(field, x, y, z, 0, 0, -1, 2.0f, limit));
                clear[at(ix, iy, iz)] = (unsigned char)std::fmin(255.0f, c);
                ++measured;
                sum += c;
                if (c > best) { best = c; best_at[0] = x; best_at[1] = y; best_at[2] = z; }
                for (int b = 0; b < kBuckets; ++b)
                    if (c <= edges[b]) { ++hist[b]; break; }
            }
    if (measured == 0) {
        std::printf("clearance        no reachable subsurface air at all\n");
        return 0;
    }
    std::printf("clearance        %zu reachable subsurface samples, mean %.1f m, "
                "max %.0f m at (%.0f, %.0f, %.0f)\n",
                measured, sum / double(measured), best,
                best_at[0], best_at[1], best_at[2]);
    float low = 0.0f;
    size_t cumulative = 0;
    for (int b = 0; b < kBuckets; ++b) {
        cumulative += hist[b];
        std::printf("  %5.0f - %5.0f m  %6zu  %5.1f%%   (cum %5.1f%%)\n",
                    low, edges[b], hist[b],
                    100.0 * double(hist[b]) / double(measured),
                    100.0 * double(cumulative) / double(measured));
        low = edges[b];
    }
    size_t wide = 0;
    for (int b = 0; b < kBuckets; ++b)
        if (edges[b] > 12.0f) wide += hist[b];
    std::printf("wide (>12 m)     %.1f%% of the reachable network by volume\n",
                100.0 * double(wide) / double(measured));

    // ---- the flyable flood ------------------------------------------------
    // Everything above is about how much room there is on average. This is
    // about whether there is a PATH: re-flood from the sky, admitting only
    // cells that clear the camera radius. A network that scores well on width
    // and fails here is one with fat chambers joined by cracks.
    for (float radius : {8.0f, 12.0f, 16.0f}) {
        std::vector<unsigned char> fat(cell.size(), 0);
        std::deque<int> fq;
        const auto fpush = [&](int ix, int iy, int iz) {
            if (ix < 0 || iy < 0 || iz < 0 || ix >= nx || iy >= ny || iz >= nz)
                return;
            const size_t i = at(ix, iy, iz);
            if (fat[i] || cell[i] != 2 || float(clear[i]) < radius) return;
            fat[i] = 1;
            fq.push_back(ix + nx * (iz + nz * iy));
        };
        for (int iz = 0; iz < nz; ++iz)
            for (int ix = 0; ix < nx; ++ix) fpush(ix, 0, iz);
        int fat_deepest = 0;
        size_t fat_cells = 0;
        while (!fq.empty()) {
            const int packed = fq.front();
            fq.pop_front();
            ++fat_cells;
            const int ix = packed % nx;
            const int iz = (packed / nx) % nz;
            const int iy = packed / (nx * nz);
            if (iy > fat_deepest) fat_deepest = iy;
            fpush(ix + 1, iy, iz); fpush(ix - 1, iy, iz);
            fpush(ix, iy + 1, iz); fpush(ix, iy - 1, iz);
            fpush(ix, iy, iz + 1); fpush(ix, iy, iz - 1);
        }
        std::printf("flyable r=%2.0f m   reaches y = %6.0f m   (%zu cells, "
                    "%.1f%% of the reachable network)\n",
                    radius, y_top - (float(fat_deepest) + 0.5f) * step,
                    fat_cells, 100.0 * double(fat_cells) / double(air));
    }
    return 0;
}
