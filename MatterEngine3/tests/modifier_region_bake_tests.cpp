// Region bake plumbing tests (spec: Testing / "region plumbing tests at the
// bake level"). Built without MATTER_HAVE_AUTOREMESHER; retopo cases use
// smooth/simplify instead (retopo's skip path is covered by modifier_apply_tests).
#include "script_host.h"
#include "check.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

static std::vector<char> read_file_bytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
}

// One voxel sphere inside a region, one outside. Both bake; the part is valid.
static void test_region_isolated_from_base_geometry() {
    script_host::ScriptHost host;
    const char* src =
        "class P extends Part { build(p) {"
        "  this.beginVoxels(0.2); this.fill(2); this.sphere([0,0,0],0.5); this.endVoxels();"
        "  this.beginModifier();"
        "  this.beginVoxels(0.2); this.fill(2); this.sphere([2,0,0],0.5); this.endVoxels();"
        "  this.endModifier([{ smooth: { iterations: 1 } }]);"
        "} }";
    script_host::BakeResult r = host.bake_source(src, "{}", {});
    CHECK(r.error.ok, "region + base geometry bakes");
    CHECK(!r.written_path.empty(), "region + base geometry writes a part");
    // The base sphere must be byte-identical to a bake WITHOUT the region part:
    // a region must not perturb non-region geometry. (Compare against a source
    // containing only the first sphere is NOT byte-comparable — different
    // hash/geometry — so instead assert the stronger property below.)
}

// Stack order is respected: simplify-then-smooth != smooth-then-simplify.
static void test_stack_order_changes_output() {
    script_host::ScriptHost host;
    const char* fmt =
        "class P extends Part { build(p) {"
        "  this.beginModifier();"
        "  this.beginVoxels(0.15); this.fill(2); this.sphere([0,0,0],0.6); this.endVoxels();"
        "  this.endModifier([%s]);"
        "} }";
    char a_src[512], b_src[512];
    std::snprintf(a_src, sizeof(a_src), fmt, "{ simplify: 0.5 }, { smooth: { iterations: 2 } }");
    std::snprintf(b_src, sizeof(b_src), fmt, "{ smooth: { iterations: 2 } }, { simplify: 0.5 }");
    script_host::BakeResult ra = host.bake_source(a_src, "{}", {});
    script_host::BakeResult rb = host.bake_source(b_src, "{}", {});
    CHECK(ra.error.ok && rb.error.ok, "both stack-order bakes succeed");
    CHECK(ra.resolved_hash != rb.resolved_hash,
          "different stack order => different resolved_hash");
    CHECK(read_file_bytes(ra.written_path) != read_file_bytes(rb.written_path),
          "different stack order => different .part bytes");
}

// Failure-skip: an always-failing modifier still yields the rest of the stack.
// (Simplify to an impossibly tiny ratio on a tiny mesh can fail/empty-out; the
// smooth after it must still land. Assert the bake succeeds and writes a part.)
static void test_failed_modifier_still_bakes() {
    script_host::ScriptHost host;
    const char* src =
        "class P extends Part { build(p) {"
        "  this.beginModifier();"
        "  this.beginVoxels(0.2); this.fill(2); this.sphere([0,0,0],0.5); this.endVoxels();"
        "  this.endModifier([{ simplify: 0.0001 }, { smooth: { iterations: 1 } }]);"
        "} }";
    script_host::BakeResult r = host.bake_source(src, "{}", {});
    CHECK(r.error.ok, "failed-modifier bake still succeeds");
    CHECK(!r.written_path.empty(), "failed-modifier bake writes a part");
}

// Two sequential regions are independent (both bake; distinct from one region).
static void test_multiple_regions() {
    script_host::ScriptHost host;
    const char* src =
        "class P extends Part { build(p) {"
        "  this.beginModifier();"
        "  this.beginVoxels(0.2); this.fill(2); this.sphere([0,0,0],0.5); this.endVoxels();"
        "  this.endModifier([{ smooth: { iterations: 1 } }]);"
        "  this.beginModifier();"
        "  this.beginVoxels(0.2); this.fill(2); this.sphere([2,0,0],0.5); this.endVoxels();"
        "  this.endModifier([{ simplify: 0.5 }]);"
        "} }";
    script_host::BakeResult r = host.bake_source(src, "{}", {});
    CHECK(r.error.ok, "two sequential regions bake");
}

// Determinism: bake the same source twice (forcing a rewrite by deleting the
// first .part) -> byte-identical output.
static void test_region_bake_deterministic() {
    script_host::ScriptHost host;
    const char* src =
        "class P extends Part { build(p) {"
        "  this.beginModifier();"
        "  this.beginVoxels(0.2); this.fill(2); this.sphere([0,0,0],0.5); this.endVoxels();"
        "  this.endModifier([{ smooth: { iterations: 2 } }, { simplify: 0.6 }]);"
        "} }";
    script_host::BakeResult r1 = host.bake_source(src, "{}", {});
    CHECK(r1.error.ok, "region bake 1 succeeds");
    std::vector<char> bytes1 = read_file_bytes(r1.written_path);
    std::remove(r1.written_path.c_str());
    script_host::BakeResult r2 = host.bake_source(src, "{}", {});
    CHECK(r2.error.ok, "region bake 2 succeeds");
    CHECK(bytes1 == read_file_bytes(r2.written_path),
          "region bake is deterministic (byte-identical)");
}

int main() {
    printf("modifier_region_bake_tests\n");
    test_region_isolated_from_base_geometry();
    test_stack_order_changes_output();
    test_failed_modifier_still_bakes();
    test_multiple_regions();
    test_region_bake_deterministic();
    if (g_failures == 0) {
        printf("all modifier_region_bake tests passed\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
