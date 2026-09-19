#include "projected_face_cache.h"
#include "part_asset.h"
#include "check.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
using namespace gpu_meshing;
namespace {
struct Fixture {
    std::filesystem::path dir;
    Fixture() {
        dir = std::filesystem::temp_directory_path() /
            ("matter-face-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        if (!std::filesystem::create_directory(dir)) throw std::runtime_error("fixture unavailable");
    }
    ~Fixture() { std::error_code ec; std::filesystem::remove_all(dir, ec); }
};
std::vector<char> read(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
void write(const std::string &path, const std::vector<char> &bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), std::streamsize(bytes.size()));
}
bool same(const FacePatch &a, const FacePatch &b) {
    if (a.recipe_digest != b.recipe_digest || a.texels.size() != b.texels.size()) return false;
    for (size_t i = 0; i < a.texels.size(); ++i) {
        auto &x = a.texels[i]; auto &y = b.texels[i];
        if (x.height_m != y.height_m || x.coverage != y.coverage ||
            x.normal_uvn.x != y.normal_uvn.x || x.normal_uvn.y != y.normal_uvn.y ||
            x.normal_uvn.z != y.normal_uvn.z) return false;
    }
    return true;
}
void tests() {
    Fixture fixture;
    const auto path = (fixture.dir / "source.pfac").string();
    SolidOp op;
    op.kind[0] = 2; op.shape = {.05f, 0, 0, 0};
    FaceJob j;
    j.source.ops = &op; j.source.op_count = 1; j.source.voxel_m = .005f;
    j.source_identity = 714;
    j.u_min_m = j.v_min_m = -.07f; j.u_max_m = j.v_max_m = .07f;
    j.height_min_m = -.06f; j.height_max_m = .06f; j.pixel_m = .002f;
    FacePatch cold, warm; FaceCacheStats stats; Error e;
    unsigned calls = 0;
    const SolidFaceProjector project = [&](const FaceJob &job, FacePatch &p, FaceStats &s,
                                           Error &error, const BuildControl &control) {
        ++calls;
        return prepare_solid_face(job, project_solid_face_region_reference, p, s, error, control);
    };
    CHECK(load_or_project_face(path, j, project, cold, stats, e), e.message.c_str());
    CHECK(!stats.hit && calls == 1, "cold face prepares exactly once");
    CHECK(load_or_project_face(path, j, {}, warm, stats, e), e.message.c_str());
    CHECK(stats.hit && calls == 1 && stats.projection.submissions == 0,
          "warm face requires no projector or GPU submission");
    CHECK(same(cold, warm), "lossless geometry round trip");
    CHECK(warm.layout.width == cold.layout.width && warm.layout.pitch_u_m == cold.layout.pitch_u_m &&
          warm.frame.n.z == cold.frame.n.z && warm.u_min_m == cold.u_min_m &&
          warm.layout.source_bounds.max_m.x == cold.layout.source_bounds.max_m.x,
          "physical frame and sample lattice survive cache");
    const auto original = read(path);
    CHECK(original.size() == 160 + cold.texels.size() * 20, "bounded packed cache size");
    for (int damage = 0; damage < 6; ++damage) {
        auto bytes = original;
        if (damage == 0) bytes.resize(12);
        if (damage == 1) bytes.push_back(0);
        if (damage == 2) bytes[4] ^= 1; // format version
        if (damage == 3) bytes[140] ^= 1; // source recipe
        if (damage == 4) bytes.back() ^= 1; // payload checksum
        if (damage == 5) {
            // Valid checksum cannot authorize semantically invalid coverage.
            bytes[160 + 16] = 2;
            std::fill(bytes.begin() + 152, bytes.begin() + 160, 0);
            const auto sum = part_asset::fnv1a64(bytes.data(), bytes.size());
            for (unsigned b = 0; b < 8; ++b) bytes[152 + b] = char(sum >> (8 * b));
        }
        write(path, bytes);
        CHECK(!load_projected_face(path, j, warm, e) && e.code == ErrorCode::ArtifactFailure,
              "truncated, foreign, corrupt and invalid cache fail closed");
        CHECK(same(warm, cold), "invalid cache preserves complete output");
        const auto before = calls;
        CHECK(load_or_project_face(path, j, project, warm, stats, e), e.message.c_str());
        CHECK(calls == before + 1 && !stats.hit && same(cold, warm), "bad cache repaired once");
        CHECK(load_or_project_face(path, j, {}, warm, stats, e) && stats.hit, "repair becomes warm");
    }
    auto changed = j; changed.frame.u = {-1, 0, 0}; changed.frame.n = {0, 0, -1};
    CHECK(!load_projected_face(path, changed, warm, e), "other face cannot reuse front recipe");
    changed = j; ++changed.source_identity;
    CHECK(!load_projected_face(path, changed, warm, e), "source edit invalidates geometry cache");
    for (int damage = 0; damage < 4; ++damage) {
        auto invalid = cold;
        if (damage == 0) invalid.texels.pop_back();
        if (damage == 1) ++invalid.recipe_digest;
        if (damage == 2) invalid.texels[0].height_m = std::numeric_limits<float>::quiet_NaN();
        if (damage == 3) invalid.texels[0].coverage = 2;
        CHECK(!save_projected_face(path, j, invalid, e), "invalid output never published");
        CHECK(read(path) == original, "failed write preserves previous artifact");
    }
    CHECK(!load_or_project_face(path, j, project, warm, stats, e, {[] { return true; }, {}}) &&
          e.code == ErrorCode::Cancelled && same(cold, warm), "cancelled warm request preserves output");
    CHECK(!save_projected_face(path, j, cold, e, {{}, [](uint64_t) { return false; }}) &&
          e.code == ErrorCode::StaleGeneration && read(path) == original, "stale write cannot replace cache");
    unsigned checks = 0;
    CHECK(!save_projected_face(path, j, cold, e, {[&] { return ++checks == 3; }, {}}) &&
          e.code == ErrorCode::Cancelled && read(path) == original, "cancellation during encoding is atomic");
    checks = 0;
    CHECK(!load_projected_face(path, j, warm, e, {[&] { return ++checks == 3; }, {}}) &&
          e.code == ErrorCode::Cancelled && same(cold, warm), "cancellation during decoding is atomic");
    const auto missing = (fixture.dir / "absent.pfac").string();
    CHECK(!load_or_project_face(missing, j, {}, warm, stats, e) &&
          e.code == ErrorCode::Unavailable && same(cold, warm), "cold request requires a backend");
    const SolidFaceProjector failing = [](const FaceJob &, FacePatch &p, FaceStats &, Error &error,
                                         const BuildControl &) {
        p = {}; error = {ErrorCode::VulkanFailure, "injected"}; return false;
    };
    CHECK(!load_or_project_face(missing, j, failing, warm, stats, e) && same(cold, warm) &&
          !std::filesystem::exists(missing), "failed preparation publishes no face or artifact");
    CHECK(std::distance(std::filesystem::directory_iterator(fixture.dir),
                        std::filesystem::directory_iterator{}) == 1, "no staging artifacts left behind");
}
} // namespace
int main() {
    try { tests(); }
    catch (const std::exception &e) { std::printf("FAIL: %s\n", e.what()); return 1; }
    return check_summary();
}
