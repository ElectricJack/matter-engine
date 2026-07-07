// tileset_provider_tests.cpp — headless GL tests for the viewer tileset slot table.
// Pattern: mirror tileset_gpu_tests.cpp — FLAG_WINDOW_HIDDEN, gl46_available SKIP,
// CloseWindow before return. Test file drives a fixture .gtex through save_gtex →
// load_slot → assertions on GL texture state → unload → clean exit.

#include "raylib.h"
#include "gl46.h"
#include "tileset_gtex.h"
#include "tileset_provider.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int g_tests = 0, g_failures = 0;
#define REQUIRE(cond) do { \
    ++g_tests; \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
    } while (0)

// Build a tiny 64x64 (4 tiles x 16px each) .gtex fixture in memory, write to /tmp,
// then load_slot(0). Verify the four GL textures exist, have mip 0 dims equal to
// atlas_tiles*tile_size_m*texels_per_meter, and that mip generation produced >1 level.
static void test_load_slot_uploads_mipped_textures() {
    using namespace tileset;
    GTexHeader hdr;
    hdr.tile_size_m       = 1.0f;
    hdr.texels_per_meter  = 4;      // 4 * 1 * 4 = 16 px per tile
    hdr.atlas_tiles_x     = 4;
    hdr.atlas_tiles_y     = 4;
    hdr.height_min        = 0.0f;
    hdr.height_max        = 0.5f;
    hdr.content_hash      = 0xDEADBEEFCAFEBABEull;

    const int W = hdr.atlas_tiles_x * (int)hdr.tile_size_m * hdr.texels_per_meter; // 16
    const int H = hdr.atlas_tiles_y * (int)hdr.tile_size_m * hdr.texels_per_meter; // 16
    std::vector<uint8_t>  albedo(W*H*3, 128);
    std::vector<uint8_t>  normal(W*H*2, 127);
    std::vector<uint8_t>  orm   (W*H*3, 200);
    std::vector<uint16_t> height(W*H,   30000);

    const std::string path = "/tmp/tileset_provider_fixture.gtex";
    std::string err;
    REQUIRE(save_gtex(path, hdr, W, H,
                      albedo.data(), normal.data(), orm.data(), height.data(), err));

    REQUIRE(viewer::tileset_provider::load_slot(0, path, err));
    if (!err.empty()) std::fprintf(stderr, "  load_slot err: %s\n", err.c_str());

    const viewer::TilesetSlot& s = viewer::tileset_provider::get_slot(0);
    REQUIRE(s.valid);
    REQUIRE(s.tex_albedo != 0);
    REQUIRE(s.tex_normal != 0);
    REQUIRE(s.tex_orm    != 0);
    REQUIRE(s.tex_height != 0);
    REQUIRE(s.tile_size_m == 1.0f);
    REQUIRE(s.texels_per_meter == 4);
    REQUIRE(s.atlas_tiles_x == 4);
    REQUIRE(s.height_min == 0.0f);
    REQUIRE(s.height_max == 0.5f);

    // Verify mip level 0 width matches and level 1 exists (glGenerateMipmap called).
    glBindTexture(GL_TEXTURE_2D, s.tex_albedo);
    GLint mip0_w = 0, mip1_w = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &mip0_w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 1, GL_TEXTURE_WIDTH, &mip1_w);
    REQUIRE(mip0_w == W);
    REQUIRE(mip1_w == W/2);

    viewer::tileset_provider::unload_slot(0);
    const viewer::TilesetSlot& s_after = viewer::tileset_provider::get_slot(0);
    REQUIRE(!s_after.valid);
    REQUIRE(s_after.tex_albedo == 0);

    std::remove(path.c_str());
}

// Verify bind_all_to_shader sets sampler uniforms for a program that declares them.
static void test_bind_all_sets_uniforms() {
    const char* fs_src =
        "#version 460 core\n"
        "uniform sampler2D groundAlbedo0;\n"
        "uniform sampler2D groundNormal0;\n"
        "uniform sampler2D groundORM0;\n"
        "uniform sampler2D groundHeight0;\n"
        "out vec4 c;\n"
        "void main(){ c = texture(groundAlbedo0, vec2(0)) + texture(groundNormal0,vec2(0))\n"
        "                  + texture(groundORM0,   vec2(0)) + texture(groundHeight0,vec2(0)); }\n";
    const char* vs_src =
        "#version 460 core\n"
        "void main(){ gl_Position = vec4(0); }\n";
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vs_src, nullptr); glCompileShader(vs);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fs_src, nullptr); glCompileShader(fs);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs); glAttachShader(prog, fs); glLinkProgram(prog);
    GLint status = 0; glGetProgramiv(prog, GL_LINK_STATUS, &status);
    REQUIRE(status);

    // Fake slot 0 as valid so bind_all sees it.
    std::string err;
    tileset::GTexHeader hdr;
    hdr.tile_size_m = 1; hdr.texels_per_meter = 4;
    hdr.atlas_tiles_x = 4; hdr.atlas_tiles_y = 4;
    hdr.height_min = 0; hdr.height_max = 1; hdr.content_hash = 0x1;
    const int W = 16, H = 16;
    std::vector<uint8_t> a(W*H*3,0), n2(W*H*2,0), o(W*H*3,0);
    std::vector<uint16_t> hh(W*H,0);
    const std::string path = "/tmp/tileset_provider_bind_fixture.gtex";
    tileset::save_gtex(path, hdr, W, H, a.data(), n2.data(), o.data(), hh.data(), err);
    REQUIRE(viewer::tileset_provider::load_slot(0, path, err));

    glUseProgram(prog);
    viewer::tileset_provider::bind_all_to_shader(prog);

    GLint u_albedo = glGetUniformLocation(prog, "groundAlbedo0");
    GLint u_normal = glGetUniformLocation(prog, "groundNormal0");
    GLint u_orm    = glGetUniformLocation(prog, "groundORM0");
    GLint u_height = glGetUniformLocation(prog, "groundHeight0");
    REQUIRE(u_albedo >= 0);
    REQUIRE(u_normal >= 0);
    REQUIRE(u_orm    >= 0);
    REQUIRE(u_height >= 0);

    GLint v_albedo = -1, v_normal = -1, v_orm = -1, v_height = -1;
    glGetUniformiv(prog, u_albedo, &v_albedo);
    glGetUniformiv(prog, u_normal, &v_normal);
    glGetUniformiv(prog, u_orm,    &v_orm);
    glGetUniformiv(prog, u_height, &v_height);
    // Slot 0 uses texture units 10,11,12,13 (per bind_all_to_shader spec).
    REQUIRE(v_albedo == 10);
    REQUIRE(v_normal == 11);
    REQUIRE(v_orm    == 12);
    REQUIRE(v_height == 13);

    viewer::tileset_provider::unload_all();
    glDeleteProgram(prog);
    glDeleteShader(vs); glDeleteShader(fs);
    std::remove(path.c_str());
}

int main() {
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(64, 64, "tileset_provider_tests");
    std::string why;
    if (!viewer::gl46_available(why)) {
        std::fprintf(stderr, "SKIP: %s; set GALLIUM_DRIVER=d3d12 on WSLg\n", why.c_str());
        CloseWindow();
        return 0;
    }
    test_load_slot_uploads_mipped_textures();
    test_bind_all_sets_uniforms();
    CloseWindow();
    std::fprintf(stderr, "tileset_provider_tests: %d run, %d failed\n", g_tests, g_failures);
    return g_failures == 0 ? 0 : 1;
}
