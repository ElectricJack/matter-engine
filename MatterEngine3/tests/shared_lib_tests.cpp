#include "../include/module_resolver.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)

// ---- Task 1: import-specifier parsing -------------------------------------
static void test_parse_imports() {
    const std::string src =
        "import { lsystem } from 'shared-lib/lsystem';\n"
        "import {rng} from \"shared-lib/rng\";\n"
        "import * as V from 'shared-lib/vecmath.js';\n"
        "// import { fake } from 'shared-lib/not-real';  (commented out)\n"
        "const s = \"import { str } from 'shared-lib/string-literal'\";\n"
        "class Foo extends Part { build(p){} }\n";
    std::vector<std::string> specs = module_resolver::parse_import_specifiers(src);
    std::sort(specs.begin(), specs.end());
    CHECK(specs.size() == 3, "exactly three real import specifiers parsed");
    CHECK(specs.size() == 3 && specs[0] == "shared-lib/lsystem", "specifier 0 = lsystem");
    CHECK(specs.size() == 3 && specs[1] == "shared-lib/rng", "specifier 1 = rng");
    CHECK(specs.size() == 3 && specs[2] == "shared-lib/vecmath.js", "specifier 2 keeps .js as written");
}

// ---- Task 2: specifier resolution -----------------------------------------
static void test_resolve_specifier() {
    const std::string root = "shared-lib-fixtures";
    std::string path, err;
    CHECK(module_resolver::resolve_specifier("shared-lib/aaa", root, path, err),
          "resolve shared-lib/aaa");
    CHECK(path == "shared-lib-fixtures/aaa.js", "aaa resolves to aaa.js under root");
    CHECK(module_resolver::resolve_specifier("shared-lib/bbb.js", root, path, err),
          "trailing .js accepted");
    CHECK(path == "shared-lib-fixtures/bbb.js", "bbb.js resolves to bbb.js");
    // missing file -> fail closed
    CHECK(!module_resolver::resolve_specifier("shared-lib/nope", root, path, err),
          "missing module fails closed");
    // non-shared-lib specifier rejected
    CHECK(!module_resolver::resolve_specifier("./relative", root, path, err),
          "relative specifier rejected");
    CHECK(!module_resolver::resolve_specifier("shared-lib/../escape", root, path, err),
          "path traversal rejected");
}

// ---- Task 3: transitive gather + canonical fold ---------------------------
static void test_fold_transitive_and_canonical() {
    const std::string root = "shared-lib-fixtures";
    // A part that imports top (which transitively pulls mid -> leaf) and bbb.
    const std::string part =
        "import { TOP } from 'shared-lib/top';\n"
        "import { BBB } from 'shared-lib/bbb';\n"
        "class P extends Part { build(p){} }\n";
    std::string err;
    module_resolver::FoldResult r1;
    CHECK(module_resolver::fold_sources(part, root, r1, err), "fold succeeds");
    // resolved modules = bbb, leaf, mid, top  (transitive, deduped)
    CHECK(r1.resolved_specifiers.size() == 4, "four transitive modules gathered");
    // canonical order: part source first, then modules by sorted resolved specifier.
    // bytes must start with the part source.
    CHECK(r1.folded.size() > part.size(), "folded buffer larger than part alone");
    CHECK(std::equal(part.begin(), part.end(), r1.folded.begin()), "part source folded first");

    // Order independence: same imports listed in a different order -> identical fold.
    const std::string part_reordered =
        "import { BBB } from 'shared-lib/bbb';\n"
        "import { TOP } from 'shared-lib/top';\n"
        "class P extends Part { build(p){} }\n";
    module_resolver::FoldResult r2;
    CHECK(module_resolver::fold_sources(part_reordered, root, r2, err), "fold (reordered) succeeds");
    // The MODULE portion of the fold (everything after the part source) is identical,
    // because modules are ordered by sorted resolved specifier, not import order.
    std::string mods1(r1.folded.begin() + part.size(), r1.folded.end());
    std::string mods2(r2.folded.begin() + part_reordered.size(), r2.folded.end());
    CHECK(mods1 == mods2, "module fold is import-order independent (canonical)");

    // Cycle / missing -> fail closed.
    module_resolver::FoldResult rbad;
    const std::string bad = "import { X } from 'shared-lib/nope';\n";
    CHECK(!module_resolver::fold_sources(bad, root, rbad, err), "missing module fails fold");
}

int main() {
    test_parse_imports();
    test_resolve_specifier();
    test_fold_transitive_and_canonical();
    if (failures == 0) printf("All shared_lib tests passed\n");
    return failures == 0 ? 0 : 1;
}
