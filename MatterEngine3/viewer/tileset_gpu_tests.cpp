// tileset_gpu_tests.cpp — headless GL tests for the tileset bake pass.
//
// Pattern mirrors gpu_cull_tests.cpp: FLAG_WINDOW_HIDDEN, gl46_available
// SKIP on WSLg-without-d3d12, and CloseWindow before return.

#include "raylib.h"
#include "gl46.h"
#include "tileset_gl_ctx.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int g_tests = 0, g_failures = 0;
#define REQUIRE(cond) do { \
    ++g_tests; \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
    } while (0)

// -----------------------------------------------------------------------------
// Task 3 tests
// -----------------------------------------------------------------------------
static void test_gl_init() {
    std::string err;
    // We're already inside InitWindow at this point.
    bool ok = tileset::tileset_gl_init(err);
    REQUIRE(ok);
    if (!ok) std::fprintf(stderr, "  err: %s\n", err.c_str());
}

static void test_trivial_compute_ssbo() {
    // Compile a compute shader that writes gid to an SSBO; dispatch 64; read back.
    const char* src = R"(#version 460 core
layout(local_size_x = 64) in;
layout(std430, binding = 0) buffer B { uint data[]; };
void main() { data[gl_GlobalInvocationID.x] = gl_GlobalInvocationID.x * 3u; }
)";
    std::string err;
    GLuint prog = tileset::compile_compute_program(src, err);
    REQUIRE(prog != 0);
    if (!prog) { std::fprintf(stderr, "  compile err: %s\n", err.c_str()); return; }

    GLuint ssbo = 0;
    glGenBuffers(1, &ssbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
    std::vector<uint32_t> zero(64, 0);
    glBufferData(GL_SHADER_STORAGE_BUFFER, zero.size()*4, zero.data(), GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo);

    glUseProgram(prog);
    glDispatchCompute(1, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    std::vector<uint32_t> out(64, 0xFFFFFFFFu);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, out.size()*4, out.data());
    bool ok = true;
    for (uint32_t i = 0; i < 64; ++i) if (out[i] != i * 3u) { ok = false; break; }
    REQUIRE(ok);

    glDeleteBuffers(1, &ssbo);
    glDeleteProgram(prog);
}

static void test_include_expansion() {
    // Fixture: a tiny .comp string that #include "trivial_include.glsl" resolves
    // by reading from a real temp file next to the source. Use /tmp for portability.
    const std::string inc_dir = "/tmp";
    const std::string inc_path = inc_dir + "/trivial_include.glsl";
    FILE* f = std::fopen(inc_path.c_str(), "wb");
    REQUIRE(f != nullptr);
    const char* inc = "const uint MAGIC = 0xABCDu;\n";
    std::fwrite(inc, 1, std::strlen(inc), f); std::fclose(f);

    const std::string primary_path = "/tmp/tileset_test_primary.comp";
    FILE* pf = std::fopen(primary_path.c_str(), "wb");
    REQUIRE(pf != nullptr);
    const char* body =
        "#version 460 core\n"
        "layout(local_size_x = 1) in;\n"
        "#include \"trivial_include.glsl\"\n"
        "layout(std430, binding = 0) buffer B { uint data[]; };\n"
        "void main() { data[0] = MAGIC; }\n";
    std::fwrite(body, 1, std::strlen(body), pf); std::fclose(pf);

    std::string src, err;
    bool ok = tileset::load_compute_source(primary_path, inc_dir, src, err);
    REQUIRE(ok);
    if (!ok) { std::fprintf(stderr, "  err: %s\n", err.c_str()); return; }
    REQUIRE(src.find("MAGIC = 0xABCDu") != std::string::npos);
    REQUIRE(src.find("#include") == std::string::npos);

    GLuint prog = tileset::compile_compute_program(src, err);
    REQUIRE(prog != 0);
    if (prog) glDeleteProgram(prog);

    std::remove(inc_path.c_str());
    std::remove(primary_path.c_str());
}

int main() {
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(320, 200, "tileset_gpu_tests");

    std::string why;
    if (!viewer::gl46_available(why)) {
        std::printf("SKIP: GL 4.6 unavailable (%s); set GALLIUM_DRIVER=d3d12 on WSLg.\n",
                    why.c_str());
        CloseWindow();
        return 0;
    }
    std::printf("GL 4.6 available - running tileset GPU tests.\n");

    test_gl_init();
    test_trivial_compute_ssbo();
    test_include_expansion();

    CloseWindow();

    std::printf("\n--- Results: %d/%d passed", g_tests - g_failures, g_tests);
    if (g_failures == 0) std::printf(" --- ALL PASS\n");
    else                 std::printf(" --- %d FAIL\n", g_failures);
    return g_failures ? 1 : 0;
}
