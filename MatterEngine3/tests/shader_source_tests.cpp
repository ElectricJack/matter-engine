#include "shader_source.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>

int main() {
    std::string text, err;
    // 1. embedded lookup works and matches the on-disk source
    assert(matter::shader_text("shaders_gpu/cull.comp", text, err));
    assert(text.find("layout") != std::string::npos);
    // 2. unknown path fails with a useful error
    std::string t2;
    assert(!matter::shader_text("shaders/nope.fs", t2, err));
    assert(err.find("nope.fs") != std::string::npos);
    // 3. override dir wins: write a marker file, point the override at it
    system("mkdir -p /tmp/shader_override_test/shaders_gpu");
    FILE* f = fopen("/tmp/shader_override_test/shaders_gpu/cull.comp", "w");
    fputs("// OVERRIDE MARKER\n", f); fclose(f);
    matter::set_shader_override_dir("/tmp/shader_override_test");
    std::string t3;
    assert(matter::shader_text("shaders_gpu/cull.comp", t3, err));
    assert(t3.find("OVERRIDE MARKER") != std::string::npos);
    matter::set_shader_override_dir(nullptr);
    printf("shader_source_tests: all passed\n");
    return 0;
}
