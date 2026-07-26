#include "shader_source.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>

int main() {
    std::string text, err;
    // 1. embedded lookup works and matches the on-disk source.
    // shaders_gpu/cull.comp (the GL GpuCuller compute shader) is gone --
    // its only readers were deleted in Phase 5a (tech-debt.md §6) and it was
    // dropped from SHADER_LOGICAL in MatterEngine3/Makefile -- so this
    // round-trip check uses shaders/bvh_tlas_common.glsl, a still-embedded
    // fixture, instead.
    assert(matter::shader_text("shaders/bvh_tlas_common.glsl", text, err));
    assert(text.find("layout") != std::string::npos);
    // 2. unknown path fails with a useful error
    std::string t2;
    assert(!matter::shader_text("shaders/nope.fs", t2, err));
    assert(err.find("nope.fs") != std::string::npos);
    // 3. override dir wins: write a marker file, point the override at it
    system("mkdir -p /tmp/shader_override_test/shaders");
    FILE* f = fopen("/tmp/shader_override_test/shaders/bvh_tlas_common.glsl", "w");
    fputs("// OVERRIDE MARKER\n", f); fclose(f);
    matter::set_shader_override_dir("/tmp/shader_override_test");
    std::string t3;
    assert(matter::shader_text("shaders/bvh_tlas_common.glsl", t3, err));
    assert(t3.find("OVERRIDE MARKER") != std::string::npos);
    matter::set_shader_override_dir(nullptr);
    printf("shader_source_tests: all passed\n");
    return 0;
}
